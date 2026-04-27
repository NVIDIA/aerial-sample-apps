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
    echo "Caught signal, stopping services..."
    if [ ! -z "$DAPP_PID" ]; then
        kill -SIGTERM $DAPP_PID 2>/dev/null
    fi
    wait $DAPP_PID 2>/dev/null
    echo "Cleanup complete."
    exit 0
}

trap cleanup SIGTERM SIGINT

# Check GPU availability
if nvidia-smi &>/dev/null; then
    echo "GPU available:"
    nvidia-smi -L
else
    echo "WARNING: No GPU detected, running in CPU mode"
fi

# Build dApp command
DAPP_CMD="/opt/dapp/bin/prb_power_python_dapp --config \"$CONFIG_FILE\""

# Read debug setting from config (using Python since jq may not be installed)
DEBUG_ENABLED=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE')).get('e3_manager',{}).get('debug_enabled', False))" 2>/dev/null || echo "false")
if [ "$DEBUG_ENABLED" = "True" ] || [ "$DEBUG_ENABLED" = "true" ]; then
    DAPP_CMD="$DAPP_CMD --debug"
fi

# Read log directory from config
LOG_DIR=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE')).get('logging',{}).get('directory', '/logs'))" 2>/dev/null || echo "/logs")
E3_LOG=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE')).get('logging',{}).get('e3_manager_log', 'e3_manager.log'))" 2>/dev/null || echo "e3_manager.log")

mkdir -p "$LOG_DIR"

# Start PRB Power dApp
echo "Starting PRB Power dApp (Python)..."
echo "Command: $DAPP_CMD"
eval "$DAPP_CMD" 2>&1 | tee "$LOG_DIR/$E3_LOG" &
DAPP_PID=$!
echo "PRB Power dApp started with PID: $DAPP_PID"

echo "Service started. Monitoring... (press Ctrl+C to stop)"

# Wait for the dApp process
wait $DAPP_PID
