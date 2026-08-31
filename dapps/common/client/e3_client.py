#!/usr/bin/env python3
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
# E3 dApp Manager client. Sends commands to the dApp E3 Manager
# for granular control of E3 Agent lifecycle and subscriptions.
#
# Usage:
#   e3_client.py status                                          # Show agent states and subscriptions
#   e3_client.py list-agents                                     # Show configured agents and ports
#   e3_client.py setup                                           # Setup all agents
#   e3_client.py setup -a NVIDIA_L1                              # Setup specific agent
#   e3_client.py release -a NVIDIA_L1                            # Release specific agent
#   e3_client.py subscribe -a NVIDIA_L1 -t 1,4,5,6               # Subscribe with default periodicity
#   e3_client.py subscribe -a NVIDIA_L1 -t 1,3,5,6,7,11 -p 0     # Subscribe every slot
#   e3_client.py unsubscribe -a NVIDIA_L1                        # Delete active subscription
#   e3_client.py --host 10.0.0.1 --port 5558 status              # Remote host

import zmq
import json
import argparse
import sys

COMMANDS = ["status", "list-agents", "setup", "release", "subscribe", "unsubscribe"]

CMD_MAP = {
    "status":      "status",
    "list-agents": "list_agents",
    "setup":       "e3_setup",
    "release":     "e3_release",
    "subscribe":   "subscribe",
    "unsubscribe": "unsubscribe",
}

def build_request(args):
    cmd = args.command
    request = {"cmd": CMD_MAP[cmd]}

    if cmd == "subscribe":
        if not args.agent:
            print("Error: subscribe requires -a/--agent")
            sys.exit(1)
        if not args.telemetry_ids:
            print("Error: subscribe requires -t/--telemetry-ids")
            sys.exit(1)

        try:
            ids = [int(x.strip()) for x in args.telemetry_ids.split(",")]
        except ValueError:
            print("Error: telemetry IDs must be comma-separated integers")
            sys.exit(1)

        request["agent"] = args.agent
        request["telemetryIdentifierList"] = ids
        request["ranFunctionIdentifier"] = args.ran_function_id
        request["periodicity"] = args.periodicity
        request["subscriptionTime"] = args.subscription_time
        if args.model:
            request["model_name"] = args.model

    elif cmd == "unsubscribe":
        if not args.agent:
            print("Error: unsubscribe requires -a/--agent")
            sys.exit(1)
        request["agent"] = args.agent

    elif cmd in ("setup", "release"):
        if args.agent:
            request["agent"] = args.agent

    return request


def main():
    parser = argparse.ArgumentParser(
        description="E3 dApp Client. Sends commands to the dApp E3 Manager for granular control of E3 Agent lifecycle and subscriptions",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
telemetry IDs (-t):
  See docs/application_development_guide.md (Available Data Streams) for the
  full list of telemetry IDs.

typical workflow:
  %(prog)s list-agents                                             # 1. discover agents
  %(prog)s setup                                                   # 2. setup all agents
  %(prog)s status                                                  # 3. verify connected
  %(prog)s subscribe -a NVIDIA_L1 -t 1,4,5,6                       # 4. start indications
  %(prog)s status                                                  # 5. verify subscription
  %(prog)s unsubscribe -a NVIDIA_L1                                # 6. stop indications
  %(prog)s release -a NVIDIA_L1                                    # 7. release agent

more examples:
  %(prog)s subscribe -a NVIDIA_L1 -t 3,4,5,6,11,12,13,14           # H-estimates + metadata
  %(prog)s subscribe -a NVIDIA_L1 -t 1,4,5,6 -p 0 -s 10            # every slot, 10s TTL
  %(prog)s subscribe -a NVIDIA_L1 -t 1,5,6 -m prb_power_numpy      # specific model
  %(prog)s setup                                                   # setup all agents
  %(prog)s release                                                 # release all agents""",
    )

    parser.add_argument("command", choices=COMMANDS,
                        help="status | list-agents | setup | release | subscribe | unsubscribe")
    parser.add_argument("-a", "--agent", default="",
                        help="target E3 Agent name, e.g. NVIDIA_L1 (required for subscribe/unsubscribe, default: all for setup/release)")
    parser.add_argument("-t", "--telemetry-ids", default=None,
                        help="comma-separated telemetry IDs to subscribe (required for subscribe)")
    parser.add_argument("-p", "--periodicity", type=int, default=100000,
                        help="indication periodicity in microseconds (default: 100000 = 100ms, 0 = every slot)")
    parser.add_argument("-s", "--subscription-time", type=int, default=5,
                        help="subscription TTL in seconds, 0 = indefinite (default: 5)")
    parser.add_argument("-m", "--model", default=None,
                        help="inference model name (default: from e3_config.json)")
    parser.add_argument("-r", "--ran-function-id", type=int, default=2,
                        help="RAN Function Identifier (default: 2 = NVIDIA KPM)")
    parser.add_argument("--host", default="127.0.0.1",
                        help="E3 Manager host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=5558,
                        help="E3 Manager dApp client port (default: 5558)")
    parser.add_argument("--timeout", type=int, default=5000,
                        help="receive timeout in ms (default: 5000)")

    args = parser.parse_args()
    request = build_request(args)

    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.connect(f"tcp://{args.host}:{args.port}")
    socket.setsockopt(zmq.RCVTIMEO, args.timeout)
    socket.setsockopt(zmq.LINGER, 0)

    print(f"Sending: {json.dumps(request)}")
    socket.send_string(json.dumps(request))

    try:
        response = socket.recv_json()
        print(f"Response: {json.dumps(response, indent=2)}")
        status = response.get("status", "")
        sys.exit(0 if status in ("ok", "sent") else 1)
    except zmq.Again:
        print("Error: timeout waiting for response")
        sys.exit(1)
    finally:
        socket.close()
        context.term()


if __name__ == "__main__":
    main()
