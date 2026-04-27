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

# Stage 1: Get the Triton server executables from the server container
FROM nvcr.io/nvidia/tritonserver:25.11-py3 AS server_build

# Stage 2: Use the SDK container as the final base (includes gRPC client library)
FROM nvcr.io/nvidia/tritonserver:25.11-py3-sdk

# Copy Triton Server and its dependencies from the server_build stage
COPY --from=server_build /opt/tritonserver /opt/tritonserver

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libzmq3-dev \
    pkg-config \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Fix UCC/UCX ABI mismatch in SDK image (missing ucs_config_doc_nop symbol)
RUN mkdir -p /opt/lib && \
    echo 'void ucs_config_doc_nop(void) {}' | \
    gcc -shared -o /opt/lib/libucc_compat.so -x c -

# Install Python dependencies
COPY applications/prb-power-triton-grpc/deps /tmp/deps
RUN pip3 install --no-deps -r /tmp/deps/no-deps/requirements.txt && \
    pip3 install -r /tmp/deps/requirements.txt

# Copy build sources and app files
COPY common /opt/src/common
COPY CMakeLists.txt /opt/src/CMakeLists.txt
COPY applications/prb-power-triton-grpc /opt/src/applications/prb-power-triton-grpc

# Build the C++ application
WORKDIR /opt/src
RUN CMAKE_PREFIX_PATH=/workspace/install cmake -B build -S . && \
    cmake --build build -j$(nproc)

# Copy runtime files to expected locations
RUN mkdir -p /opt/dapp/bin /models /logs && \
    cp build/bin/prb_power_dapp /opt/dapp/bin/

COPY applications/prb-power-triton-grpc/models /models

WORKDIR /opt

# Environment variables
ENV TRITON_MODEL_REPO=/models \
    E3_CONFIG=/opt/src/applications/prb-power-triton-grpc/config/e3_config.json \
    PATH="/opt/tritonserver/bin:/opt/dapp/bin:${PATH}" \
    CUDA_VISIBLE_DEVICES=0 \
    LD_PRELOAD=/opt/lib/libucc_compat.so

# Expose ports
# Triton: 8000 (HTTP), 8001 (gRPC), 8002 (Metrics)
# E3: 5558 (dApp client), 5559 (Results PUB)
EXPOSE 8000 8001 8002 5558 5559

# Entry point
CMD ["/opt/src/applications/prb-power-triton-grpc/start_services.sh"]
