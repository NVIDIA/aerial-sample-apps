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
# Top-level CMake for NVIDIA Aerial dApps.
#
# Build:
#   mkdir -p build && cd build
#   cmake ../dapps && make -j$(nproc)
#

cmake_minimum_required(VERSION 3.18)
project(aerial_dapps
    VERSION 1.1.0
    DESCRIPTION "NVIDIA Aerial dApp Applications"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Dependencies ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)

# --- Shared core ---
add_subdirectory(common/e3_manager)

# --- Engine dependencies (optional, per-application) ---

# Triton C API (in-process server)
find_library(TRITONSERVER_LIB tritonserver PATHS /opt/tritonserver/lib NO_DEFAULT_PATH)

# Triton gRPC client (out-of-process server)
find_library(GRPC_CLIENT_LIB grpcclient PATHS /workspace/install/lib NO_DEFAULT_PATH)

# Python + pybind11 (embedded interpreter)
find_package(Python3 COMPONENTS Development NumPy QUIET)
find_package(pybind11 QUIET)

# --- Applications (auto-discovered, each app gates its own deps) ---
file(GLOB app_entries RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/applications" "applications/*")
foreach(app ${app_entries})
    set(app_dir "${CMAKE_CURRENT_SOURCE_DIR}/applications/${app}")
    if(IS_DIRECTORY "${app_dir}" AND EXISTS "${app_dir}/CMakeLists.txt")
        add_subdirectory("applications/${app}")
    endif()
endforeach()
