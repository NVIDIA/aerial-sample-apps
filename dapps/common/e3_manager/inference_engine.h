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
 * Inference engine interface and common data types for dApp applications.
 * Implement this interface to support different inference backends (e.g. Triton,
 * TensorRT, ONNX Runtime).
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <map>
#include <cstdint>

namespace e3 {

struct ModelInput {
    std::string name;
    std::string type;
    std::vector<int64_t> shape;
};

struct ModelOutput {
    std::string name;
    std::string type;
    std::vector<int64_t> shape;
};

struct ModelMetadata {
    std::string name;
    std::string platform;
    std::vector<ModelInput> inputs;
    std::vector<ModelOutput> outputs;
};

struct ShmInfo {
    size_t offset;
    size_t size;
};

using TensorData = std::variant<std::vector<uint8_t>, ShmInfo>;

struct TensorInput {
    std::string name;
    std::vector<int64_t> shape;
    std::string datatype;
    TensorData data;
};

struct TensorOutput {
    std::vector<uint8_t> data;
};

class InferenceEngine {
public:
    struct InferenceResult {
        bool success;
        std::map<std::string, TensorOutput> outputs;
        std::string error_message;
    };

    virtual ~InferenceEngine() = default;

    virtual void Initialize(size_t shm_size) = 0;
    virtual void Shutdown() = 0;
    virtual bool RegisterSharedMemory(const std::string& key, size_t size) { return true; }
    virtual std::optional<ModelMetadata> GetModelMetadata(const std::string& model_name) = 0;
    virtual InferenceResult infer(
        const std::string& model_name,
        const std::vector<TensorInput>& inputs,
        const std::vector<std::string>& output_names) = 0;
    virtual bool LoadModel(const std::string& model_name) { return true; }
    virtual bool UnloadModel(const std::string& model_name) { return true; }
};

} // namespace e3
