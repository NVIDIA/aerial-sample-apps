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

set -euo pipefail

GNB_CONFIG_TEMPLATE="${GNB_CONFIG_TEMPLATE:-/configs/gnb_split6.yml}"
if [ -z "${GNB_CONFIG_RENDERED:-}" ]; then
  GNB_CONFIG_RENDERED="$(mktemp "${TMPDIR:-/tmp}/gnb_config.XXXXXX.yml")"
fi
OPEN5GS_IP_VALUE="${OPEN5GS_IP:-}"
GNB_IP_VALUE="${GNB_IP:-}"

if [ ! -r "$GNB_CONFIG_TEMPLATE" ]; then
  echo "Error: config template not readable: $GNB_CONFIG_TEMPLATE" >&2
  exit 1
fi

if [ -z "$OPEN5GS_IP_VALUE" ] || [ -z "$GNB_IP_VALUE" ]; then
  echo "Error: OPEN5GS_IP and GNB_IP must be set (source: docker/split6.env)." >&2
  exit 1
fi

if ! command -v envsubst >/dev/null 2>&1; then
  echo "Error: envsubst not found. Install gettext-base in this environment." >&2
  exit 1
fi

export OPEN5GS_IP="$OPEN5GS_IP_VALUE"
export GNB_IP="$GNB_IP_VALUE"

rendered_content="$(<"$GNB_CONFIG_TEMPLATE")"
default_pattern='(\$\{([A-Za-z_][A-Za-z0-9_]*):-([^}]*)\})'
substitution_count=0
while [[ "$rendered_content" =~ $default_pattern ]]; do
  substitution_count=$((substitution_count + 1))
  if (( substitution_count > 100 )); then
    echo "Error: too many nested default substitutions in $GNB_CONFIG_TEMPLATE" >&2
    exit 1
  fi
  placeholder="${BASH_REMATCH[1]}"
  variable_name="${BASH_REMATCH[2]}"
  default_value="${BASH_REMATCH[3]}"
  variable_value="${!variable_name-}"
  if [[ -z "$variable_value" ]]; then
    variable_value="$default_value"
  fi
  rendered_content="${rendered_content//"$placeholder"/"$variable_value"}"
done

printf '%s\n' "$rendered_content" | envsubst '${OPEN5GS_IP} ${GNB_IP}' > "$GNB_CONFIG_RENDERED"

echo "Rendered gNB config: $GNB_CONFIG_TEMPLATE -> $GNB_CONFIG_RENDERED"
