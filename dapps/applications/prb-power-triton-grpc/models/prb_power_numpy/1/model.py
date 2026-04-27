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
import time
import triton_python_backend_utils as pb_utils

class TritonPythonModel:
    """
    This model consumes I/Q data and computes the average power for each
    of the 273 resource blocks.
    """
    def initialize(self, args):
        """
        Called once at server start to initialize the model.
        """
        self.model_config = json.loads(args['model_config'])
        print(f"PRB Power Model '{self.model_config['name']}' initialized")

    def execute(self, requests):
        """
        This function is called for every inference request.
        """
        responses = []
        for request in requests:
            try:
                t0 = time.perf_counter()

                # 1. Get the input tensor by name
                input_tensor = pb_utils.get_input_tensor_by_name(request, "iq_samples")
                input_data = input_tensor.as_numpy()

                # 2. Convert FP16 to FP32 for CPU computation
                # Note: numpy treats FP16 as float16, ensure proper conversion
                if input_data.dtype == np.float16:
                    input_data_fp32 = input_data.astype(np.float32)
                else:
                    input_data_fp32 = input_data.astype(np.float32)

                t1 = time.perf_counter()

                # 3. Calculate power (I^2 + Q^2)
                power = np.square(input_data_fp32[..., 0]) + np.square(input_data_fp32[..., 1])

                # 4. Average the power across antennas, symbols, and subcarriers
                #    to get a single value per resource block.
                # power shape: (4, 14, 273, 12) -> output shape: (273,)
                # Average over: antennas(0), symbols(1), subcarriers_per_prb(3)
                average_power = power.mean(axis=(0, 1, 3))

                t2 = time.perf_counter()

                total_us = (t2 - t0) * 1_000_000
                compute_us = (t2 - t1) * 1_000_000

                # 5. Create the output tensors
                output_tensor = pb_utils.Tensor("prb_power", average_power)
                timing_tensor = pb_utils.Tensor(
                    "timing_us",
                    np.array([total_us, compute_us], dtype=np.float64))

                # 6. Create and send the response
                inference_response = pb_utils.InferenceResponse(
                    output_tensors=[output_tensor, timing_tensor])
                responses.append(inference_response)

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