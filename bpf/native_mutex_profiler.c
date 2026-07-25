#include "vmlinux.h"

#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "bpf_profiler.h"

char __license[] SEC("license") = "Dual MIT/GPL";

/* Flags exported by lock:contention_begin since Linux 5.19. */
#define LCB_F_SPIN (1U << 0)
#define LCB_F_READ (1U << 1)
#define LCB_F_WRITE (1U << 2)
#define LCB_F_MUTEX (1U << 5)

static volatile const u64 lock_wait_threshold_ns = 1000;

struct mutex_wait_start_t {
	u64 started_ns;
	u64 lock;
};

struct mutex_event_t {
	struct profiler_event_base_t base;
	u64 lock;
};

struct spin_wait_start_t {
	struct mutex_event_t event;
	u64 started_ns;
	u64 transfer_count;
	u8 active;
};

/*
 * A task can migrate between CPUs while blocked. Keying in-flight waits by
 * pid_tgid in an LRU hash keeps enter/exit correlation correct across that
 * migration; a per-CPU map would lose or mismatch the start record.
 */
struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 65536);
	__type(key, u64);
	__type(value, struct mutex_wait_start_t);
} mutex_wait_starts SEC(".maps");

/*
 * Spinning contenders cannot migrate or nest sleeping lock acquisition.
 * A per-CPU slot matches perf lock's bounded correlation model and avoids a
 * global hash update in the spinlock hot path.
 */
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, u32);
	__type(value, struct spin_wait_start_t);
} spin_wait_starts SEC(".maps");

DEFINE_PROFILER_MAPS(struct mutex_event_t);

static __always_inline int mutex_wait_begin(u64 pid_tgid, u64 lock)
{
	u64 cpu_css = current_task_cpu_css_addr();
	if (!profiler_should_trace(pid_tgid, cpu_css))
		return 0;

	struct mutex_wait_start_t start = {
		.started_ns = bpf_ktime_get_ns(),
		.lock = lock,
	};
	bpf_map_update_elem(&mutex_wait_starts, &pid_tgid, &start,
			    COMPAT_BPF_ANY);
	return 0;
}

static __always_inline int mutex_wait_end(void *ctx, u64 pid_tgid, bool success)
{
	struct mutex_wait_start_t *found =
		bpf_map_lookup_elem(&mutex_wait_starts, &pid_tgid);
	if (!found)
		return 0;

	struct mutex_wait_start_t start = *found;
	bpf_map_delete_elem(&mutex_wait_starts, &pid_tgid);
	if (!success)
		return 0;

	u64 wait_ns = bpf_ktime_get_ns() - start.started_ns;
	if (wait_ns < lock_wait_threshold_ns)
		return 0;

	u64 *transfer_count_ptr;
	u64 *sample_count_ptrs[2];
	void *select_profiler_stack_map;
	void *select_profiler_output;
	u64 *select_profiler_sample_count_ptr;

	if (!profiler_init_state(&profiler_state_map, &transfer_count_ptr,
				 sample_count_ptrs))
		return 0;

	SELECT_PROFILER_AB();

	u32 idx = 0;
	struct mutex_event_t *event = bpf_map_lookup_elem(&event_buf, &idx);
	if (!event)
		return 0;

	__builtin_memset(event, 0, sizeof(*event));
	event->base.value = wait_ns;
	event->lock = start.lock;
	if (profiler_fill_event_base(&event->base, pid_tgid, ctx,
				     select_profiler_stack_map) < 0)
		return 0;

	profiler_emit_event(ctx, select_profiler_output,
			    select_profiler_sample_count_ptr, event,
			    sizeof(*event));
	return 0;
}

