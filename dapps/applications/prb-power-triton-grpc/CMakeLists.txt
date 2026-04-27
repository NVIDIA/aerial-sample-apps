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
# PRB Power dApp with Triton inference backend.
#

if(NOT GRPC_CLIENT_LIB)
    message(STATUS "Skipping prb-power-triton-grpc (grpcclient not found)")
    return()
endif()

add_executable(prb_power_dapp
    prb_power_app.cpp
    triton_integration/triton_engine.cpp
)

target_include_directories(prb_power_dapp
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        /workspace/install/include
        /workspace/install/include/triton
)

target_link_libraries(prb_power_dapp
    PRIVATE
        aerial_dapp_core
        /workspace/install/lib/libprotobuf.a
        /workspace/install/lib/libgrpcclient.so
)

set_target_properties(prb_power_dapp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "/workspace/install/lib"
    INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS prb_power_dapp
    RUNTIME DESTINATION bin
)
