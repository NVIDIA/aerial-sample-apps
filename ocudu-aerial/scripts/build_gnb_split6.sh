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

# Build only the gnb_split_6 target (Split-6, ROHC only; no UHD/DPDK).
# For use inside the dev container: ocudu is at /workspace, this script at /workspace/split6_scripts/.
# builder.sh is taken from the source tree (SRC_DIR/docker/scripts/builder.sh).
#
# Environment (optional):
#   SRC_DIR            - source tree (default: /workspace for dev container)
#   INSTALL_PREFIX     - install prefix (default: /opt/ocudu)
#   NUM_JOBS           - parallel jobs (default: nproc)
#   DEBUG_BUILD        - set to 1 for RelWithDebInfo (default: 0)
#   MARCH              - -march for build (default: native)
#   COMPILER           - gcc or clang (default: gcc)
#   EXTRA_CMAKE_ARGS   - extra args passed to cmake
#
# Usage (in dev container): ./split6_scripts/build_gnb_split6.sh
#   Or: ./split6_scripts/build_gnb_split6.sh [SRC_DIR]
#

set -e

SRC_DIR="${1:-${SRC_DIR:-/workspace}}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/ocudu}"
NUM_JOBS="${NUM_JOBS:-$(nproc)}"
DEBUG_BUILD="${DEBUG_BUILD:-0}"
MARCH="${MARCH:-native}"
COMPILER="${COMPILER:-gcc}"
EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS:-}"

BUILD_TYPE=""
if [ "$DEBUG_BUILD" = "1" ]; then
  BUILD_TYPE="-DCMAKE_BUILD_TYPE=RelWithDebInfo"
fi

# builder.sh lives in the ocudu source tree (not next to this script)
BUILD_SCRIPT="${SRC_DIR}/docker/scripts/builder.sh"
CHECK_MARCH_SCRIPT="${SRC_DIR}/docker/scripts/check_cpu_crypto.sh"
if [[ ! -x "$BUILD_SCRIPT" ]]; then
  echo "Error: builder.sh not found or not executable: $BUILD_SCRIPT" >&2
  echo "Set SRC_DIR or pass the ocudu source path (e.g. /workspace)." >&2
  exit 1
fi

EFFECTIVE_MARCH="${MARCH}"
if [[ -x "$CHECK_MARCH_SCRIPT" ]]; then
  EFFECTIVE_MARCH="$(bash "$CHECK_MARCH_SCRIPT" "${MARCH}")"
fi
echo "Using MARCH=${EFFECTIVE_MARCH} for gnb_split_6 build"

ARCH_FLAG="-DMARCH=${EFFECTIVE_MARCH}"
if [ "$(uname -m)" = "aarch64" ]; then
  ARCH_FLAG="-DMCPU=${MARCH}"
  if [ "${EFFECTIVE_MARCH}" != "native" ]; then
    ARCH_FLAG="${ARCH_FLAG} -DMARCH=${EFFECTIVE_MARCH}"
  fi
fi

PLUGIN_ARGS="-DENABLE_PLUGINS=ON"
if [[ -f "${SRC_DIR}/plugins/aerial/CMakeLists.txt" ]]; then
  PLUGIN_ARGS="${PLUGIN_ARGS} -DENABLE_AERIAL_PLUGIN=On"
fi

"$BUILD_SCRIPT" -c "$COMPILER" -m "-j${NUM_JOBS} gnb_split_6" \
  -DBUILD_TESTING=False -DENABLE_UHD=Off -DENABLE_DPDK=Off -DENABLE_EXPORT=ON \
  ${PLUGIN_ARGS} \
  ${ARCH_FLAG} -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
  $BUILD_TYPE $EXTRA_CMAKE_ARGS "$SRC_DIR"

# Preserve gnb_split_6 binary name (make install may install as 'gnb')
preserved_binary="$(mktemp "${TMPDIR:-/tmp}/gnb_split_6.XXXXXX")"
trap 'rm -f "$preserved_binary"' EXIT
cp "$SRC_DIR/build/apps/gnb_split_6/gnb" "$preserved_binary"
cd "$SRC_DIR/build" && make install
mkdir -p "$INSTALL_PREFIX/bin"
mv "$preserved_binary" "$INSTALL_PREFIX/bin/gnb_split_6"