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

import numpy as np
import json
import torch
import triton_python_backend_utils as pb_utils
import datetime
import time

def get_timestamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")

class TritonPythonModel:
    """
    This model consumes I/Q data on the GPU and computes the average power
    for each of the 273 resource blocks using PyTorch.
    """
    def initialize(self, args):
        """
        Called once at server start to initialize the model.
        """
        self.model_config = json.loads(args['model_config'])
        # Check if CUDA is available, fallback to CPU if not
        self.device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
        print(f"PRB Power Model '{self.model_config['name']}' initialized on device: {self.device}")

    def execute(self, requests):
        """
        This function is called for every inference request.
        """
        responses = []
        for request in requests:
            try:
                start_time = time.perf_counter()
                
                # 1. Get the input tensor and check its location
                input_tensor = pb_utils.get_input_tensor_by_name(request, "iq_samples")
                
                step1_time = time.perf_counter()
                
                # 2. Convert to PyTorch tensor using DLPack (stays on GPU)
                torch_tensor = torch.from_dlpack(input_tensor.to_dlpack())
                
                # Debug: Check device before and after
                device_before = str(torch_tensor.device)
                torch_tensor = torch_tensor.to(self.device)  # Ensure on GPU
                device_after = str(torch_tensor.device)
                
                # Convert FP16 to FP32 on GPU for computation
                torch_tensor = torch_tensor.float()
                
                step2_time = time.perf_counter()
                
                # DEBUG: Print tensor device location (first few requests only)
                if hasattr(self, 'debug_count'):
                    self.debug_count += 1
                else:
                    self.debug_count = 1
                    
                if self.debug_count <= 3:
                    print(f"DEBUG: Input tensor device: {torch_tensor.device}")
                    print(f"DEBUG: Tensor shape: {torch_tensor.shape}, dtype: {torch_tensor.dtype}")

                # 4. Calculate power (I^2 + Q^2) on the GPU
                power = torch.square(torch_tensor[..., 0]) + torch.square(torch_tensor[..., 1])

                step3_time = time.perf_counter()

                # 5. Average the power across antennas, symbols, and subcarriers
                # power shape: (4, 14, 273, 12) -> output shape: (273,)
                # Average over: antennas(0), symbols(1), subcarriers_per_prb(3)
                average_power = power.mean(dim=(0, 1, 3))

                step4_time = time.perf_counter()

                # 6. Create the output tensor (keep on GPU if possible)
                try:
                    # Try to use DLPack to avoid CPU transfer
                    output_tensor = pb_utils.Tensor.from_dlpack(
                        "prb_power", torch.to_dlpack(average_power))
                except:
                    # Fallback: convert to CPU numpy array
                    output_tensor = pb_utils.Tensor(
                        "prb_power", average_power.cpu().numpy())

                step5_time = time.perf_counter()

                # Calculate timing breakdown
                total_us = (step5_time - start_time) * 1_000_000
                step1_us = (step1_time - start_time) * 1_000_000
                step2_us = (step2_time - step1_time) * 1_000_000
                step3_us = (step3_time - step2_time) * 1_000_000
                step4_us = (step4_time - step3_time) * 1_000_000
                step5_us = (step5_time - step4_time) * 1_000_000
                # Include device info in timing array (encode as numbers for simplicity)
                device_before_code = 0 if 'cpu' in device_before else 1
                device_after_code = 0 if 'cpu' in device_after else 1
                # Also encode self.device for debugging (0=cpu, 1=cuda, 2=other)
                self_device_code = 0 if 'cpu' in str(self.device) else (1 if 'cuda' in str(self.device) else 2)
                timing_array = np.array([total_us, step1_us, step2_us, step3_us, step4_us, step5_us, device_before_code, device_after_code, self_device_code], dtype=np.float64)

                timing_output_tensor = pb_utils.Tensor("timing_us", timing_array)

                # 7. Create and send the response
                inference_response = pb_utils.InferenceResponse(
                    output_tensors=[output_tensor, timing_output_tensor]
                )
                responses.append(inference_response)
                
                end_time = time.perf_counter()
                
                # Print timing breakdown for first few requests
                if self.debug_count <= 3:
                    total_us = (end_time - start_time) * 1_000_000
                    step1_us = (step1_time - start_time) * 1_000_000
                    step2_us = (step2_time - step1_time) * 1_000_000
                    step3_us = (step3_time - step2_time) * 1_000_000
                    step4_us = (step4_time - step3_time) * 1_000_000
                    step5_us = (step5_time - step4_time) * 1_000_000
                    
                    print(f"TIMING BREAKDOWN (Request {self.debug_count}):")
                    print(f"  Step 1 (Get tensor): {step1_us:.1f} μs")
                    print(f"  Step 2 (DLPack + to GPU + FP32): {step2_us:.1f} μs")
                    print(f"  Step 3 (Power calc): {step3_us:.1f} μs")
                    print(f"  Step 4 (Mean): {step4_us:.1f} μs")
                    print(f"  Step 5 (Output tensor): {step5_us:.1f} μs")
                    print(f"  TOTAL: {total_us:.1f} μs")

            except Exception as e:
                error = pb_utils.TritonError(f"{self.model_config['name']} error: {e}")
                inference_response = pb_utils.InferenceResponse(error=error)
                responses.append(inference_response)

        return responses

    def finalize(self):
        """
        Called once when the model is unloaded.
        """
        print(f"PRB Power Model '{self.model_config['name']}' finalizing") 