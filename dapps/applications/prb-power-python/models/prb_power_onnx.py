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
# PRB Power model using ONNX Runtime (GPU).
#
# On first initialize(), exports the PRB power computation as an ONNX model
# via torch.onnx.export and loads it with ONNX Runtime. The artifact is
# cached in /tmp so subsequent calls within the same container are instant.
#
# Input:  iq_samples  — shape (4, 14, 273, 12, 2), dtype float16
#         [antennas, symbols, PRBs, subcarriers, I/Q]
# Output: prb_power   — shape (273,), dtype float32
#         timing_us   — shape (2,),   dtype float64
#                       [total_us, compute_us]
#

import numpy as np
import time
import os

try:
    import onnxruntime as ort
    ORT_AVAILABLE = True
except ImportError:
    ORT_AVAILABLE = False

try:
    import torch
    import torch.nn as nn
    TORCH_AVAILABLE = True
except ImportError:
    TORCH_AVAILABLE = False

CACHE_DIR = "/tmp/prb_power_models"
ONNX_PATH = os.path.join(CACHE_DIR, "prb_power.onnx")


if TORCH_AVAILABLE:
    class _PowerCalculator(nn.Module):
        """Torch module for ONNX export: I^2 + Q^2, mean over antennas/symbols/subcarriers."""

        def forward(self, x: "torch.Tensor") -> "torch.Tensor":
            fp32 = x.float()
            power = torch.square(fp32[..., 0]) + torch.square(fp32[..., 1])
            return power.mean(dim=(0, 1, 3))


    def _export_onnx():
        """Export the PRB power model to ONNX format."""
        os.makedirs(CACHE_DIR, exist_ok=True)
        model = _PowerCalculator().eval()
        dummy = torch.randn(4, 14, 273, 12, 2, dtype=torch.float16)
        if torch.cuda.is_available():
            model = model.cuda()
            dummy = dummy.cuda()
        torch.onnx.export(
            model, dummy, ONNX_PATH,
            export_params=True,
            opset_version=18,
            do_constant_folding=True,
            input_names=["iq_samples"],
            output_names=["prb_power"],
        )
        print(f"prb_power_onnx: exported ONNX model to {ONNX_PATH}")


class InferenceModel:
    """ONNX Runtime PRB power computation (GPU via CUDAExecutionProvider)."""

    def initialize(self):
        if not ORT_AVAILABLE:
            raise RuntimeError("onnxruntime is required for prb_power_onnx model")
        if not TORCH_AVAILABLE:
            raise RuntimeError("PyTorch is required to export the ONNX model")

        if not os.path.exists(ONNX_PATH):
            _export_onnx()

        providers = ["CUDAExecutionProvider", "CPUExecutionProvider"]
        self.session = ort.InferenceSession(ONNX_PATH, providers=providers)
        active = self.session.get_providers()
        print(f"prb_power_onnx initialized (providers: {active})")

    def get_metadata(self):
        return {
            "name": "prb_power_onnx",
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

        iq = inputs["iq_samples"].astype(np.float16)

        compute_start = time.perf_counter()
        results = self.session.run(["prb_power"], {"iq_samples": iq})
        compute_end = time.perf_counter()

        total_us = (compute_end - start) * 1_000_000
        compute_us = (compute_end - compute_start) * 1_000_000

        return {
            "prb_power": results[0],
            "timing_us": np.array([total_us, compute_us], dtype=np.float64)
        }

    def finalize(self):
        self.session = None
