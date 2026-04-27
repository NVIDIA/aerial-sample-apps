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

echo "=== Restart Script for PRB Power dApp (Python) ==="

# Read IPC mode from config
CONFIG_FILE="config/e3_config.json"
if [ -f "$CONFIG_FILE" ]; then
    IPC_MODE=$(python3 -c "import json; print(json.load(open('$CONFIG_FILE')).get('deployment',{}).get('ipc_mode', 'container:nv-cubb'))" 2>/dev/null || echo "container:nv-cubb")
    echo "IPC mode: $IPC_MODE"
    export IPC_MODE
else
    echo "WARNING: Config file not found, using default IPC mode"
    export IPC_MODE="container:nv-cubb"
fi

# Stop existing container
echo "Removing existing container..."
docker compose down
docker rm --force dapp-prb-power-python

# Delete existing image
echo "Deleting existing image..."
docker image rm dapp-prb-power-python:latest

# Build new image
echo "Building new image..."
docker compose build

# Start container
echo "Starting PRB Power Python container..."
docker compose up

# Stop container
echo "Stopping PRB Power Python container..."
docker compose down

echo "=== Restart Script Complete ==="
