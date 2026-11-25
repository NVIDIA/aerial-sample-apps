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
# Force-handover a UE to the other cell.
# Usage: ./scripts/ho_ue.sh <RNTI>
#   RNTI may be 4603 or 0x4603.
#
# Reads serving PCI from recent ocudu_gnb scheduler stdout (the "pci rnti |" table).
# Both cells share PLMN/TAC from the split6 config (00101 / 7).
#

set -euo pipefail

CONTAINER="${GNB_CONTAINER:-${GNB_CONTAINER_NAME:-ocudu_gnb}}"
PLMN="${HO_PLMN:-00101}"
TAC="${HO_TAC:-7}"
PCI_A="${HO_PCI_A:-1}"
PCI_B="${HO_PCI_B:-2}"
LOG_TAIL="${HO_LOG_TAIL:-80}"

usage() {
  echo "Usage: $0 <RNTI>" >&2
  echo "  Example: $0 0x4603" >&2
  echo "  Env: GNB_CONTAINER GNB_CONTAINER_NAME HO_PLMN HO_TAC HO_PCI_A HO_PCI_B HO_LOG_TAIL" >&2
  exit 1
}

[[ $# -eq 1 ]] || usage
[[ "${1}" != "-h" && "${1}" != "--help" ]] || usage

rnti_in="${1}"
rnti_hex="${rnti_in#0[xX]}"
if [[ ! "${rnti_hex}" =~ ^[0-9a-fA-F]+$ ]]; then
  echo "Invalid RNTI '${rnti_in}'" >&2
  exit 1
fi
# Canonical form for the ho CLI (hex with 0x prefix).
rnti_cmd="0x$(printf '%x' "0x${rnti_hex}")"
# Scheduler stdout prints the hex without 0x, space-padded, e.g. " 4603".
rnti_log="$(printf '%x' "0x${rnti_hex}")"

serving_pci="$(
  docker logs --tail "${LOG_TAIL}" "${CONTAINER}" 2>&1 |
    awk -v rnti="${rnti_log}" '
      $1 ~ /^[0-9]+$/ && tolower($2) == rnti && $3 == "|" { pci = $1 }
      END { if (pci != "") print pci }
    '
)"

if [[ -z "${serving_pci}" ]]; then
  echo "Could not find rnti ${rnti_cmd} in last ${LOG_TAIL} lines of ${CONTAINER} logs." >&2
  echo "Is stdout UE metrics running? Try: docker logs --tail 40 ${CONTAINER}" >&2
  exit 1
fi

if [[ "${serving_pci}" == "${PCI_A}" ]]; then
  target_pci="${PCI_B}"
elif [[ "${serving_pci}" == "${PCI_B}" ]]; then
  target_pci="${PCI_A}"
else
  echo "Serving pci=${serving_pci} is not ${PCI_A} or ${PCI_B} (set HO_PCI_A / HO_PCI_B)." >&2
  exit 1
fi

cmd="ho ${serving_pci} ${rnti_cmd} ${target_pci} ${PLMN} ${TAC}"
echo "Triggering: ${cmd}"
docker exec "${CONTAINER}" bash -c "printf '%s\\n' '${cmd}' > /proc/1/fd/0"
