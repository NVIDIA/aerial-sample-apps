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
CONFIG_FILE="${E3_CONFIG:-/opt/src/applications/prb-power-triton-grpc/config/e3_config.json}"

echo "=== Starting PRB Power dApp (Triton gRPC) ==="
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
TRITON_MODEL_REPO=$(jq -r '.triton.model_repository' "$CONFIG_FILE")
TRITON_HTTP_PORT=$(jq -r '.triton.http_port' "$CONFIG_FILE")
TRITON_GRPC_PORT=$(jq -r '.triton.grpc_port' "$CONFIG_FILE")
TRITON_METRICS_PORT=$(jq -r '.triton.metrics_port' "$CONFIG_FILE")
TRITON_STARTUP_TIMEOUT=$(jq -r '.triton.startup_timeout_seconds' "$CONFIG_FILE")

E3_DEBUG_ENABLED=$(jq -r '.e3_manager.debug_enabled' "$CONFIG_FILE")

LOG_DIR=$(jq -r '.logging.directory' "$CONFIG_FILE")
TRITON_LOG=$(jq -r '.logging.triton_log' "$CONFIG_FILE")
E3_MANAGER_LOG=$(jq -r '.logging.e3_manager_log' "$CONFIG_FILE")

TRITON_READY_TIMEOUT=$(jq -r '.triton.ready_timeout_seconds' "$CONFIG_FILE")
RETRY_INTERVAL=$(jq -r '.triton.startup_retry_interval_seconds' "$CONFIG_FILE")

VIS_LOG=$(jq -r '.logging.visualizer_log // "visualizer.log"' "$CONFIG_FILE")
VIS_ENABLED=$(jq -r '.application.visualizer.enabled // false' "$CONFIG_FILE")
VIS_PORT=$(jq -r '.application.visualizer.web_port // 5001' "$CONFIG_FILE")
RESULTS_PUB_PORT=$(jq -r '.application.results_pub_port // 5559' "$CONFIG_FILE")
RESULTS_PUB_ENABLED=$(jq -r 'if .application.enable_results_publishing == null then "true" else (.application.enable_results_publishing | tostring) end' "$CONFIG_FILE")

echo "Model repository: $TRITON_MODEL_REPO"
echo "E3 Manager config: $CONFIG_FILE"
echo "Debug mode: $E3_DEBUG_ENABLED"

# Function to cleanup on exit
cleanup() {
    rc=$?
    [ -n "$CLEANED" ] && return; CLEANED=1
    echo "Caught signal, stopping services..."
    rm -f /dev/shm/triton_python_backend_shm_region_* 2>/dev/null || true
    [ -n "$VIS_PID" ]    && kill -SIGTERM "$VIS_PID"    2>/dev/null || true
    [ -n "$TRITON_PID" ] && kill -SIGTERM "$TRITON_PID" 2>/dev/null || true
    [ -n "$E3_PID" ]     && kill -SIGTERM "$E3_PID"     2>/dev/null || true
    [ -n "$VIS_PID" ] && wait "$VIS_PID" 2>/dev/null || true
    wait "$TRITON_PID" 2>/dev/null || true
    wait "$E3_PID"     2>/dev/null || true
    echo "Cleanup complete."
    exit $rc
}

trap cleanup EXIT SIGTERM SIGINT

# Check if we're running with proper GPU access
if nvidia-smi &>/dev/null; then
    echo "GPU available:"
    nvidia-smi -L
else
    echo "WARNING: No GPU detected, running in CPU mode"
fi

# Prevent PyTorch from loading distributed backends that we don't need
export TORCH_DISTRIBUTED_BACKENDS=""

# Start Triton Server (separate process, communicates via gRPC/HTTP)
echo "Starting Triton Inference Server in the background..."
tritonserver --model-repository=$TRITON_MODEL_REPO \
             --model-control-mode=explicit \
             --http-port=$TRITON_HTTP_PORT \
             --grpc-port=$TRITON_GRPC_PORT \
             --metrics-port=$TRITON_METRICS_PORT \
             --log-file="$LOG_DIR/$TRITON_LOG" \
             --allow-gpu-metrics=false &
TRITON_PID=$!
echo "Triton server started with PID: $TRITON_PID"

# Wait for Triton to be ready with configurable timeout
echo "Waiting for Triton server to be ready (timeout: ${TRITON_READY_TIMEOUT}s)..."
TIMEOUT_COUNT=0
until curl -s -f -o /dev/null localhost:$TRITON_HTTP_PORT/v2/health/ready; do
    echo "Triton is not ready yet. Retrying in ${RETRY_INTERVAL} seconds..."
    sleep $RETRY_INTERVAL
    TIMEOUT_COUNT=$((TIMEOUT_COUNT + RETRY_INTERVAL))
    if [ $TIMEOUT_COUNT -ge $TRITON_READY_TIMEOUT ]; then
        echo "ERROR: Triton failed to start within ${TRITON_READY_TIMEOUT} seconds"
        exit 1
    fi
done
echo "Triton server is ready."

# Build PRB Power dApp command
DAPP_ARGS=(--config "$CONFIG_FILE")

# Start PRB Power dApp
echo "Starting PRB Power dApp with command:"
echo "/opt/dapp/bin/prb_power_dapp ${DAPP_ARGS[*]}"
/opt/dapp/bin/prb_power_dapp "${DAPP_ARGS[@]}" > >(tee "$LOG_DIR/$E3_MANAGER_LOG") 2>&1 &
E3_PID=$!
echo "PRB Power dApp started with PID: $E3_PID"

sleep 2

# Load default model (read from config)
E3_DEFAULT_MODEL=$(jq -r '.e3_manager.default_model' "$CONFIG_FILE")
echo "Loading default model: $E3_DEFAULT_MODEL..."
curl -X POST "http://localhost:$TRITON_HTTP_PORT/v2/repository/models/$E3_DEFAULT_MODEL/load"

# Start web visualizer (background, optional)
if [ "${VIS_ENABLED,,}" = "true" ]; then
    [ "${RESULTS_PUB_ENABLED,,}" != "true" ] && echo "WARNING: visualizer enabled but enable_results_publishing=false; no data feed."
    VIS_SCRIPT="/opt/src/applications/prb-power-triton-grpc/visualizer/prb_power_visualizer.py"
    : > "$LOG_DIR/$VIS_LOG"
    python3 "$VIS_SCRIPT" --port "$VIS_PORT" --zmq-port "$RESULTS_PUB_PORT" > "$LOG_DIR/$VIS_LOG" 2>&1 &
    VIS_PID=$!
    echo "Visualizer PID: $VIS_PID  (http://localhost:$VIS_PORT)"
fi

# Keep container running
echo "Services started. Monitoring... (press Ctrl+C to stop)"

# Wait indefinitely for background jobs. The trap will handle cleanup.
wait
