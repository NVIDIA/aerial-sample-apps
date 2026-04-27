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

# Exit on any error
set -e

umask 000

# Configuration file path
CONFIG_FILE="${E3_CONFIG:-/opt/src/applications/prb-power-triton/config/e3_config.json}"

echo "=== Starting PRB Power dApp (Triton C API) ==="
echo "Architecture: $(uname -m)"
echo "CUDA devices: $CUDA_VISIBLE_DEVICES"

# Check if config file exists
if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Config file not found: $CONFIG_FILE"
    exit 1
fi

# Load configuration from JSON
echo "Loading configuration from $CONFIG_FILE..."

# Read configuration directly from JSON
LOG_DIR=$(jq -r '.logging.directory' "$CONFIG_FILE")
E3_MANAGER_LOG=$(jq -r '.logging.e3_manager_log' "$CONFIG_FILE")
E3_DEBUG_ENABLED=$(jq -r '.e3_manager.debug_enabled' "$CONFIG_FILE")

echo "E3 Manager config: $CONFIG_FILE"
echo "Debug mode: $E3_DEBUG_ENABLED"

# Function to cleanup on exit
cleanup() {
    echo "Caught signal, stopping..."
    if [ -n "$DAPP_PID" ]; then
        kill -SIGTERM "$DAPP_PID" 2>/dev/null
    fi
    wait "$DAPP_PID" 2>/dev/null
    echo "Cleanup complete."
    exit 0
}

trap cleanup SIGTERM SIGINT

# Check if we're running with proper GPU access
if nvidia-smi &>/dev/null; then
    echo "GPU available:"
    nvidia-smi -L
else
    echo "WARNING: No GPU detected, running in CPU mode"
fi

# Prevent PyTorch from loading distributed backends that we don't need
export TORCH_DISTRIBUTED_BACKENDS=""

SCRIPTS_DIR="/opt/src/applications/prb-power-triton/scripts"

echo "=== Generating models ==="
python3 "$SCRIPTS_DIR/create_onnx_model.py"
python3 "$SCRIPTS_DIR/create_trt_model.py"
python3 "$SCRIPTS_DIR/create_libtorch_model.py"
echo "=== Model generation complete ==="

# Build PRB Power dApp command
DAPP_CMD="/opt/dapp/bin/prb_power_dapp --config \"$CONFIG_FILE\""

if [ "$E3_DEBUG_ENABLED" = "true" ]; then
    DAPP_CMD="$DAPP_CMD --debug"
fi

# Start PRB Power dApp
echo "Starting PRB Power dApp with command:"
echo "$DAPP_CMD"
eval "$DAPP_CMD" 2>&1 | tee "$LOG_DIR/$E3_MANAGER_LOG" &
DAPP_PID=$!
echo "PRB Power dApp started with PID: $DAPP_PID"

echo "Running. Press Ctrl+C to stop."
wait "$DAPP_PID"
