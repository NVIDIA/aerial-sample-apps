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

# Run from the aerial container after pack_nvipc.sh (from /opt/nvidia/cuBB/cuPHY-CP/gt_common_libs).
# Copies the tarball to /nvipc_out and extracts so that gt_common_libs layout is at /nvipc_out
# (same dir is mounted in gnb_dev as /workspace/gt_common_libs).

set -e

TEMP_SRC_DIR="${OUT:-/nvipc_out}"
TEMP_SRC_DIR="$TEMP_SRC_DIR" "${cuBB_SDK}/cuPHY-CP/gt_common_libs/pack_nvipc.sh"
echo "Extracted to $TEMP_SRC_DIR (gnb_dev sees /nvipc_out at /workspace/gt_common_libs)."
