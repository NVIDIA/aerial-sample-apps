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

set -euo pipefail

# iperf3 on docker network container:${NETWORK_CONTAINER} (e.g. open5gs UPF namespace).
NETWORK_CONTAINER="${NETWORK_CONTAINER:-open5gs_5gc}"
IMAGE="${IMAGE:-networkstatic/iperf3}"
SERVER_BIND_IP="${SERVER_BIND_IP:-10.45.0.1}"
CLIENT_TARGET_IP="${CLIENT_TARGET_IP:-10.45.0.16}"
UL_PORT="${UL_PORT:-5201}"
DL_PORT="${DL_PORT:-5202}"
DUAL="${DUAL:-0}"
CLIENT_TARGET_IP_2="${CLIENT_TARGET_IP_2:-}"
UL_PORT_2="${UL_PORT_2:-5203}"
MODE="${MODE:-both}"
TEST_SECONDS="${TEST_SECONDS:-300}"
BANDWIDTH="${BANDWIDTH:-2000M}"
PACKET_LENGTH="${PACKET_LENGTH:-1200}"
STOP_GRACE_SEC="${STOP_GRACE_SEC:-5}"
# Match UE -TestSeconds when UL runs longer than DL (-t TEST_SECONDS), or when DL fails early (set -e would else kill UL servers).
UE_TEST_SECONDS="${UE_TEST_SECONDS:-${TEST_SECONDS}}"
UL_START_SKEW_SEC="${UL_START_SKEW_SEC:-45}"
# After DL (MODE=both): keep NW UL iperf3 servers up so UE UL (-t) can finish. Default is the larger of
# UE_TEST_SECONDS and (TEST_SECONDS+UL_START_SKEW_SEC) so short DL + long UL does not cut UL early.
_min_hold_after_dl=$((TEST_SECONDS + UL_START_SKEW_SEC))
if (( UE_TEST_SECONDS > _min_hold_after_dl )); then
  _default_nw_ul_hold="${UE_TEST_SECONDS}"
else
  _default_nw_ul_hold="${_min_hold_after_dl}"
fi
NW_UL_HOLD_AFTER_DL_SEC="${NW_UL_HOLD_AFTER_DL_SEC:-${_default_nw_ul_hold}}"
unset _min_hold_after_dl _default_nw_ul_hold
READY_TIMEOUT_SEC="${READY_TIMEOUT_SEC:-120}"
READY_INTERVAL_SEC="${READY_INTERVAL_SEC:-1}"
READINESS_DOCKER_IMAGE="${READINESS_DOCKER_IMAGE:-busybox:1.36}"
# After NW UL listen is ready, sleep this many seconds before starting DL iperf3 client(s).
CLIENT_START_DELAY_SEC="${CLIENT_START_DELAY_SEC:-10}"

usage() {
  cat <<'EOF'
Usage: run_iperf3_nw.sh [--mode ul|dl|both] [--dual]

  --mode   ul | dl | both (default: both)
  --dual   Second UE: requires CLIENT_TARGET_IP_2; UL_PORT_2 for second UL server (default 5203).

Env (see script defaults): NETWORK_CONTAINER, IMAGE, SERVER_BIND_IP, CLIENT_TARGET_IP,
  UL_PORT, DL_PORT, CLIENT_TARGET_IP_2, UL_PORT_2, MODE, TEST_SECONDS, BANDWIDTH,
  PACKET_LENGTH, STOP_GRACE_SEC, DUAL, READY_TIMEOUT_SEC, READY_INTERVAL_SEC,
  READINESS_DOCKER_IMAGE (busybox: awk + /proc/net/tcp for local NW UL listen),
  CLIENT_START_DELAY_SEC (countdown sleep before DL client; default 10; 0 to skip).

MODE=both: set UE_TEST_SECONDS to UE -TestSeconds when it differs from NW TEST_SECONDS (DL -t).
  NW_UL_HOLD_AFTER_DL_SEC (default max(UE_TEST_SECONDS, TEST_SECONDS+UL_START_SKEW_SEC)): hold after DL
  before killing NW UL servers. Set 0 only if UL always ends before DL.

Align delay with UE -ClientStartDelaySec (default 10) and NW CLIENT_START_DELAY_SEC.
Same settings as run_iperf3_ue.ps1 use PascalCase params (Mode, TestSeconds, Serial, …).
EOF
}

ipv4_proc_addr() {
  local a b c d
  IFS=. read -r a b c d <<< "$1"
  printf '%02X%02X%02X%02X' "$d" "$c" "$b" "$a"
}

wait_local_listen() {
  local bind_ip="$1"
  local port="$2"
  local label="$3"
  local server_pid="$4"
  local deadline=$((SECONDS + READY_TIMEOUT_SEC))
  local ih ph
  ih="$(ipv4_proc_addr "${bind_ip}")"
  ph="$(printf '%04X' "${port}")"

  echo "Waiting for local LISTEN ${label} (${bind_ip}:${port}) ..."
  while ((SECONDS < deadline)); do
    if ! kill -0 "$server_pid" 2>/dev/null; then
      wait "$server_pid" || true
      echo "error: ${label} exited before listening on ${bind_ip}:${port}" >&2
      return 1
    fi
    if docker run --rm \
      --network "container:${NETWORK_CONTAINER}" \
      "${READINESS_DOCKER_IMAGE}" \
      sh -c "awk -v ih=${ih} -v ph=${ph} 'BEGIN{f=0} NR>1 && \$4==\"0A\" && \$2==ih\":\"ph { f=1; exit } END { exit !f }' /proc/net/tcp" 2>/dev/null; then
      echo "Ready: ${label}"
      return 0
    fi
    sleep "${READY_INTERVAL_SEC}"
  done
  echo "error: timeout waiting for ${label} (${bind_ip}:${port})" >&2
  exit 1
}

