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
# PRB Power model using TensorRT (GPU, optimized).
#
# On first initialize(), builds a TensorRT engine from the ONNX model
# (exporting it first if needed). The engine is cached in /tmp so
# subsequent starts within the same container are instant.
#
# Input:  iq_samples  - shape (4, 14, 273, 12, 2), dtype float16
#         [antennas, symbols, PRBs, subcarriers, I/Q]
# Output: prb_power   - shape (273,), dtype float32
#         timing_us   - shape (2,),   dtype float64
#                       [total_us, compute_us]
#

import numpy as np
import time
import os

try:
    import tensorrt as trt
    TRT_AVAILABLE = True
except ImportError:
    TRT_AVAILABLE = False

from cuda.bindings import driver as cuda_drv

CACHE_DIR = "/tmp/prb_power_models"
ONNX_PATH = os.path.join(CACHE_DIR, "prb_power.onnx")
ENGINE_PATH = os.path.join(CACHE_DIR, "prb_power.engine")

INPUT_SHAPE = (4, 14, 273, 12, 2)
OUTPUT_SHAPE = (273,)


def _ensure_onnx():
    """Make sure the ONNX model exists (shared with prb_power_onnx)."""
    if os.path.exists(ONNX_PATH):
        return
    from prb_power_onnx import _export_onnx
    _export_onnx()


def _build_engine():
    """Build a TensorRT engine from the ONNX model."""
    _ensure_onnx()
    os.makedirs(CACHE_DIR, exist_ok=True)

    logger = trt.Logger(trt.Logger.WARNING)
    builder = trt.Builder(logger)
    config = builder.create_builder_config()
    config.set_memory_pool_limit(trt.MemoryPoolType.WORKSPACE, 2 << 30)
    config.set_flag(trt.BuilderFlag.FP16)

    network = builder.create_network(
        1 << int(trt.NetworkDefinitionCreationFlag.EXPLICIT_BATCH)
    )

    parser = trt.OnnxParser(network, logger)
    with open(ONNX_PATH, "rb") as f:
        if not parser.parse(f.read()):
            errors = [str(parser.get_error(i)) for i in range(parser.num_errors)]
            raise RuntimeError(f"ONNX parse failed: {errors}")

    profile = builder.create_optimization_profile()
    profile.set_shape("iq_samples", INPUT_SHAPE, INPUT_SHAPE, INPUT_SHAPE)
    config.add_optimization_profile(profile)

    print("prb_power_trt: building TensorRT engine (this may take a moment)...")
    serialized = builder.build_serialized_network(network, config)
    if serialized is None:
        raise RuntimeError("TensorRT engine build failed")

    with open(ENGINE_PATH, "wb") as f:
        f.write(serialized)
    print(f"prb_power_trt: engine saved to {ENGINE_PATH}")


class InferenceModel:
    """TensorRT PRB power computation (GPU, FP16-optimized)."""

    def initialize(self):
        if not TRT_AVAILABLE:
            raise RuntimeError("tensorrt is required for prb_power_trt model")

        if not os.path.exists(ENGINE_PATH):
            _build_engine()

        self.logger = trt.Logger(trt.Logger.WARNING)
        self.runtime = trt.Runtime(self.logger)
        with open(ENGINE_PATH, "rb") as f:
            self.engine = self.runtime.deserialize_cuda_engine(f.read())
        self.context = self.engine.create_execution_context()

        err, self.stream = cuda_drv.cuStreamCreate(0)
        input_nbytes = int(np.prod(INPUT_SHAPE)) * np.dtype(np.float16).itemsize
        output_nbytes = int(np.prod(OUTPUT_SHAPE)) * np.dtype(np.float32).itemsize
        err, self.d_input = cuda_drv.cuMemAlloc(input_nbytes)
        err, self.d_output = cuda_drv.cuMemAlloc(output_nbytes)
        self.input_nbytes = input_nbytes
        self.output_nbytes = output_nbytes

        print(f"prb_power_trt initialized (engine: {ENGINE_PATH})")

    def get_metadata(self):
        return {
            "name": "prb_power_trt",
            "inputs": [
                {"name": "iq_samples", "type": "FP16", "shape": list(INPUT_SHAPE)}
            ],
            "outputs": [
                {"name": "prb_power", "type": "FP32", "shape": list(OUTPUT_SHAPE)},
                {"name": "timing_us", "type": "FP64", "shape": [2]}
            ]
        }

    def infer(self, inputs):
        start = time.perf_counter()
        iq = np.ascontiguousarray(inputs["iq_samples"].astype(np.float16))
        output = np.empty(OUTPUT_SHAPE, dtype=np.float32)

        compute_start = time.perf_counter()

        cuda_drv.cuMemcpyHtoDAsync(
            self.d_input, iq.ctypes.data, self.input_nbytes, self.stream)
        self.context.set_tensor_address("iq_samples", int(self.d_input))
        self.context.set_tensor_address("prb_power", int(self.d_output))
        self.context.execute_async_v3(stream_handle=int(self.stream))
        cuda_drv.cuMemcpyDtoHAsync(
            output.ctypes.data, self.d_output, self.output_nbytes, self.stream)
        cuda_drv.cuStreamSynchronize(self.stream)

        compute_end = time.perf_counter()

        total_us = (compute_end - start) * 1_000_000
        compute_us = (compute_end - compute_start) * 1_000_000

        return {
            "prb_power": output,
            "timing_us": np.array([total_us, compute_us], dtype=np.float64)
        }

    def finalize(self):
        cuda_drv.cuMemFree(self.d_input)
        cuda_drv.cuMemFree(self.d_output)
        cuda_drv.cuStreamDestroy(self.stream)
        self.context = None
        self.engine = None
        self.runtime = None
