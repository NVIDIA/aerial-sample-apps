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

# e3_e2e_test.py - End-to-end E3 Manager test
import zmq
import json
import time
import sys
import argparse

def wait_for_enter(interactive_mode):
    """If in interactive mode, waits for the user to press Enter."""
    if interactive_mode:
        input("  Press Enter to continue to the next step...")

def test_e3_dapp_lifecycle(args):
    """Main test function to test the full dApp lifecycle with E3SM protocol."""
    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.connect(f"tcp://{args.host}:{args.port}")
    socket.setsockopt(zmq.RCVTIMEO, 5000)

    print(f"--- Testing Full dApp Lifecycle for model: {args.model or '(server default)'} ---")
    if args.agent:
        print(f"--- Target agent: {args.agent} ---")
    if args.interactive:
        print("--- Running in INTERACTIVE mode ---")


    # 1. List available agents
    print("\n[Step 1] Listing available E3 Agents...")
    list_request = {"cmd": "list_agents"}
    socket.send_string(json.dumps(list_request))
    try:
        response = socket.recv_json()
        print(f"  Response: {json.dumps(response, indent=2)}")
        assert response.get("status") == "ok"
        print("  ✓ Agent listing PASSED")
    except Exception as e:
        print(f"  ✗ Agent listing FAILED: {e}")
        return

    wait_for_enter(args.interactive)

    # 2. Initial Status Check
    print("\n[Step 2] Checking initial E3 Manager status...")
    status_request = {"cmd": "status"}
    socket.send_string(json.dumps(status_request))
    try:
        response = socket.recv_json()
        print(f"  Response: {json.dumps(response, indent=2)}")
        assert response.get("status") == "ok"
        print("  ✓ Initial status check PASSED")
    except Exception as e:
        print(f"  ✗ Initial status check FAILED: {e}")
        return

    wait_for_enter(args.interactive)

    # 3. Subscribe dApp
    agent_info = f" to agent '{args.agent}'" if args.agent else ""
    model_info = args.model if args.model else "(server default)"
    print(f"\n[Step 3] Subscribing this manager to model='{model_info}'{agent_info} with telemetry IDs={args.telemetry_ids}...")
    subscribe_request = {
        "cmd": "subscribe",
        "telemetryIdentifierList": args.telemetry_ids,
        "ranFunctionIdentifier": args.ran_function_id,
        "periodicity": args.periodicity,
        "subscriptionTime": args.subscription_time
    }
    if args.model:
        subscribe_request["model_name"] = args.model
    if args.agent:
        subscribe_request["agent"] = args.agent
    
    print(f"  Sending request: {json.dumps(subscribe_request)}")
    socket.send_string(json.dumps(subscribe_request))
    try:
        response = socket.recv_json()
        print(f"  Response: {response}")
        assert response.get("status") == "sent"
        print("  ✓ Subscription request sent")
    except Exception as e:
        print(f"  ✗ Subscription FAILED: {e}")
        return

    wait_for_enter(args.interactive)

    # 4. Wait to simulate data flow
    if args.interactive:
        print(f"\n[Step 4] Data should be flowing (check e3_manager logs)...")
    else:
        print(f"\n[Step 4] Data should be flowing. Waiting {args.wait_time} seconds (check e3_manager logs)...")
        time.sleep(args.wait_time)

    wait_for_enter(args.interactive)

    # 5. Check Status After Subscription
    print("\n[Step 5] Checking status after subscription...")
    socket.send_string(json.dumps(status_request))
    try:
        response = socket.recv_json()
        print(f"  Response: {json.dumps(response, indent=2)}")
        # Check if any agent has a confirmed subscription
        agents = response.get("agents", [])
        any_confirmed = any(agent.get("subscription_status") == "confirmed" 
                           for agent in agents)
        assert any_confirmed, "No agent has a confirmed subscription"
        print("  ✓ Status shows at least one agent with confirmed subscription")
    except Exception as e:
        print(f"  ✗ Status check FAILED: {e}")

    wait_for_enter(args.interactive)

    # 6. Unsubscribe dApp
    print(f"\n[Step 6] Unsubscribing this manager{agent_info}...")
    unsubscribe_request = {"cmd": "unsubscribe"}
    if args.agent:
        unsubscribe_request["agent"] = args.agent
        
    socket.send_string(json.dumps(unsubscribe_request))
    try:
        response = socket.recv_json()
        print(f"  Response: {response}")
        assert response.get("status") == "sent"
        print("  ✓ Subscription delete request sent")
    except Exception as e:
        print(f"  ✗ Subscription delete FAILED: {e}")
        return

    if args.interactive:
        print("\n -> Data flow should now be stopped.")
    else:
        print("\n -> Waiting for delete confirmation...")
        time.sleep(2)

    wait_for_enter(args.interactive)

    # 7. Final Status Check
    print("\n[Step 7] Checking status after unsubscription...")
    socket.send_string(json.dumps(status_request))
    try:
        response = socket.recv_json()
        print(f"  Response: {json.dumps(response, indent=2)}")
        # Check if no agent is subscribed
        agents = response.get("agents", [])
        any_subscribed = any(agent.get("is_subscribed", False) 
                            for agent in agents)
        assert not any_subscribed, "Some agent is still subscribed"
        print("  ✓ Status shows no agent is subscribed")
    except Exception as e:
        print(f"  ✗ Final status check FAILED: {e}")

    print("\n--- Test Complete ---")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Test Multi-Agent E3 Protocol communication with E3 Manager")
    parser.add_argument('--host', default='localhost', help='E3 Manager host')
    parser.add_argument('--port', default=5558, type=int, help='E3 Manager port')
    parser.add_argument('-a', '--agent', help='Specific E3 Agent name to target (e.g., NVIDIA_L1)')
    parser.add_argument('-m', '--model', default=None, help='Model to request for subscription (omit to use server default)')
    parser.add_argument('-t', '--telemetry-ids', default='1,4,5,6', help='Comma-separated telemetry IDs to subscribe to (e.g., 1=iq_samples, 4=timestamp, 5=sfn, 6=slot)')
    parser.add_argument('-r', '--ran-function-id', default=2, type=int, help='RAN Function Identifier (default: 2 = NVIDIA KPM)')
    parser.add_argument('-p', '--periodicity', dest='periodicity', default=100000, type=int, help='Periodicity in microseconds for E3 indications (default: 100000 = 100ms)')
    parser.add_argument('-d', '--duration', dest='wait_time', default=5, type=int, help='seconds to wait before sending subscription delete (default: 5)')
    parser.add_argument('-s', '--subscription-time', dest='subscription_time', default=0, type=int, help='server-side subscription TTL in seconds, 0 = indefinite (default: 0)')
    parser.add_argument('-i', '--interactive', action='store_true', help='Wait for user input between steps')

    args = parser.parse_args()
    
    # Parse comma-separated telemetry IDs into a list of integers
    args.telemetry_ids = [int(tid.strip()) for tid in args.telemetry_ids.split(',')]
    
    test_e3_dapp_lifecycle(args) 