run_dl_client() {
  docker run --rm \
    --network "container:${NETWORK_CONTAINER}" \
    "${IMAGE}" -c "$1" -p "${DL_PORT}" -t "${TEST_SECONDS}" -u -b "${BANDWIDTH}" -i 1 -l "${PACKET_LENGTH}" --repeating-payload
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      if [[ $# -lt 2 ]]; then
        echo "error: --mode requires a value" >&2
        usage
        exit 1
      fi
      MODE="$2"
      shift 2
      ;;
    --dual)
      DUAL=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ "${DUAL}" == "1" ]]; then
  if [[ -z "${CLIENT_TARGET_IP_2}" ]]; then
    echo "error: --dual requires CLIENT_TARGET_IP_2 (second UE PDU IP)" >&2
    exit 1
  fi
fi

case "${MODE}" in
  ul|dl|both) ;;
  *)
    echo "error: MODE must be one of: ul, dl, both (got: ${MODE})" >&2
    usage
    exit 1
    ;;
esac

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" 2>/dev/null || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SERVER_PID2:-}" ]] && kill -0 "${SERVER_PID2}" 2>/dev/null; then
    kill "${SERVER_PID2}" 2>/dev/null || true
    wait "${SERVER_PID2}" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

if [[ "${MODE}" == "ul" || "${MODE}" == "both" ]]; then
  docker run --rm \
    --network "container:${NETWORK_CONTAINER}" \
    "${IMAGE}" -s -B "${SERVER_BIND_IP}" -p "${UL_PORT}" &
  SERVER_PID=$!
  if [[ "${DUAL}" == "1" ]]; then
    docker run --rm \
      --network "container:${NETWORK_CONTAINER}" \
      "${IMAGE}" -s -B "${SERVER_BIND_IP}" -p "${UL_PORT_2}" &
    SERVER_PID2=$!
  fi
  wait_local_listen "${SERVER_BIND_IP}" "${UL_PORT}" "NW UL server" "${SERVER_PID}"
  if [[ "${DUAL}" == "1" ]]; then
    wait_local_listen "${SERVER_BIND_IP}" "${UL_PORT_2}" "NW UL server (UE2)" "${SERVER_PID2}"
  fi
fi

dl_exit=0
if [[ "${MODE}" == "dl" || "${MODE}" == "both" ]]; then
  if [[ "${CLIENT_START_DELAY_SEC}" =~ ^[0-9]+$ ]] && (( CLIENT_START_DELAY_SEC > 0 )); then
    echo "Countdown before NW DL iperf3 client(s):"
    rem="${CLIENT_START_DELAY_SEC}"
    while (( rem > 0 )); do
      printf '\r  %d s remaining ... ' "${rem}"
      sleep 1
      ((rem--)) || true
    done
    printf '\r  starting DL client(s).\033[K\n'
  fi
  set +e
  if [[ "${DUAL}" == "1" ]]; then
    run_dl_client "${CLIENT_TARGET_IP}" &
    CLIENT_PID=$!
    run_dl_client "${CLIENT_TARGET_IP_2}" &
    CLIENT_PID2=$!
    wait "${CLIENT_PID}" || dl_exit=1
    wait "${CLIENT_PID2}" || dl_exit=1
  else
    run_dl_client "${CLIENT_TARGET_IP}"
    dl_exit=$?
  fi
  set -e

  if [[ "${MODE}" == "both" ]]; then
    if (( dl_exit != 0 )); then
      hold=$((UE_TEST_SECONDS + UL_START_SKEW_SEC))
      echo "warning: DL iperf3 client(s) exited non-zero (${dl_exit}); holding NW UL servers ${hold}s for UE UL" >&2
    else
      hold="${NW_UL_HOLD_AFTER_DL_SEC}"
      if [[ "${hold}" =~ ^[0-9]+$ ]] && (( hold > 0 )); then
        echo "MODE=both: holding NW UL servers ${hold}s after DL (NW_UL_HOLD_AFTER_DL_SEC)." >&2
      elif [[ "${hold}" == "0" ]]; then
        echo "MODE=both: NW_UL_HOLD_AFTER_DL_SEC=0; NW UL servers torn down immediately after DL." >&2
      else
        echo "warning: NW_UL_HOLD_AFTER_DL_SEC='${hold}' invalid; skipping hold sleep" >&2
      fi
    fi
    if [[ "${hold}" =~ ^[0-9]+$ ]] && (( hold > 0 )); then
      sleep "${hold}"
    fi
  fi
elif [[ "${MODE}" == "ul" ]]; then
  wait "${SERVER_PID}"
  if [[ "${DUAL}" == "1" ]]; then
    wait "${SERVER_PID2}"
  fi
fi

if [[ "${STOP_GRACE_SEC}" -gt 0 ]]; then
  sleep "${STOP_GRACE_SEC}"
fi

exit "$dl_exit"
