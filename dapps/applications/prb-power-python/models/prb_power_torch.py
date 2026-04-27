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
# PRB Power model using PyTorch (GPU-accelerated).
#
# Same computation as prb_power_numpy.py but runs on CUDA if available.
#
# Input:  iq_samples  — shape (4, 14, 273, 12, 2), dtype float16
#         [antennas, symbols, PRBs, subcarriers, I/Q]
# Output: prb_power   — shape (273,), dtype float32
#         timing_us   — shape (2,),   dtype float64
#                       [total_us, compute_us]
#

import numpy as np
import time

try:
    import torch
    TORCH_AVAILABLE = True
except ImportError:
    TORCH_AVAILABLE = False


class InferenceModel:
    """PyTorch-based PRB power computation (GPU if available, CPU fallback)."""

    def initialize(self):
        if not TORCH_AVAILABLE:
            raise RuntimeError("PyTorch is required for prb_power_torch model")
        self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        print(f"prb_power_torch initialized on device: {self.device}")

    def get_metadata(self):
        return {
            "name": "prb_power_torch",
            "inputs": [
                {"name": "iq_samples", "type": "FP16", "shape": [4, 14, 273, 12, 2]}
            ],
            "outputs": [
                {"name": "prb_power", "type": "FP32", "shape": [273]},
                {"name": "timing_us", "type": "FP64", "shape": [2]}
            ]
        }

    def infer(self, inputs):
        start = time.perf_counter()

        iq = torch.from_numpy(inputs["iq_samples"]).to(self.device).float()

        compute_start = time.perf_counter()
        power = torch.square(iq[..., 0]) + torch.square(iq[..., 1])
        prb_power = power.mean(dim=(0, 1, 3))
        if self.device.type == "cuda":
            torch.cuda.synchronize()
        compute_end = time.perf_counter()

        total_us = (compute_end - start) * 1_000_000
        compute_us = (compute_end - compute_start) * 1_000_000

        return {
            "prb_power": prb_power.cpu().numpy(),
            "timing_us": np.array([total_us, compute_us], dtype=np.float64)
        }

    def finalize(self):
        pass
