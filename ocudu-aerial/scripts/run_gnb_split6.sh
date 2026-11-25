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

# Run gnb_split_6 inside the dev container.
# For use with config templates mounted under /configs.
# The Aerial split6 plugin is linked into the binary when built with ENABLE_PLUGINS=ON (no dlopen).
# LD_LIBRARY_PATH still includes install lib and optional nvIPC paths for libnvipc and other deps.
#
# Usage: ./split6_scripts/run_gnb_split6.sh [extra args...]
#
# Core dumps (on segfault): set ENABLE_COREDUMP=1 to allow core files, e.g.:
#   ENABLE_COREDUMP=1 ./split6_scripts/run_gnb_split6.sh
# Core file is written in the current directory (e.g. /workspace/split6_scripts in dev
# container → scripts/ on host). Then: gdb /opt/ocudu/bin/gnb_split_6 core
#

set -e

DEBUG="${DEBUG:-0}"
ENABLE_COREDUMP="${ENABLE_COREDUMP:-0}"
SRC_DIR="${SRC_DIR:-/workspace}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/ocudu}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${INSTALL_PREFIX}/lib:${LD_LIBRARY_PATH:-}"

export OCUDU_AERIAL_P7_RX_SCHED_PRIORITY=${OCUDU_AERIAL_P7_RX_SCHED_PRIORITY:-95}
export OCUDU_AERIAL_P7_RX_THREAD_NAME=${OCUDU_AERIAL_P7_RX_THREAD_NAME:-gnb_split6_p7_rx}
export OCUDU_AERIAL_P7_TX_SCHED_PRIORITY=${OCUDU_AERIAL_P7_TX_SCHED_PRIORITY:-95}
export OCUDU_AERIAL_P7_TX_THREAD_NAME=${OCUDU_AERIAL_P7_TX_THREAD_NAME:-gnb_split6_p7_tx}
NVIPC_LIB_PATH=${NVIPC_LIB_PATH:-/workspace/gt_common_libs/build/nvIPC}
export NVIPC_LIB_PATH

GNB_START_DELAY="${GNB_START_DELAY:-0}"
if ! [[ "$GNB_START_DELAY" =~ ^[0-9]+$ ]]; then
  echo "Error: GNB_START_DELAY must be a non-negative integer (got: $GNB_START_DELAY)." >&2
  exit 1
fi
if (( GNB_START_DELAY > 0 )); then
  echo "Delaying gNB startup by ${GNB_START_DELAY}s."
  sleep "$GNB_START_DELAY"
fi

GNB_BIN="gnb_split_6"
if ! command -v "$GNB_BIN" >/dev/null 2>&1; then
  BUILD_GNB_BIN="${SRC_DIR}/build/apps/gnb_split_6/gnb"
  INSTALL_GNB_BIN="${INSTALL_PREFIX}/bin/gnb_split_6"

  if [ -x "$INSTALL_GNB_BIN" ]; then
    GNB_BIN="$INSTALL_GNB_BIN"
  elif [ -x "$BUILD_GNB_BIN" ]; then
    mkdir -p "${INSTALL_PREFIX}/bin" 2>/dev/null || true
    if cp "$BUILD_GNB_BIN" "$INSTALL_GNB_BIN" 2>/dev/null; then
      chmod +x "$INSTALL_GNB_BIN" 2>/dev/null || true
      GNB_BIN="$INSTALL_GNB_BIN"
      echo "gnb_split_6 not in PATH; copied to $INSTALL_GNB_BIN"
    else
      GNB_BIN="$BUILD_GNB_BIN"
      echo "gnb_split_6 not in PATH; using build binary: $BUILD_GNB_BIN"
    fi
  else
    echo "Error: gnb_split_6 not found in PATH, install dir, or build dir." >&2
    echo "Expected one of:" >&2
    echo "  - PATH: gnb_split_6" >&2
    echo "  - install: $INSTALL_GNB_BIN" >&2
    echo "  - build:   $BUILD_GNB_BIN" >&2
    echo "Run build script first: ./split6_scripts/build_gnb_split6.sh" >&2
    exit 1
  fi
fi

if [ "$ENABLE_COREDUMP" = "1" ]; then
  ulimit -c unlimited
  # %e=executable, %p=pid, %t=unix timestamp (date in filename, no overwrite)
  if ! echo "core.%e.%p.%t" > /proc/sys/kernel/core_pattern 2>/dev/null; then
    echo "Warning: cannot update /proc/sys/kernel/core_pattern; using the existing pattern." >&2
  fi
  echo "Coredump enabled ($(date '+%Y-%m-%d %H:%M:%S')). Core will be in: $(pwd) (pattern: $(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo 'core'))"
fi

export GNB_CONFIG_TEMPLATE="${GNB_CONFIG_TEMPLATE:-/configs/gnb_split6.yml}"
if [ -z "${GNB_CONFIG_RENDERED:-}" ]; then
  GNB_CONFIG_RENDERED="$(mktemp "${TMPDIR:-/tmp}/gnb_config.XXXXXX.yml")"
fi
export GNB_CONFIG_RENDERED
"$SCRIPT_DIR/render_gnb_config.sh"

CONFIG_ARGS=(-c "$GNB_CONFIG_RENDERED")

# GNB_CONFIG_OVERLAY names a file next to the template, e.g. a multi-cell cells list. It is
# passed after the base so it wins, and it is not rendered, so it must hold no placeholders.
if [ -n "${GNB_CONFIG_OVERLAY:-}" ]; then
  OVERLAY_PATH="$(dirname "$GNB_CONFIG_TEMPLATE")/${GNB_CONFIG_OVERLAY}"
  if [ ! -r "$OVERLAY_PATH" ]; then
    echo "Error: config overlay not readable: $OVERLAY_PATH" >&2
    exit 1
  fi
  CONFIG_ARGS+=(-c "$OVERLAY_PATH")
  echo "gNB config overlay: $OVERLAY_PATH"
fi

if [ "$DEBUG" = "1" ]; then
  exec gdb -q --args "$GNB_BIN" "${CONFIG_ARGS[@]}" "$@"
else
  exec "$GNB_BIN" "${CONFIG_ARGS[@]}" "$@"
fi
