#!/bin/bash

#
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
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
#

usage() {
    cat <<'EOF'
Usage: ./restart_script.sh [-b|--build] [-c|--config PATH]

  -b, --build         Rebuild the image (auto-enabled if the image does not exist yet).
                      Needed after C++ or source changes.
  -c, --config PATH   Config file; a bare name resolves under config/.
                      Default: config/e3_config.json
  -h, --help          Show this help.
EOF
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

BUILD=0
CONFIG_FILE="config/e3_config.json"
while [ $# -gt 0 ]; do
    case "$1" in
        -b|--build)  BUILD=1; shift;;
        -c|--config) case "${2-}" in ""|-*) echo "Missing value for $1" >&2; usage; exit 1;; esac; CONFIG_FILE="$2"; shift 2;;
        -h|--help)   usage; exit 0;;
        *) echo "Unknown option: $1" >&2; usage; exit 1;;
    esac
done
case "$CONFIG_FILE" in */*) ;; *) CONFIG_FILE="config/$CONFIG_FILE";; esac
if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: config not found: $CONFIG_FILE" >&2
    exit 1
fi
case "$(realpath "$CONFIG_FILE")" in "$(realpath config)"/*) ;; *)
    echo "ERROR: --config must live under config/: $CONFIG_FILE" >&2
    exit 1;;
esac

echo "=== Restart Script for PRB Power dApp (Triton) ==="

CONTAINER_PREFIX="/opt/src/applications/prb-power-triton"
export E3_CONFIG="$CONTAINER_PREFIX/$CONFIG_FILE"

IPC_MODE=$(jq -r '.deployment.ipc_mode // "container:nv-cubb"' "$CONFIG_FILE")
echo "IPC mode: $IPC_MODE"
export IPC_MODE

IMAGE="dapp-prb-power-triton:latest"
if [ "$BUILD" -eq 0 ] && ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "Image $IMAGE not found; building it this time."
    BUILD=1
fi
echo "Config: $CONFIG_FILE (build=$BUILD)"

# Stop and remove existing container
echo "Removing existing container..."
docker compose down --remove-orphans 2>/dev/null || true

if [ "$BUILD" -eq 1 ]; then
    # Delete existing image
    echo "Deleting existing image..."
    docker image rm "$IMAGE" 2>/dev/null || true

    # Build new image
    echo "Building new image..."
    docker compose build
fi

# Start container
echo "Starting PRB Power Triton container..."
docker compose up

# Stop container
echo "Stopping PRB Power Triton container..."
docker compose down

echo "=== Restart Script Complete ==="
