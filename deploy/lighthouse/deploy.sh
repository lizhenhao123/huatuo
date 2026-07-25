#!/usr/bin/env bash
#
# Copyright 2026 The HuaTuo Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

if [[ $# -ne 1 ]]; then
	echo "usage: $0 RELEASE_DIRECTORY" >&2
	exit 2
fi

release_dir=$(readlink -f "$1")
deploy_root=${HUATUO_DEPLOY_ROOT:-/opt/huatuo}
root_env_file="${deploy_root}/.env"
compose_file="${release_dir}/deploy/lighthouse/compose.yaml"
runtime_dir="${release_dir}/deploy/lighthouse/runtime"

if [[ ! -f "${root_env_file}" ]]; then
	echo "missing deployment environment: ${root_env_file}" >&2
	exit 1
fi
if [[ ! -f "${compose_file}" ]]; then
	echo "missing deployment compose file: ${compose_file}" >&2
	exit 1
fi

set -a
# shellcheck disable=SC1090
source "${root_env_file}"
set +a

require_hex_secret() {
	local name=$1
	local value=${!name:-}
	if [[ ! "${value}" =~ ^[[:xdigit:]]{64,128}$ ]]; then
		echo "${name} must contain 64 to 128 hexadecimal characters" >&2
		exit 1
	fi
}

require_hex_secret ELASTIC_PASSWORD
require_hex_secret GRAFANA_ADMIN_PASSWORD
require_hex_secret API_ADMIN_TOKEN

if [[ ! "${HUATUO_IMAGE:-}" =~ ^[a-z0-9./_-]+$ ]]; then
	echo "HUATUO_IMAGE contains unsupported characters" >&2
	exit 1
fi
if [[ ! "${HUATUO_IMAGE_TAG:-}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
	echo "HUATUO_IMAGE_TAG contains unsupported characters" >&2
	exit 1
fi

install -d -m 0755 "${deploy_root}" "${deploy_root}/releases"
install -d -m 0700 "${runtime_dir}"
release_env_file="${runtime_dir}/deploy.env"
install -m 0600 "${root_env_file}" "${release_env_file}"

render_config() {
	local source=$1
	local destination=$2
	local content
	content=$(< "${source}")
	content=${content//__ELASTIC_PASSWORD__/${ELASTIC_PASSWORD}}
	content=${content//__API_ADMIN_TOKEN__/${API_ADMIN_TOKEN}}
	(umask 077 && printf '%s\n' "${content}" > "${destination}")
}

render_config \
	"${release_dir}/deploy/lighthouse/huatuo-apiserver.conf.tmpl" \
	"${runtime_dir}/huatuo-apiserver.conf"
render_config \
	"${release_dir}/deploy/lighthouse/huatuo-bamai.conf.tmpl" \
	"${runtime_dir}/huatuo-bamai.conf"

compose=(
	docker compose
	--project-name huatuo
	--env-file "${release_env_file}"
	--file "${compose_file}"
)

"${compose[@]}" config --quiet
"${compose[@]}" pull

previous_release=""
if [[ -L "${deploy_root}/current" ]]; then
	previous_release=$(readlink -f "${deploy_root}/current")
fi

rollback() {
	local exit_code=$?
	if [[ -n "${previous_release}" &&
		-f "${previous_release}/deploy/lighthouse/compose.yaml" &&
		-f "${previous_release}/deploy/lighthouse/runtime/deploy.env" ]]; then
		echo "deployment failed; restoring ${previous_release}" >&2
		docker compose \
			--project-name huatuo \
			--env-file \
			"${previous_release}/deploy/lighthouse/runtime/deploy.env" \
			--file "${previous_release}/deploy/lighthouse/compose.yaml" \
			up --detach --remove-orphans --wait --wait-timeout 180 || true
	else
		echo "deployment failed; stopping incomplete first release" >&2
		"${compose[@]}" down || true
	fi
	exit "${exit_code}"
}
trap rollback ERR

"${compose[@]}" up --detach --remove-orphans --wait --wait-timeout 180
ln -sfn "${release_dir}" "${deploy_root}/current.next"
mv -Tf "${deploy_root}/current.next" "${deploy_root}/current"
trap - ERR

"${compose[@]}" ps
echo "deployed ${release_dir}"