static __always_inline int spin_wait_begin(void *ctx, u64 lock)
{
	u64 pid_tgid = bpf_get_current_pid_tgid();
	u64 cpu_css = current_task_cpu_css_addr();
	if (!profiler_should_trace(pid_tgid, cpu_css))
		return 0;

	u64 *transfer_count_ptr;
	u64 *sample_count_ptrs[2];
	void *select_profiler_stack_map;

	if (!profiler_init_state(&profiler_state_map, &transfer_count_ptr,
				 sample_count_ptrs))
		return 0;

	u64 transfer_count = *transfer_count_ptr;
	if ((transfer_count & 1ULL) == 0)
		select_profiler_stack_map = (void *)&stack_map_a;
	else
		select_profiler_stack_map = (void *)&stack_map_b;

	u32 idx = 0;
	struct spin_wait_start_t *start =
		bpf_map_lookup_elem(&spin_wait_starts, &idx);
	if (!start)
		return 0;

	__builtin_memset(start, 0, sizeof(*start));
	start->event.lock = lock;
	start->started_ns = bpf_ktime_get_ns();
	if (profiler_fill_event_base(&start->event.base, pid_tgid, ctx,
				     select_profiler_stack_map) < 0)
		return 0;
	if (*transfer_count_ptr != transfer_count)
		return 0;

	start->transfer_count = transfer_count;
	start->active = 1;
	return 0;
}

static __always_inline int spin_wait_end(void *ctx, u64 lock, bool success)
{
	u32 idx = 0;
	struct spin_wait_start_t *start =
		bpf_map_lookup_elem(&spin_wait_starts, &idx);
	if (!start || !start->active || start->event.lock != lock)
		return 0;

	struct mutex_event_t event = start->event;
	u64 wait_ns = bpf_ktime_get_ns() - start->started_ns;
	u64 transfer_count = start->transfer_count;
	start->active = 0;
	if (!success || wait_ns < lock_wait_threshold_ns)
		return 0;

	u64 *transfer_count_ptr;
	u64 *sample_count_ptrs[2];
	if (!profiler_init_state(&profiler_state_map, &transfer_count_ptr,
				 sample_count_ptrs))
		return 0;

	/*
	 * A drain flipped the stack/output pair during this wait. Dropping this
	 * rare sample avoids publishing stack IDs into the wrong generation.
	 */
	if (*transfer_count_ptr != transfer_count)
		return 0;

	void *select_profiler_output;
	u64 *select_profiler_sample_count_ptr;
	if ((transfer_count & 1ULL) == 0) {
		select_profiler_output = (void *)&profiler_output_a;
		select_profiler_sample_count_ptr = sample_count_ptrs[0];
	} else {
		select_profiler_output = (void *)&profiler_output_b;
		select_profiler_sample_count_ptr = sample_count_ptrs[1];
	}

	event.base.value = wait_ns;
	profiler_emit_event(ctx, select_profiler_output,
			    select_profiler_sample_count_ptr, &event,
			    sizeof(event));
	return 0;
}

SEC("kprobe/huatuo_mutex_lock_slowpath")
int trace_mutex_lock_slowpath(struct pt_regs *ctx)
{
	return mutex_wait_begin(bpf_get_current_pid_tgid(), PT_REGS_PARM1(ctx));
}

SEC("kretprobe/huatuo_mutex_lock_slowpath")
int trace_mutex_lock_slowpath_return(struct pt_regs *ctx)
{
	return mutex_wait_end(ctx, bpf_get_current_pid_tgid(), true);
}

struct lock_contention_begin_ctx {
	u64 common;
	u64 lock_addr;
	u32 flags;
};

struct lock_contention_end_ctx {
	u64 common;
	u64 lock_addr;
	s32 ret;
};

SEC("tracepoint/lock/contention_begin")
int trace_mutex_contention_begin(struct lock_contention_begin_ctx *ctx)
{
	if (!(ctx->flags & LCB_F_MUTEX))
		return 0;
	return mutex_wait_begin(bpf_get_current_pid_tgid(), ctx->lock_addr);
}

SEC("tracepoint/lock/contention_end")
int trace_mutex_contention_end(struct lock_contention_end_ctx *ctx)
{
	return mutex_wait_end(ctx, bpf_get_current_pid_tgid(), ctx->ret == 0);
}

SEC("tracepoint/lock/contention_begin")
int trace_spin_contention_begin(struct lock_contention_begin_ctx *ctx)
{
	if (!(ctx->flags & LCB_F_SPIN) ||
	    (ctx->flags & (LCB_F_READ | LCB_F_WRITE)))
		return 0;
	return spin_wait_begin(ctx, ctx->lock_addr);
}

SEC("tracepoint/lock/contention_end")
int trace_spin_contention_end(struct lock_contention_end_ctx *ctx)
{
	return spin_wait_end(ctx, ctx->lock_addr, ctx->ret == 0);
}
