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

# Build only the gnb_split_6 target with plugins enabled (Aerial plugin in-tree at SRC_DIR/plugins/aerial).
# For use inside the dev container where ocudu source is mounted at /workspace.
#
# Environment (optional):
#   SRC_DIR            - ocudu source tree (default: /workspace)
#   INSTALL_PREFIX     - install prefix (default: /opt/ocudu)
#   NUM_JOBS           - parallel jobs (default: nproc)
#   DEBUG_BUILD        - set to 1 for RelWithDebInfo (default: 0)
#   CLEAN_BUILD        - set to 1 for a clean build (default: 0, reuses build dir)
#   MARCH              - -march for build (default: native)
#   COMPILER           - gcc or clang (default: gcc)
#   EXTRA_CMAKE_ARGS   - extra args passed to cmake
#
# Usage:
#   ./split6_scripts/build_gnb_split6_aerial_plugin.sh
#   ./split6_scripts/build_gnb_split6_aerial_plugin.sh [SRC_DIR]
#

set -e

SRC_DIR="${1:-${SRC_DIR:-/workspace}}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/ocudu}"
NUM_JOBS="${NUM_JOBS:-$(nproc)}"
DEBUG_BUILD="${DEBUG_BUILD:-0}"
CLEAN_BUILD="${CLEAN_BUILD:-0}"
MARCH="${MARCH:-native}"
COMPILER="${COMPILER:-gcc}"
EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS:-}"

BUILD_TYPE=""
if [ "$DEBUG_BUILD" = "1" ]; then
  BUILD_TYPE="-DCMAKE_BUILD_TYPE=RelWithDebInfo"
fi

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

PLUGIN_CMAKE="${SRC_DIR}/plugins/aerial/CMakeLists.txt"
if [[ ! -f "$PLUGIN_CMAKE" ]]; then
  echo "Error: Aerial plugin not found at ${PLUGIN_CMAKE}" >&2
  echo "Expected in-tree layout: <ocudu>/plugins/aerial" >&2
  exit 1
fi

BUILDER_CLEAN_ARG="True"
if [ "$CLEAN_BUILD" = "0" ]; then
  BUILDER_CLEAN_ARG="False"
fi

ARCH_FLAG="-DMARCH=${EFFECTIVE_MARCH}"
if [ "$(uname -m)" = "aarch64" ]; then
  ARCH_FLAG="-DMCPU=${MARCH}"
  if [ "${EFFECTIVE_MARCH}" != "native" ]; then
    ARCH_FLAG="${ARCH_FLAG} -DMARCH=${EFFECTIVE_MARCH}"
  fi
fi

"$BUILD_SCRIPT" -r "$BUILDER_CLEAN_ARG" -c "$COMPILER" -m "-j${NUM_JOBS} gnb_split_6" \
  -DBUILD_TESTING=False -DENABLE_UHD=Off -DENABLE_DPDK=Off -DENABLE_EXPORT=ON -DENABLE_PLUGINS=ON \
  -DENABLE_AERIAL_PLUGIN=On \
  ${ARCH_FLAG} -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
  $BUILD_TYPE $EXTRA_CMAKE_ARGS "$SRC_DIR"

# Preserve gnb_split_6 binary name (make install may install as 'gnb')
preserved_binary="$(mktemp "${TMPDIR:-/tmp}/gnb_split_6.XXXXXX")"
trap 'rm -f "$preserved_binary"' EXIT
cp "$SRC_DIR/build/apps/gnb_split_6/gnb" "$preserved_binary"
cd "$SRC_DIR/build" && make install
mkdir -p "$INSTALL_PREFIX/bin"
mv "$preserved_binary" "$INSTALL_PREFIX/bin/gnb_split_6"
