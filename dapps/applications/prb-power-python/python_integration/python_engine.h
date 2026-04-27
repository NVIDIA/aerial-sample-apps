/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Python inference backend for the InferenceEngine interface.
 * Embeds a Python interpreter via pybind11 and delegates inference
 * to a user-provided Python model (InferenceModel class).
 */

#pragma once

#include "inference_engine.h"
#include <pybind11/embed.h>
#include <pybind11/numpy.h>
#include <memory>

namespace py = pybind11;

namespace e3 {

class __attribute__((visibility("hidden"))) PythonEngine : public InferenceEngine {
public:
    // module_path: directory containing the Python model file
    // model_name:  Python module name (filename without .py)
    PythonEngine(const std::string& module_path, const std::string& model_name);
    ~PythonEngine() override;

    void Initialize(size_t shm_size) override;
    void Shutdown() override;
    bool RegisterSharedMemory(const std::string& key, size_t size) override;
    std::optional<ModelMetadata> GetModelMetadata(const std::string& model_name) override;
    InferenceResult infer(
        const std::string& model_name,
        const std::vector<TensorInput>& inputs,
        const std::vector<std::string>& output_names) override;

private:
    // Maps E3 type strings (e.g., "FP16") to numpy dtype strings
    static std::string DtypeToNumpy(const std::string& e3_dtype);

    std::string module_path_;
    std::string model_name_;
    std::unique_ptr<py::scoped_interpreter> interpreter_;
    std::unique_ptr<py::gil_scoped_release> gil_release_;
    py::object py_model_;

    // Shared memory mapping for zero-copy input access
    void* shm_base_ = nullptr;
    size_t shm_size_ = 0;
    int shm_fd_ = -1;
};

} // namespace e3
