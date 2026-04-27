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

FROM nvcr.io/nvidia/tritonserver:25.11-py3

# Install build and runtime dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libzmq3-dev \
    pkg-config \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
COPY applications/prb-power-triton/deps /tmp/deps
RUN pip3 install --no-deps -r /tmp/deps/no-deps/requirements.txt && \
    pip3 install -r /tmp/deps/requirements.txt

# Copy build sources and app files
COPY common /opt/src/common
COPY CMakeLists.txt /opt/src/CMakeLists.txt
COPY applications/prb-power-triton /opt/src/applications/prb-power-triton

# Build the C++ application
WORKDIR /opt/src
RUN cmake -B build -S . && \
    cmake --build build -j$(nproc)

# Copy runtime files to expected locations
RUN mkdir -p /opt/dapp/bin /models /logs && \
    cp build/bin/prb_power_dapp /opt/dapp/bin/

COPY applications/prb-power-triton/models /models

WORKDIR /opt

# Environment variables
ENV TRITON_MODEL_REPO=/models \
    E3_CONFIG=/opt/src/applications/prb-power-triton/config/e3_config.json \
    PATH="/opt/dapp/bin:${PATH}" \
    CUDA_VISIBLE_DEVICES=0

# E3: 5558 (dApp client), 5559 (Results PUB)
# Expose ports
EXPOSE 5558 5559

# Entry point
CMD ["/opt/src/applications/prb-power-triton/start_services.sh"]
