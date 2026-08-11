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
# PRB Power dApp with embedded Python inference backend.
#

find_package(Python3 COMPONENTS Development NumPy QUIET)
find_package(pybind11 QUIET)
if(NOT Python3_FOUND OR NOT Python3_NumPy_FOUND OR NOT pybind11_FOUND)
    message(STATUS "Skipping prb-power-python (Python3/NumPy/pybind11 not found)")
    return()
endif()

add_executable(prb_power_python_dapp
    prb_power_app.cpp
    python_integration/python_engine.cpp
)

target_include_directories(prb_power_python_dapp
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(prb_power_python_dapp
    PRIVATE
        aerial_dapp_core
        Python3::Python
        Python3::NumPy
        pybind11::embed
)

set_target_properties(prb_power_python_dapp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    INSTALL_RPATH "$ORIGIN/../lib"
)

install(TARGETS prb_power_python_dapp
    RUNTIME DESTINATION bin
)
