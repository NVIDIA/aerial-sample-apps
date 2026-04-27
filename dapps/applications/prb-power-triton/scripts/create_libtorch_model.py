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

import torch
import torch.nn as nn
import os

class PowerCalculator(nn.Module):
    def __init__(self):
        super(PowerCalculator, self).__init__()

    def forward(self, input_tensor: torch.Tensor) -> torch.Tensor:
        """
        Calculates the average power for each resource block.
        Input shape: [4, 14, 273, 12, 2] (antennas, symbols, PRBs, subcarriers, I/Q)
        Output shape: [273] (PRB power)
        
        Unified approach: All models receive FP16 from E3 Manager
        """
        # Simple conversion: FP16 → FP32 (E3 Manager provides FP16)
        input_fp32 = input_tensor.float()
            
        # Calculate power (I^2 + Q^2)
        power = torch.square(input_fp32[..., 0]) + torch.square(input_fp32[..., 1])
        
        # Average across antennas(0), symbols(1), subcarriers_per_prb(3) to get PRB power
        # power shape: (4, 14, 273, 12) -> output shape: (273,)
        average_power = power.mean(dim=(0, 1, 3))
        
        return average_power

def create_config(base_dir="/models"):
    """Create the config.pbtxt file for PyTorch LibTorch model"""
    config_content = '''name: "prb_power_libtorch"
platform: "pytorch_libtorch"
default_model_filename: "model.pt"

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
    
    model_name = "prb_power_libtorch"
    config_dir = os.path.join(base_dir, model_name)
    os.makedirs(config_dir, exist_ok=True)
    config_path = os.path.join(config_dir, "config.pbtxt")
    
    with open(config_path, 'w') as f:
        f.write(config_content)
    
    print(f"Created config file: {config_path}")
    return config_path

def create_pytorch_model():
    """Creates and saves a PyTorch LibTorch model and its config."""
    model_name = "prb_power_libtorch"
    # Use absolute path for Triton's model repository
    base_dir = "/models"
    model_dir = os.path.join(base_dir, model_name)
    version_dir = os.path.join(model_dir, "1")
    os.makedirs(version_dir, exist_ok=True)

# --- Main execution ---
if __name__ == "__main__":
    print("Creating PyTorch LibTorch model and config...")
    
    base_dir = "/models"
    model_name = "prb_power_libtorch"
    model_version_dir = os.path.join(base_dir, model_name, "1")
    os.makedirs(model_version_dir, exist_ok=True)

    # Create config file
    create_config(base_dir)

    # Instantiate the model and set to evaluation mode
    model = PowerCalculator().eval()
    
    # Try GPU first, fallback to CPU if no GPU available
    try:
        model = model.to("cuda")
        # Create dummy input with correct shape [4, 14, 273, 12, 2] and FP16 dtype (unified approach)
        dummy_input = torch.randn(4, 14, 273, 12, 2, dtype=torch.float16).to("cuda")
        print("Using GPU for model creation")
    except:
        print("GPU not available, using CPU for model creation")
        dummy_input = torch.randn(4, 14, 273, 12, 2, dtype=torch.float16)

    # Use torch.jit.trace to convert the model to TorchScript
    scripted_model = torch.jit.trace(model, dummy_input)

    # Define the output directory and file path
    output_path = os.path.join(model_version_dir, "model.pt")

    # Save the scripted model
    scripted_model.save(output_path)

    print(f"Successfully saved TorchScript model to: {output_path}")
    print("PyTorch LibTorch model is ready!") 