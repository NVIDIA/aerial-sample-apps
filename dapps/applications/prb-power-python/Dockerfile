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

FROM nvcr.io/nvidia/pytorch:26.01-py3

# Install build dependencies (Python, NumPy, PyTorch, CUDA already included)
RUN apt-get update && apt-get install -y \
    libzmq3-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Install Python dependencies
COPY applications/prb-power-python/deps /tmp/deps
RUN pip install -r /tmp/deps/requirements.txt

# Copy build sources and app files
COPY common /opt/src/common
COPY CMakeLists.txt /opt/src/CMakeLists.txt
COPY applications/prb-power-python /opt/src/applications/prb-power-python

# Build the C++ application
WORKDIR /opt/src
RUN cmake -B build -S . \
        -Dpybind11_DIR=$(python3 -m pybind11 --cmakedir) && \
    cmake --build build -j$(nproc)

# Copy binary to runtime location
RUN mkdir -p /opt/dapp/bin /logs && \
    cp build/bin/prb_power_python_dapp /opt/dapp/bin/

WORKDIR /opt

# Environment variables
ENV E3_CONFIG=/opt/src/applications/prb-power-python/config/e3_config.json \
    PATH="/opt/dapp/bin:${PATH}" \
    CUDA_VISIBLE_DEVICES=0

# E3: 5558 (dApp client), 5559 (Results PUB)
EXPOSE 5558 5559

# Entry point
CMD ["/opt/src/applications/prb-power-python/start_services.sh"]
