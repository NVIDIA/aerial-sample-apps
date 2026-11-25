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

# Build nvIPC (gt_common_libs: nvIPC, nvlog, external).
# Copies a packed tree from the source mount into OCUDU_ROOT/plugins/aerial/gt_common_libs,
# then configures and builds there (GT_COMMON_LIBS_DIR becomes that plugin path).
# Same layout as copy_nvipc_to_out.sh → /nvipc_out (gnb_dev: /workspace/gt_common_libs).
#
# First-time: run copy_nvipc_to_out.sh from aerial so the nvipc_src volume has CMakeLists.txt,
# nvIPC/, nvlog/, external/.
#
# Environment (optional):
#   GT_COMMON_LIBS_SRC - tree to copy from (default: /workspace/gt_common_libs); or pass as $1
#   NUM_JOBS           - parallel jobs (default: nproc)
#   NVIPC_CUDA_ENABLE  - ON or OFF (default: OFF for the gNB container)
#   NVIPC_FMTLOG_ENABLE - ON or OFF (default: ON)
#   CMAKE_BUILD_TYPE   - Release or Debug (default: Release)
#   DO_INSTALL         - 1 to run cmake --install; if unset, defaults to 1 when a
#                      container is detected (/.dockerenv or cgroup), else 0
#   OCUDU_ROOT         - ocudu root; syncs sources to plugins/aerial/gt_common_libs before cmake
#                      (default: /workspace)
#   EXTRA_CMAKE_ARGS  - extra args passed to cmake
#
# Usage (in dev container): ./split6_scripts/build_nvipc.sh
#   Or: ./split6_scripts/build_nvipc.sh [GT_COMMON_LIBS_SRC]
#

set -e

OCUDU_ROOT="${OCUDU_ROOT:-/workspace}"
GT_COMMON_LIBS_SRC="${1:-${GT_COMMON_LIBS_SRC:-/workspace/gt_common_libs}}"
NUM_JOBS="${NUM_JOBS:-$(nproc)}"
NVIPC_CUDA_ENABLE="${NVIPC_CUDA_ENABLE:-OFF}"
NVIPC_FMTLOG_ENABLE="${NVIPC_FMTLOG_ENABLE:-ON}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
EXTRA_CMAKE_ARGS="${EXTRA_CMAKE_ARGS:-}"

# If DO_INSTALL is unset: install by default inside common container runtimes; skip on host.
if [[ ! -v DO_INSTALL ]]; then
  if [[ -f /.dockerenv ]] || grep -qE '(docker|containerd|libpod|lxc|kubepods)' /proc/1/cgroup 2>/dev/null; then
    DO_INSTALL=1
  else
    DO_INSTALL=0
  fi
fi

if [[ ! -f "$GT_COMMON_LIBS_SRC/CMakeLists.txt" ]]; then
  echo "Error: CMakeLists.txt not found in $GT_COMMON_LIBS_SRC" >&2
  echo "Set GT_COMMON_LIBS_SRC or pass the source gt_common_libs path as \$1." >&2
  exit 1
fi

GT_COMMON_LIBS_DIR=$OCUDU_ROOT/plugins/aerial/gt_common_libs
mkdir -p $GT_COMMON_LIBS_DIR
cp -a $GT_COMMON_LIBS_SRC/* $GT_COMMON_LIBS_DIR/
echo "Synced gt_common_libs from $GT_COMMON_LIBS_SRC to $GT_COMMON_LIBS_DIR"

cd "$GT_COMMON_LIBS_DIR"
cmake -Bbuild \
  -DNVIPC_CUDA_ENABLE="$NVIPC_CUDA_ENABLE" \
  -DNVIPC_FMTLOG_ENABLE="$NVIPC_FMTLOG_ENABLE" \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  $EXTRA_CMAKE_ARGS .
cmake --build build -j"$NUM_JOBS"

if [[ "$DO_INSTALL" = "1" ]]; then
  cmake --install build
fi
