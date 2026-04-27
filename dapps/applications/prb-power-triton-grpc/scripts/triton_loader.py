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

"""
Script to load models into a running Triton Inference Server
"""
import argparse
import requests
import os
import sys
from typing import List

def get_triton_status(triton_url: str = "http://localhost:8000") -> bool:
    """Check if Triton server is running and accessible."""
    try:
        response = requests.get(f"{triton_url}/v2/health/ready", timeout=5)
        return response.status_code == 200
    except Exception as e:
        print(f"ERROR: Cannot connect to Triton server at {triton_url}: {e}")
        return False

def get_available_models(model_repository: str = "/models") -> List[str]:
    """Get list of available models in the model repository."""
    models = []
    
    if not os.path.exists(model_repository):
        print(f"ERROR: Model repository not found: {model_repository}")
        return models
    
    try:
        for item in os.listdir(model_repository):
            model_path = os.path.join(model_repository, item)
            if os.path.isdir(model_path):
                # Check if it has config.pbtxt and version directory
                config_file = os.path.join(model_path, "config.pbtxt")
                version_dir = os.path.join(model_path, "1")
                if os.path.exists(config_file) and os.path.exists(version_dir):
                    models.append(item)
    except Exception as e:
        print(f"ERROR: Error reading model repository: {e}")
    
    return models

def check_model_status(model_name: str, triton_url: str = "http://localhost:8000") -> str:
    """Check the status of a specific model."""
    try:
        response = requests.get(f"{triton_url}/v2/models/{model_name}", timeout=5)
        if response.status_code == 200:
            return "LOADED"
        elif response.status_code == 400 or response.status_code == 404:
            return "NOT LOADED"
        else:
            return "UNKNOWN"
    except Exception:
        return "UNKNOWN"

def load_model(model_name: str, triton_url: str = "http://localhost:8000") -> bool:
    """Load a specific model into Triton server."""
    load_url = f"{triton_url}/v2/repository/models/{model_name}/load"
    
    try:
        print(f"Loading model: {model_name}")
        response = requests.post(load_url, timeout=30)
        
        if response.status_code == 200:
            print(f"SUCCESS: Successfully loaded: {model_name}")
            return True
        else:
            print(f"ERROR: Failed to load {model_name}: HTTP {response.status_code}")
            try:
                error_data = response.json()
                print(f"   Error details: {error_data.get('error', 'Unknown error')}")
            except:
                print(f"   Response: {response.text}")
            return False
            
    except requests.exceptions.Timeout:
        print(f"ERROR: Timeout loading {model_name} (took more than 30 seconds)")
        return False
    except Exception as e:
        print(f"ERROR: Error loading {model_name}: {e}")
        return False

def unload_model(model_name: str, triton_url: str = "http://localhost:8000") -> bool:
    """Unload a specific model from Triton server."""
    unload_url = f"{triton_url}/v2/repository/models/{model_name}/unload"
    
    try:
        print(f"Unloading model: {model_name}")
        response = requests.post(unload_url, timeout=30)
        
        if response.status_code == 200:
            print(f"SUCCESS: Successfully unloaded: {model_name}")
            return True
        else:
            print(f"ERROR: Failed to unload {model_name}: HTTP {response.status_code}")
            try:
                error_data = response.json()
                print(f"   Error details: {error_data.get('error', 'Unknown error')}")
            except:
                print(f"   Response: {response.text}")
            return False
            
    except Exception as e:
        print(f"ERROR: Error unloading {model_name}: {e}")
        return False

def show_model_status(triton_url: str = "http://localhost:8000", model_repository: str = "/models"):
    """Show status of all models (available vs loaded)."""
    print("=" * 60)
    print("TRITON MODEL STATUS")
    print("=" * 60)
    
    available_models = get_available_models(model_repository)
    
    if not available_models:
        print("ERROR: No models found in repository")
        return
    
    print(f"Model Repository: {model_repository}")
    print(f"Triton Server: {triton_url}")
    print()
    
    print("MODEL STATUS:")
    print("-" * 40)
    
    loaded_count = 0
    for model in sorted(available_models):
        status = check_model_status(model, triton_url)
        if status == "LOADED":
            loaded_count += 1
        print(f"  {model:<30} {status}")
    
    print()
    print(f"Total Available: {len(available_models)}")
    print(f"Total Loaded: {loaded_count}")

def main():
    parser = argparse.ArgumentParser(description='Load/unload models in Triton Inference Server')
    parser.add_argument('--model-name', '-m', help='Specific model name to load')
    parser.add_argument('--all', '-a', action='store_true', help='Load all available models')
    parser.add_argument('--unload', '-u', help='Unload specific model')
    parser.add_argument('--status', '-s', action='store_true', help='Show model status')
    parser.add_argument('--triton-url', default='http://localhost:8000', 
                       help='Triton server URL (default: http://localhost:8000)')
    parser.add_argument('--model-repository', default='/models',
                       help='Model repository path (default: /models)')
    
    args = parser.parse_args()
    
    # Check Triton server connectivity
    if not get_triton_status(args.triton_url):
        print("ERROR: Triton server is not accessible. Please check if it's running.")
        sys.exit(1)
    
    print(f"Connected to Triton server at {args.triton_url}")
    
    # Handle different operations
    if args.status:
        show_model_status(args.triton_url, args.model_repository)
        return
    
    if args.unload:
        success = unload_model(args.unload, args.triton_url)
        sys.exit(0 if success else 1)
    
    if args.model_name:
        # Load specific model
        success = load_model(args.model_name, args.triton_url)
        sys.exit(0 if success else 1)
    
    if args.all:
        # Load all available models
        available_models = get_available_models(args.model_repository)
        if not available_models:
            print("ERROR: No models found in repository")
            sys.exit(1)
        
        # Check which models need to be loaded
        models_to_load = []
        for model in available_models:
            status = check_model_status(model, args.triton_url)
            if status != "LOADED":
                models_to_load.append(model)
        
        if not models_to_load:
            print("INFO: All available models are already loaded")
            show_model_status(args.triton_url, args.model_repository)
            return
        
        print(f"Loading {len(models_to_load)} models...")
        success_count = 0
        for model in models_to_load:
            if load_model(model, args.triton_url):
                success_count += 1
        
        print(f"\nResults: {success_count}/{len(models_to_load)} models loaded successfully")
        
        # Show final status
        print()
        show_model_status(args.triton_url, args.model_repository)
        return
    
    # No specific action requested, show help
    parser.print_help()

if __name__ == "__main__":
    main()