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

# Start PRB Power & Inference Time Web Visualizer
#
# Usage:
#   ./start_prb_visualizer.sh                    # Start on default ports
#   ./start_prb_visualizer.sh --port 5001        # Custom web port
#   ./start_prb_visualizer.sh --zmq-port 5560    # Custom ZMQ port
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default settings
WEB_PORT=5001
ZMQ_PORT=5559

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --port)
            WEB_PORT="$2"
            shift 2
            ;;
        --zmq-port)
            ZMQ_PORT="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--port PORT] [--zmq-port ZMQ_PORT]"
            exit 1
            ;;
    esac
done

echo "=========================================="
echo "PRB Power & Inference Time Visualizer"
echo "=========================================="
echo "Web Port: $WEB_PORT"
echo "ZMQ Port: $ZMQ_PORT"
echo "Script: prb_power_visualizer.py"
echo "=========================================="

# Check dependencies
python3 -c "import flask" 2>/dev/null || {
    echo "Error: Flask not installed. Run: pip3 install --no-deps -r applications/prb-power-triton/deps/requirements.txt"
    exit 1
}

# Check if Python script exists
if [ ! -f "prb_power_visualizer.py" ]; then
    echo "Error: prb_power_visualizer.py not found!"
    exit 1
fi

# Run visualizer
python3 prb_power_visualizer.py --port $WEB_PORT --zmq-port $ZMQ_PORT
