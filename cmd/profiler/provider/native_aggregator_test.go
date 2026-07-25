// Copyright 2026 The HuaTuo Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package provider

import (
	"testing"

	"huatuo-bamai/internal/profiler"
	pcontext "huatuo-bamai/internal/profiler/context"
	"huatuo-bamai/pkg/profiling"
)

func TestNativeAggregatorAggregatesLockTime(t *testing.T) {
	aggregator := &nativeAggregator{
		aggrMap:     map[string]*stackEntry{},
		lockAggrMap: map[string]*lockStackEntry{},
	}
	record := &lockStackEntry{
		Proc:      &processIDNameLock{Pid: 12, Name: "app", Lock: 0xab},
		User:      "foo;bar",
		WaitTime:  10,
		Contended: 2,
		LockType:  profiling.LockTypeMutex,
	}

	aggregator.Aggregate(record)
	aggregator.Aggregate(record)

	requireSingleLockRecord(t, aggregator, 20, 4)
	_, sampleType, err := profileTypeOptions(&pcontext.ProfilerContext{Type: profiling.TypeLock})
	if err != nil {
		t.Fatalf("profileTypeOptions() error = %v", err)
	}
	if sampleType != profiler.ProfileTypeLockTimeSample {
		t.Fatalf("sample type = %q, want %q", sampleType, profiler.ProfileTypeLockTimeSample)
	}
}

func TestNativeAggregatorKeepsDistinctLockMetadata(t *testing.T) {
	aggregator := &nativeAggregator{
		aggrMap:     map[string]*stackEntry{},
		lockAggrMap: map[string]*lockStackEntry{},
	}
	base := &lockStackEntry{
		Proc:      &processIDNameLock{Pid: 12, Name: "app", Lock: 0xab},
		User:      "foo;bar",
		Kernel:    "mutex_lock",
		WaitTime:  10,
		Contended: 1,
		LockType:  profiling.LockTypeMutex,
	}
	differentPID := *base
	differentPID.Proc = &processIDNameLock{
		Pid:  13,
		Name: "app",
		Lock: 0xab,
	}
	differentKernel := *base
	differentKernel.Kernel = "mutex_lock_nested"
	differentType := *base
	differentType.LockType = profiling.LockTypeSpinlock

	aggregator.Aggregate(base)
	aggregator.Aggregate(&differentPID)
	aggregator.Aggregate(&differentKernel)
	aggregator.Aggregate(&differentType)

	if len(aggregator.lockAggrMap) != 4 {
		t.Fatalf(
			"lock records = %d, want 4 distinct metadata groups",
			len(aggregator.lockAggrMap),
		)
	}
}

func TestLockPrefixFramesIncludeLockType(t *testing.T) {
	frames, value := lockPrefixFrames(&lockStackEntry{
		Proc: &processIDNameLock{
			Pid:  12,
			Name: "app",
			Lock: 0xab,
		},
		WaitTime:  15,
		Contended: 2,
		LockType:  profiling.LockTypeSpinlock,
	})

	if len(frames) == 0 || frames[0] != "lock type: spinlock" {
		t.Fatalf("lock type frame = %v, want spinlock", frames)
	}
	if value != 15 {
		t.Fatalf("lock value = %d, want 15", value)
	}
}

func requireSingleLockRecord(
	t *testing.T,
	aggregator *nativeAggregator,
	waitTime, contended uint64,
) {
	t.Helper()
	if len(aggregator.lockAggrMap) != 1 {
		t.Fatalf("lock records = %d, want 1", len(aggregator.lockAggrMap))
	}
	for _, record := range aggregator.lockAggrMap {
		if record.WaitTime != waitTime || record.Contended != contended {
			t.Fatalf(
				"lock record = (wait=%d, contended=%d), want (wait=%d, contended=%d)",
				record.WaitTime,
				record.Contended,
				waitTime,
				contended,
			)
		}
	}
}
