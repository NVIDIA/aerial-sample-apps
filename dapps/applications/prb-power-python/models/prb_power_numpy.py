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
# PRB Power model using NumPy (CPU).
#
# Computes per-PRB average power from IQ samples:
#   power = mean(I^2 + Q^2) over antennas, symbols, and subcarriers.
#
# Input:  iq_samples  — shape (4, 14, 273, 12, 2), dtype float16
#         [antennas, symbols, PRBs, subcarriers, I/Q]
# Output: prb_power   — shape (273,), dtype float32
#         timing_us   — shape (2,),   dtype float64
#                       [total_us, compute_us]
#

import numpy as np
import time


class InferenceModel:
    """NumPy-based PRB power computation (CPU only)."""

    def initialize(self):
        pass

    def get_metadata(self):
        return {
            "name": "prb_power_numpy",
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

        iq = inputs["iq_samples"].astype(np.float32)

        compute_start = time.perf_counter()
        # Power = I^2 + Q^2 per sample, then average over
        # antennas (axis 0), symbols (axis 1), subcarriers (axis 3)
        power = np.square(iq[..., 0]) + np.square(iq[..., 1])
        prb_power = power.mean(axis=(0, 1, 3))
        compute_end = time.perf_counter()

        total_us = (compute_end - start) * 1_000_000
        compute_us = (compute_end - compute_start) * 1_000_000

        return {
            "prb_power": prb_power,
            "timing_us": np.array([total_us, compute_us], dtype=np.float64)
        }

    def finalize(self):
        pass
