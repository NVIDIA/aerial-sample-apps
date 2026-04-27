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
 * Triton Inference Server backend for the InferenceEngine interface.
 * Communicates with Triton via gRPC.
 */

#pragma once

#include "inference_engine.h"
#include <memory>
#include <unordered_map>

// Forward declarations from the Triton C++ client library
namespace triton { namespace client {
    class InferenceServerGrpcClient;
}}

namespace e3 {

class TritonEngine : public InferenceEngine {
public:
    // Connects to a Triton server via gRPC URL (e.g., "localhost:8001")
    explicit TritonEngine(const std::string& server_url = "localhost:8001");
    ~TritonEngine() override;

    void Initialize(size_t shm_size) override;
    void Shutdown() override;
    bool RegisterSharedMemory(const std::string& key, size_t size) override;
    std::optional<ModelMetadata> GetModelMetadata(const std::string& model_name) override;
    InferenceResult infer(
        const std::string& model_name,
        const std::vector<TensorInput>& inputs,
        const std::vector<std::string>& output_names) override;
    bool UnloadModel(const std::string& model_name) override;

private:
    std::unique_ptr<triton::client::InferenceServerGrpcClient> client_;
    std::string server_url_;
    bool shm_registered_ = false;
    std::unordered_map<std::string, ModelMetadata> metadata_cache_;
};

} // namespace e3