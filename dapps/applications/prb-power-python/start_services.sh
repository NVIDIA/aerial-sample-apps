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

set -e

umask 000

CONFIG_FILE="${E3_CONFIG:-/opt/src/applications/prb-power-python/config/e3_config.json}"

echo "=== Starting PRB Power dApp (Python) ==="
echo "Architecture: $(uname -m)"
echo "CUDA devices: $CUDA_VISIBLE_DEVICES"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Config file not found: $CONFIG_FILE"
    exit 1
fi

echo "Config: $CONFIG_FILE"

# Function to cleanup on exit
cleanup() {
    rc=$?
    [ -n "$CLEANED" ] && return; CLEANED=1
    echo "Caught signal, stopping services..."
    [ -n "$VIS_PID" ]  && kill -SIGTERM "$VIS_PID"  2>/dev/null || true
    [ -n "$DAPP_PID" ] && kill -SIGTERM "$DAPP_PID" 2>/dev/null || true
    [ -n "$VIS_PID" ] && wait "$VIS_PID" 2>/dev/null || true
    wait "$DAPP_PID" 2>/dev/null || true
    echo "Cleanup complete."
    exit $rc
}

trap cleanup EXIT SIGTERM SIGINT

# Check GPU availability
if nvidia-smi &>/dev/null; then
    echo "GPU available:"
    nvidia-smi -L
else
    echo "WARNING: No GPU detected, running in CPU mode"
fi

# Build dApp command
DAPP_ARGS=(--config "$CONFIG_FILE")

# Read log directory from config
LOG_DIR=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('logging',{}).get('directory', '/logs'))" "$CONFIG_FILE" 2>/dev/null || echo "/logs")
E3_LOG=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('logging',{}).get('e3_manager_log', 'e3_manager.log'))" "$CONFIG_FILE" 2>/dev/null || echo "e3_manager.log")
VIS_LOG=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('logging',{}).get('visualizer_log', 'visualizer.log'))" "$CONFIG_FILE" 2>/dev/null || echo "visualizer.log")
VIS_ENABLED=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('application',{}).get('visualizer',{}).get('enabled', False))" "$CONFIG_FILE" 2>/dev/null || echo "False")
VIS_PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('application',{}).get('visualizer',{}).get('web_port', 5001))" "$CONFIG_FILE" 2>/dev/null || echo "5001")
RESULTS_PUB_PORT=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('application',{}).get('results_pub_port', 5559))" "$CONFIG_FILE" 2>/dev/null || echo "5559")
RESULTS_PUB_ENABLED=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('application',{}).get('enable_results_publishing', True))" "$CONFIG_FILE" 2>/dev/null || echo "True")

mkdir -p "$LOG_DIR"

# Start PRB Power dApp
echo "Starting PRB Power dApp (Python)..."
echo "Command: /opt/dapp/bin/prb_power_python_dapp ${DAPP_ARGS[*]}"
/opt/dapp/bin/prb_power_python_dapp "${DAPP_ARGS[@]}" > >(tee "$LOG_DIR/$E3_LOG") 2>&1 &
DAPP_PID=$!
echo "PRB Power dApp started with PID: $DAPP_PID"

# Start web visualizer (background, optional)
if [ "${VIS_ENABLED,,}" = "true" ]; then
    [ "${RESULTS_PUB_ENABLED,,}" != "true" ] && echo "WARNING: visualizer enabled but enable_results_publishing=false; no data feed."
    VIS_SCRIPT="/opt/src/applications/prb-power-python/visualizer/prb_power_visualizer.py"
    : > "$LOG_DIR/$VIS_LOG"
    python3 "$VIS_SCRIPT" --port "$VIS_PORT" --zmq-port "$RESULTS_PUB_PORT" > "$LOG_DIR/$VIS_LOG" 2>&1 &
    VIS_PID=$!
    echo "Visualizer PID: $VIS_PID  (http://localhost:$VIS_PORT)"
fi

echo "Service started. Monitoring... (press Ctrl+C to stop)"

# Wait for the dApp process
wait $DAPP_PID
