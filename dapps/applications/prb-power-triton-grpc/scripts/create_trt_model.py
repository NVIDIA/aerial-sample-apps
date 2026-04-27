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

import tensorrt as trt
import os
import numpy as np

def create_config(base_dir="/models"):
    """Create the config.pbtxt file for TensorRT model"""
    config_content = '''name: "prb_power_trt"
platform: "tensorrt_plan"
default_model_filename: "model.plan"

input [
  {
    name: "iq_samples"
    data_type: TYPE_FP16
    dims: [ 4, 14, 273, 12, 2 ]
  }
]

output [
  {
    name: "prb_power"
    data_type: TYPE_FP32
    dims: [ 273 ]
  }
]

instance_group [
  {
    count: 1
    kind: KIND_GPU
  }
]'''
    
    model_name = "prb_power_trt"
    config_dir = os.path.join(base_dir, model_name)
    os.makedirs(config_dir, exist_ok=True)
    config_path = os.path.join(config_dir, "config.pbtxt")
    
    with open(config_path, 'w') as f:
        f.write(config_content)
    
    print(f"Created config file: {config_path}")
    return config_path

def create_tensorrt_engine(base_dir="/models"):
    """
    Create a TensorRT engine from the ONNX model
    """
    print("Creating TensorRT engine from ONNX model...")
    
    # Define paths using absolute base directory
    onnx_model_name = "prb_power_onnx"
    tensorrt_model_name = "prb_power_trt"
    
    onnx_path = os.path.join(base_dir, onnx_model_name, "1", "model.onnx")
    output_dir = os.path.join(base_dir, tensorrt_model_name, "1")
    engine_path = os.path.join(output_dir, "model.plan")
    
    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    
    # Check if ONNX model exists
    if not os.path.exists(onnx_path):
        print(f"ONNX model not found at {onnx_path}. Creating it first...")
        script_dir = os.path.dirname(os.path.abspath(__file__))
        import subprocess, sys
        result = subprocess.run(
            [sys.executable, os.path.join(script_dir, "create_onnx_model.py")],
            check=True
        )
        if not os.path.exists(onnx_path):
            raise FileNotFoundError(f"ONNX model still not found at {onnx_path} after running create_onnx_model.py")
    
    # Create TensorRT logger and builder
    TRT_LOGGER = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(TRT_LOGGER)
    config = builder.create_builder_config()
    
    # Set memory pool size (2GB)
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 2 << 30)
    
    # Enable FP16 precision
    config.set_flag(trt.BuilderFlag.FP16)
    
    # Create network definition
    network = builder.create_network(1 << int(trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH))
    
    # Create an optimization profile
    profile = builder.create_optimization_profile()
    
    # Define the expected input dimensions for the input tensor 'iq_samples'
    # Format: [4, 14, 273, 12, 2] (antennas, symbols, PRBs, subcarriers, I/Q)
    # Static optimization profile to match static ONNX model (273 PRBs)
    min_shape = (4, 14, 273, 12, 2)   # Static: 273 PRBs
    opt_shape = (4, 14, 273, 12, 2)   # Static: 273 PRBs
    max_shape = (4, 14, 273, 12, 2)   # Static: 273 PRBs
    profile.set_shape("iq_samples", min_shape, opt_shape, max_shape)
    config.add_optimization_profile(profile)
    
    # Parse ONNX model
    parser = trt.OnnxParser(network, TRT_LOGGER)
    
    with open(onnx_path, 'rb') as model:
        if not parser.parse(model.read()):
            print("Failed to parse ONNX model")
            for error in range(parser.num_errors):
                print(parser.get_error(error))
            return False
    
    # Build the engine
    print("Building TensorRT engine... This may take a few minutes.")
    serialized_engine = builder.build_serialized_network(network, config)
    
    if serialized_engine is None:
        print("Failed to build TensorRT engine")
        return False
    
    # Save the engine
    with open(engine_path, 'wb') as f:
        f.write(serialized_engine)
    
    print(f"Successfully created TensorRT engine at: {engine_path}")
    return True

if __name__ == "__main__":
    print("Creating TensorRT model and config...")
    
    base_dir = "/models"
    
    # Create config file
    create_config(base_dir)
    
    # Create the engine
    success = create_tensorrt_engine(base_dir)
    if not success:
        exit(1)
    
    print("TensorRT model is ready!") 