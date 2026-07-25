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

if [[ $# -ne 2 ]]; then
	echo "usage: $0 REVISION IMAGE_TAG" >&2
	exit 2
fi

revision=$1
image_tag=$2
repository=${HUATUO_GITHUB_REPOSITORY:-ccfos/huatuo}
deploy_root=${HUATUO_DEPLOY_ROOT:-/opt/huatuo}
release_dir="${deploy_root}/releases/${revision}"
archive=$(mktemp)
staging=$(mktemp -d)
env_staging=$(mktemp)
trap 'rm -f "${archive}" "${env_staging}"; rm -rf "${staging}"' EXIT

if [[ ! "${revision}" =~ ^[[:xdigit:]]{40}$ ]]; then
	echo "revision must be a full 40-character commit hash" >&2
	exit 1
fi
if [[ ! "${image_tag}" =~ ^[A-Za-z0-9_.-]+$ ]]; then
	echo "image tag contains unsupported characters" >&2
	exit 1
fi
if [[ ! "${repository}" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
	echo "GitHub repository contains unsupported characters" >&2
	exit 1
fi

curl -fsSL --retry 5 \
	"https://github.com/${repository}/archive/${revision}.tar.gz" \
	-o "${archive}"
tar -xzf "${archive}" --strip-components=1 -C "${staging}"
test -x "${staging}/deploy/lighthouse/deploy.sh"

sudo install -d -m 0755 "${deploy_root}/releases"
if [[ ! -f "${deploy_root}/.env" ]]; then
	echo "missing ${deploy_root}/.env" >&2
	exit 1
fi
sudo awk -v tag="${image_tag}" '
	BEGIN { found = 0 }
	/^HUATUO_IMAGE_TAG=/ {
		print "HUATUO_IMAGE_TAG=" tag
		found = 1
		next
	}
	{ print }
	END {
		if (!found) {
			print "HUATUO_IMAGE_TAG=" tag
		}
	}
' "${deploy_root}/.env" > "${env_staging}"
sudo install -m 0600 "${env_staging}" "${deploy_root}/.env"

if [[ ! -d "${release_dir}" ]]; then
	sudo install -d -m 0755 "${release_dir}"
	sudo cp -a "${staging}/." "${release_dir}/"
elif [[ ! -x "${release_dir}/deploy/lighthouse/deploy.sh" ]]; then
	echo "immutable release directory is incomplete: ${release_dir}" >&2
	exit 1
fi

sudo "${release_dir}/deploy/lighthouse/deploy.sh" "${release_dir}"
