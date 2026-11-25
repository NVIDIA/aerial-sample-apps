#!/usr/bin/env bash

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

set -e

cpu="${1:-native}"

if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "$cpu"
  exit 0
fi

src_file="/tmp/check_arm_crypto.cpp"
bin_file="/tmp/check_arm_crypto"

cleanup() {
  rm -f "$src_file" "$bin_file"
}
trap cleanup EXIT

cat >"$src_file" <<'EOF'
#include <arm_neon.h>
int main() {
  uint64_t a = 0x12345678abcdef00ULL;
  uint64_t b = 0xfedcba9876543210ULL;
  uint64x2_t result = vreinterpretq_u64_p128(vmull_p64(a, b));
  return (int)vgetq_lane_u64(result, 0);
}
EOF

if ! printf 'int main() { return 0; }\n' | c++ -x c++ -mcpu="$cpu" -o "$bin_file" - >/dev/null 2>&1; then
  echo "Unsupported ARM CPU value for -mcpu: $cpu" >&2
  exit 1
fi

if c++ -mcpu="$cpu" "$src_file" -o "$bin_file" >/dev/null 2>&1; then
  echo "$cpu"
  exit 0
fi

for fallback in armv9.2-a armv9-a armv8.2-a armv8-a; do
  if c++ -march="${fallback}+crypto" "$src_file" -o "$bin_file" >/dev/null 2>&1; then
    echo "$fallback"
    exit 0
  fi
done

echo "No ARM crypto-capable -march found for vmull_p64" >&2
exit 1
