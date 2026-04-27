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
 */

#include "triton_integration/triton_engine.h"
#include <grpc_client.h>
#include <iostream>

namespace tc = triton::client;

namespace e3 {

TritonEngine::TritonEngine(const std::string& server_url)
    : server_url_(server_url) {
}

void TritonEngine::Initialize(size_t shm_size) {
    auto err = tc::InferenceServerGrpcClient::Create(&client_, server_url_, false);
    if (!err.IsOk()) {
        std::cerr << "Failed to create Triton gRPC client: " << err.Message() << std::endl;
        client_ = nullptr;
        return;
    }
    std::cout << "Triton gRPC client created (server: " << server_url_ << ")" << std::endl;
}

void TritonEngine::Shutdown() {
    if (client_) {
        if (shm_registered_) {
            auto err = client_->UnregisterSystemSharedMemory("cpu_e3_ran_buffers");
            if (!err.IsOk()) {
                std::cerr << "Failed to unregister shared memory: " << err.Message() << std::endl;
            }
            shm_registered_ = false;
        }
        client_.reset();
        std::cout << "Triton gRPC client shut down" << std::endl;
    }
}

bool TritonEngine::RegisterSharedMemory(const std::string& key, size_t size) {
    if (shm_registered_ || !client_ || size == 0) {
        return false;
    }
    
    auto err = client_->RegisterSystemSharedMemory("cpu_e3_ran_buffers", key.c_str(), size, 0);
    if (!err.IsOk()) {
        std::cerr << "Failed to register shared memory: " << err.Message() << std::endl;
        return false;
    }
    
    shm_registered_ = true;
    std::cout << "Shared memory registered with Triton" << std::endl;
    std::cout << "  Size: " << size << " bytes (" << (size / 1024.0 / 1024.0) << " MB)" << std::endl;
    return true;
}

TritonEngine::~TritonEngine() {
    Shutdown();
}

std::optional<e3::ModelMetadata> TritonEngine::GetModelMetadata(const std::string& model_name) {
    auto it = metadata_cache_.find(model_name);
    if (it != metadata_cache_.end()) return it->second;

    if (!client_) {
        std::cerr << "Cannot get model metadata, client not initialized." << std::endl;
        return std::nullopt;
    }

    // The model version can be an empty string, in which case the
    // latest version of the model is returned.
    std::string model_version;
    inference::ModelMetadataResponse model_metadata_response;
    
    auto err = client_->ModelMetadata(&model_metadata_response, model_name, model_version);
    if (!err.IsOk()) {
        std::cerr << "Failed to get metadata for model '" << model_name << "': " << err.Message() << std::endl;
        return std::nullopt;
    }
    
    e3::ModelMetadata metadata;
    metadata.name = model_metadata_response.name();
    metadata.platform = model_metadata_response.platform();
    
    for (const auto& input : model_metadata_response.inputs()) {
        ModelInput model_input;
        model_input.name = input.name();
        model_input.type = input.datatype();
        for (const auto& dim : input.shape()) {
            model_input.shape.push_back(dim);
        }
        metadata.inputs.push_back(model_input);
    }
    
    for (const auto& output : model_metadata_response.outputs()) {
        ModelOutput model_output;
        model_output.name = output.name();
        model_output.type = output.datatype();
        for (const auto& dim : output.shape()) {
            model_output.shape.push_back(dim);
        }
        metadata.outputs.push_back(model_output);
    }
    
    metadata_cache_[model_name] = metadata;
    return metadata;
}

InferenceEngine::InferenceResult TritonEngine::infer(
    const std::string& model_name,
    const std::vector<TensorInput>& inputs,
    const std::vector<std::string>& output_names) {
    
    InferenceResult result;
    result.success = false;

    if (!client_) {
        result.error_message = "Client not initialized or failed to create.";
        return result;
    }

    // 1. Create Input Tensors
    std::vector<tc::InferInput*> input_tensors;
    std::vector<std::unique_ptr<tc::InferInput>> input_ptr_owner; // To manage lifetime

    for (const auto& input_data : inputs) {
        tc::InferInput* input;
        auto err = tc::InferInput::Create(&input, input_data.name, input_data.shape, input_data.datatype);
        if (!err.IsOk()) {
            result.error_message = "Failed to create input '" + input_data.name + "': " + err.Message();
            return result;
        }
        input_ptr_owner.emplace_back(input);

        // Set data for the input tensor
        std::visit([&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                // Data is provided as a raw byte vector
                err = input->AppendRaw(arg);
            } else if constexpr (std::is_same_v<T, ShmInfo>) {
                // Data is in shared memory
                err = input->SetSharedMemory("cpu_e3_ran_buffers", arg.size, arg.offset);
            }
        }, input_data.data);

        if (!err.IsOk()) {
            result.error_message = "Failed to set data for input '" + input_data.name + "': " + err.Message();
            return result;
        }
        input_tensors.push_back(input);
    }

    // 2. Create Output Tensors
    std::vector<const tc::InferRequestedOutput*> output_tensors;
    std::vector<std::unique_ptr<tc::InferRequestedOutput>> output_ptr_owner;
    for (const auto& output_name : output_names) {
        tc::InferRequestedOutput* output;
        auto err = tc::InferRequestedOutput::Create(&output, output_name);
        if (!err.IsOk()) {
            result.error_message = "Failed to create output request for '" + output_name + "': " + err.Message();
            return result;
        }
        output_ptr_owner.emplace_back(output);
        output_tensors.push_back(output);
    }
    
    // 3. Perform Inference
    tc::InferOptions options(model_name);
    tc::InferResult* infer_result = nullptr;
    auto err = client_->Infer(&infer_result, options, input_tensors, output_tensors);
    std::unique_ptr<tc::InferResult> infer_result_ptr(infer_result);

    if (!err.IsOk()) {
        result.error_message = "Inference failed: " + err.Message();
        return result;
    }

    // 4. Process Response
    for (const auto& output_name : output_names) {
        const uint8_t* output_raw;
        size_t output_byte_size;
        err = infer_result_ptr->RawData(output_name, &output_raw, &output_byte_size);
        if (!err.IsOk()) {
            result.error_message = "Failed to get raw data for output '" + output_name + "': " + err.Message();
            // In a multi-output scenario, we might want to continue and report partial success,
            // but for now, we fail the whole request.
            return result;
        }
        
        TensorOutput tensor_output;
        tensor_output.data.assign(output_raw, output_raw + output_byte_size);
        result.outputs[output_name] = tensor_output;
    }

    result.success = true;
    return result;
}

bool TritonEngine::UnloadModel(const std::string& model_name) {
    if (!client_) {
        std::cerr << "Cannot unload model, client not initialized." << std::endl;
        return false;
    }
    
    // Unload model - let Triton handle it asynchronously
    auto err = client_->UnloadModel(model_name);
    if (!err.IsOk()) {
        std::cerr << "Failed to unload model '" << model_name << "': " << err.Message() << std::endl;
        return false;
    }
    
    std::cout << "Requested unload for model: " << model_name << " (may take time if GPU is busy)" << std::endl;
    return true;
}

} // namespace e3