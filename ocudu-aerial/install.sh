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

# Install and run the split-6 stack (OCUDU gNB + Open5GS 5GC) for ocudu-aerial.
# Run from repo root.
#
# Prerequisites: Docker, Docker Compose, ocudu submodule initialized and (optionally) built.
#
# Usage:
#   ./install.sh [build|up|down|logs]           # production image
#   ./install.sh dev [build|up|down|logs|exec|aerial]  # dev image + host-mounted source/build
#   build   - build images (default)
#   up      - build and start containers
#   down    - stop and remove containers
#   logs    - tail compose logs
#   exec    - (dev only) attach to gNB container with bash
#   aerial  - (dev only) attach to aerial container (run pack_nvipc.sh manually; put result in /nvipc_out)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
COMPOSE_DIR="$SCRIPT_DIR/docker"
COMPOSE_FILE="$COMPOSE_DIR/docker-compose.yml"
SPLIT6_ENV_FILE="${SPLIT6_ENV_FILE:-$COMPOSE_DIR/split6.env}"
COMPOSE_PROFILE="prod"
DEV_MODE=false

# Ensure we're in the repo that has ocudu as submodule
if [[ ! -d "$REPO_ROOT/ocudu" ]]; then
  echo "Error: ocudu submodule not found at $REPO_ROOT/ocudu"
  echo "Run from repo root: git submodule update --init --recursive"
  exit 1
fi

if [[ ! -f "$SPLIT6_ENV_FILE" ]]; then
  echo "Error: split6 env file not found at $SPLIT6_ENV_FILE"
  exit 1
fi

# Subscriber DB: prefer configs/subscriber_db.local.csv when split6.env still has the default.
LOCAL_SUBSCRIBER_DB="$REPO_ROOT/configs/subscriber_db.local.csv"
configured_subscriber_db="${OPEN5GS_SUBSCRIBER_DB_PATH:-}"
if [[ -z "$configured_subscriber_db" ]]; then
  configured_subscriber_db="$(sed -n 's/^OPEN5GS_SUBSCRIBER_DB_PATH=//p' "$SPLIT6_ENV_FILE" | tail -n 1)"
fi
if [[ -f "$LOCAL_SUBSCRIBER_DB" ]] &&
   { [[ -z "$configured_subscriber_db" ]] || [[ "$configured_subscriber_db" == "../configs/subscriber_db.csv" ]]; }; then
  export OPEN5GS_SUBSCRIBER_DB_PATH="../configs/subscriber_db.local.csv"
  echo "Using local subscriber DB: $LOCAL_SUBSCRIBER_DB"
fi

resolved_subscriber_db="${OPEN5GS_SUBSCRIBER_DB_PATH:-${configured_subscriber_db:-../configs/subscriber_db.csv}}"
if [[ "$resolved_subscriber_db" != /* ]]; then
  resolved_subscriber_db="$COMPOSE_DIR/$resolved_subscriber_db"
fi
if [[ ! -f "$resolved_subscriber_db" ]]; then
  echo "Warning: subscriber DB not found at $resolved_subscriber_db. 5GC may fail to start."
fi

if [[ "${1:-}" == "dev" ]]; then
  DEV_MODE=true
  COMPOSE_PROFILE="dev"
  shift
fi

cmd="${1:-build}"
case "$cmd" in
  build)
    if [[ "$DEV_MODE" == true ]]; then
      echo "Building split-6 dev image (context: ocudu, Dockerfile.split6.dev)..."
    else
      echo "Building 5GC (ocudu/open5gs-5gc), gNB (ocudu/gnb), and Aerial L1 locally..."
    fi
    (cd "$REPO_ROOT" && docker compose --env-file "$SPLIT6_ENV_FILE" -f "$COMPOSE_FILE" --profile "$COMPOSE_PROFILE" build)
    ;;
  up)
    echo "Building and starting split-6 stack (profile: $COMPOSE_PROFILE)..."
    (cd "$REPO_ROOT" && docker compose --env-file "$SPLIT6_ENV_FILE" -f "$COMPOSE_FILE" --profile "$COMPOSE_PROFILE" up -d --build)
    echo "Containers started. Check: docker compose -f $COMPOSE_FILE --profile $COMPOSE_PROFILE logs -f"
    ;;
  down)
    (cd "$REPO_ROOT" && docker compose --env-file "$SPLIT6_ENV_FILE" -f "$COMPOSE_FILE" --profile prod --profile dev down)
    ;;
  logs)
    (cd "$REPO_ROOT" && docker compose --env-file "$SPLIT6_ENV_FILE" -f "$COMPOSE_FILE" --profile prod --profile dev logs -f)
    ;;
  exec)
    if [[ "$DEV_MODE" != true ]]; then
      echo "Error: exec is only available in dev mode. Use: $0 dev exec"
      exit 1
    fi
    docker exec -it ocudu_gnb_dev bash
    ;;
  aerial)
    if [[ "$DEV_MODE" != true ]]; then
      echo "Error: aerial is only available in dev mode. Use: $0 dev aerial"
      exit 1
    fi
    docker exec -it ocudu_aerial_dev bash
    ;;
  *)
    echo "Usage: $0 [build|up|down|logs]"
    echo "       $0 dev [build|up|down|logs|exec|aerial]   # dev image + host-mounted ocudu/build"
    exit 1
    ;;
esac
