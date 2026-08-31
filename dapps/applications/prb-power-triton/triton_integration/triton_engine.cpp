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

 /*
 * Triton C API (in-process) implementation of InferenceEngine.
 */

#include "triton_integration/triton_engine.h"
#include <nlohmann/json.hpp>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <future>
#include <cstring>

using json = nlohmann::json;

namespace {

#define RETURN_IF_ERR(expr)                                              \
    do {                                                                 \
        TRITONSERVER_Error* _err = (expr);                               \
        if (_err != nullptr) {                                           \
            std::cerr << "Triton error: "                                \
                      << TRITONSERVER_ErrorMessage(_err) << std::endl;    \
            TRITONSERVER_ErrorDelete(_err);                              \
            return false;                                                \
        }                                                                \
    } while (false)

TRITONSERVER_Error* ResponseAlloc(
    TRITONSERVER_ResponseAllocator* allocator, const char* tensor_name,
    size_t byte_size, TRITONSERVER_MemoryType preferred_memory_type,
    int64_t preferred_memory_type_id, void* userp,
    void** buffer, void** buffer_userp,
    TRITONSERVER_MemoryType* actual_memory_type,
    int64_t* actual_memory_type_id) {
    if (byte_size == 0) {
        *buffer = nullptr;
    } else {
        *buffer = malloc(byte_size);
        if (*buffer == nullptr) {
            return TRITONSERVER_ErrorNew(
                TRITONSERVER_ERROR_INTERNAL,
                "Failed to allocate output buffer");
        }
    }
    *buffer_userp = nullptr;
    *actual_memory_type = TRITONSERVER_MEMORY_CPU;
    *actual_memory_type_id = 0;
    return nullptr;
}

TRITONSERVER_Error* ResponseRelease(
    TRITONSERVER_ResponseAllocator* allocator, void* buffer, void* buffer_userp,
    size_t byte_size, TRITONSERVER_MemoryType memory_type,
    int64_t memory_type_id) {
    free(buffer);
    return nullptr;
}

struct InferContext {
    std::promise<TRITONSERVER_InferenceResponse*> promise;
};

void InferResponseComplete(
    TRITONSERVER_InferenceResponse* response, const uint32_t flags, void* userp) {
    if (userp != nullptr) {
        auto* ctx = static_cast<InferContext*>(userp);
        ctx->promise.set_value(response);
    }
}

void InferRequestRelease(
    TRITONSERVER_InferenceRequest* request, const uint32_t flags, void* userp) {
    TRITONSERVER_InferenceRequestDelete(request);
}

} // anonymous namespace

namespace e3 {

TritonEngine::TritonEngine(const std::string& model_repo_path)
    : model_repo_path_(model_repo_path) {
}

TritonEngine::~TritonEngine() {
    Shutdown();
}

void TritonEngine::Initialize(size_t /*shm_size*/) {
    TRITONSERVER_ServerOptions* opts = nullptr;
    auto* err = TRITONSERVER_ServerOptionsNew(&opts);
    if (err != nullptr) {
        std::cerr << "Failed to create server options: "
                  << TRITONSERVER_ErrorMessage(err) << std::endl;
        TRITONSERVER_ErrorDelete(err);
        return;
    }

    TRITONSERVER_ServerOptionsSetModelRepositoryPath(opts, model_repo_path_.c_str());
    TRITONSERVER_ServerOptionsSetModelControlMode(opts, TRITONSERVER_MODEL_CONTROL_NONE);
    TRITONSERVER_ServerOptionsSetStrictModelConfig(opts, false);
    TRITONSERVER_ServerOptionsSetLogVerbose(opts, 0);

    err = TRITONSERVER_ServerNew(&server_, opts);
    TRITONSERVER_ServerOptionsDelete(opts);

    if (err != nullptr) {
        std::cerr << "Failed to create Triton server: "
                  << TRITONSERVER_ErrorMessage(err) << std::endl;
        TRITONSERVER_ErrorDelete(err);
        server_ = nullptr;
        return;
    }

    // Wait for server to be ready
    bool ready = false;
    for (int i = 0; i < 300 && !ready; ++i) {
        err = TRITONSERVER_ServerIsReady(server_, &ready);
        if (err != nullptr) {
            TRITONSERVER_ErrorDelete(err);
            break;
        }
        if (!ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    if (!ready) {
        std::cerr << "Triton server failed to become ready" << std::endl;
        TRITONSERVER_ServerDelete(server_);
        server_ = nullptr;
        return;
    }

    err = TRITONSERVER_ResponseAllocatorNew(
        &allocator_, ResponseAlloc, ResponseRelease, nullptr);
    if (err != nullptr) {
        std::cerr << "Failed to create response allocator: "
                  << TRITONSERVER_ErrorMessage(err) << std::endl;
        TRITONSERVER_ErrorDelete(err);
        TRITONSERVER_ServerDelete(server_);
        server_ = nullptr;
        return;
    }

    std::cout << "Triton C API server initialized (repo: "
              << model_repo_path_ << ")" << std::endl;
}

void TritonEngine::Shutdown() {
    if (shm_base_ptr_ != nullptr && shm_base_ptr_ != MAP_FAILED) {
        munmap(shm_base_ptr_, shm_mapped_size_);
        shm_base_ptr_ = nullptr;
        shm_mapped_size_ = 0;
    }

    if (allocator_ != nullptr) {
        TRITONSERVER_ResponseAllocatorDelete(allocator_);
        allocator_ = nullptr;
    }

    if (server_ != nullptr) {
        TRITONSERVER_ServerDelete(server_);
        server_ = nullptr;
        std::cout << "Triton C API server shut down" << std::endl;
    }
}

bool TritonEngine::RegisterSharedMemory(const std::string& key, size_t size) {
    if (shm_base_ptr_ != nullptr || size == 0) {
        return false;
    }

    int fd = shm_open(key.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        std::cerr << "Failed to open shared memory '" << key
                  << "': " << strerror(errno) << std::endl;
        return false;
    }

    shm_base_ptr_ = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (shm_base_ptr_ == MAP_FAILED) {
        std::cerr << "Failed to mmap shared memory: "
                  << strerror(errno) << std::endl;
        shm_base_ptr_ = nullptr;
        return false;
    }

    shm_mapped_size_ = size;
    std::cout << "Shared memory mapped in-process" << std::endl;
    std::cout << "  Size: " << size << " bytes ("
              << (size / 1024.0 / 1024.0) << " MB)" << std::endl;
    return true;
}

bool TritonEngine::UnloadModel(const std::string& model_name) {
    if (server_ == nullptr) {
        std::cerr << "Cannot unload model, server not initialized." << std::endl;
        return false;
    }

    RETURN_IF_ERR(TRITONSERVER_ServerUnloadModel(server_, model_name.c_str()));
    std::cout << "Requested unload for model: " << model_name << std::endl;
    return true;
}

std::optional<e3::ModelMetadata> TritonEngine::GetModelMetadata(
    const std::string& model_name) {
    auto it = metadata_cache_.find(model_name);
    if (it != metadata_cache_.end()) return it->second;

    if (server_ == nullptr) {
        std::cerr << "Cannot get model metadata, server not initialized." << std::endl;
        return std::nullopt;
    }

    TRITONSERVER_Message* metadata_msg = nullptr;
    auto* err = TRITONSERVER_ServerModelMetadata(
        server_, model_name.c_str(), -1, &metadata_msg);
    if (err != nullptr) {
        std::cerr << "Failed to get metadata for model '"
                  << model_name << "': "
                  << TRITONSERVER_ErrorMessage(err) << std::endl;
        TRITONSERVER_ErrorDelete(err);
        return std::nullopt;
    }

    const char* buf = nullptr;
    size_t buf_size = 0;
    err = TRITONSERVER_MessageSerializeToJson(metadata_msg, &buf, &buf_size);
    if (err != nullptr) {
        std::cerr << "Failed to serialize metadata: "
                  << TRITONSERVER_ErrorMessage(err) << std::endl;
        TRITONSERVER_ErrorDelete(err);
        TRITONSERVER_MessageDelete(metadata_msg);
        return std::nullopt;
    }

    json j = json::parse(buf, buf + buf_size, nullptr, false);
    TRITONSERVER_MessageDelete(metadata_msg);

    if (j.is_discarded()) {
        std::cerr << "Failed to parse metadata JSON" << std::endl;
        return std::nullopt;
    }

    e3::ModelMetadata metadata;
    metadata.name = j.value("name", "");
    metadata.platform = j.value("platform", "");

    if (j.contains("inputs")) {
        for (const auto& inp : j["inputs"]) {
            ModelInput mi;
            mi.name = inp.value("name", "");
            mi.type = inp.value("datatype", "");
            if (inp.contains("shape")) {
                for (const auto& d : inp["shape"]) {
                    mi.shape.push_back(d.get<int64_t>());
                }
            }
            metadata.inputs.push_back(mi);
        }
    }

    if (j.contains("outputs")) {
        for (const auto& out : j["outputs"]) {
            ModelOutput mo;
            mo.name = out.value("name", "");
            mo.type = out.value("datatype", "");
            if (out.contains("shape")) {
                for (const auto& d : out["shape"]) {
                    mo.shape.push_back(d.get<int64_t>());
                }
            }
            metadata.outputs.push_back(mo);
        }
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

    if (server_ == nullptr) {
        result.error_message = "Server not initialized.";
        return result;
    }

    // 1. Create inference request for the target model
    TRITONSERVER_InferenceRequest* request = nullptr;
    auto* err = TRITONSERVER_InferenceRequestNew(
        &request, server_, model_name.c_str(), -1);
    if (err != nullptr) {
        result.error_message = std::string("Failed to create request: ") +
                               TRITONSERVER_ErrorMessage(err);
        TRITONSERVER_ErrorDelete(err);
        return result;
    }

    // 2. Attach input tensors (shared memory pointer or raw bytes)
    for (const auto& inp : inputs) {
        auto triton_dtype = TRITONSERVER_StringToDataType(inp.datatype.c_str());
        err = TRITONSERVER_InferenceRequestAddInput(
            request, inp.name.c_str(), triton_dtype,
            inp.shape.data(), static_cast<uint32_t>(inp.shape.size()));
        if (err != nullptr) {
            result.error_message = "Failed to add input '" + inp.name + "': " +
                                   TRITONSERVER_ErrorMessage(err);
            TRITONSERVER_ErrorDelete(err);
            TRITONSERVER_InferenceRequestDelete(request);
            return result;
        }

        bool input_err = false;
        std::visit([&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, ShmInfo>) {
                if (shm_base_ptr_ == nullptr) {
                    result.error_message = "ShmInfo input but no shared memory mapped";
                    input_err = true;
                    return;
                }
                auto* e = TRITONSERVER_InferenceRequestAppendInputData(
                    request, inp.name.c_str(),
                    static_cast<const uint8_t*>(shm_base_ptr_) + arg.offset,
                    arg.size, TRITONSERVER_MEMORY_CPU, 0);
                if (e != nullptr) {
                    result.error_message = "Failed to set shm data for '" +
                                           inp.name + "': " +
                                           TRITONSERVER_ErrorMessage(e);
                    TRITONSERVER_ErrorDelete(e);
                    input_err = true;
                }
            } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
                auto* e = TRITONSERVER_InferenceRequestAppendInputData(
                    request, inp.name.c_str(),
                    arg.data(), arg.size(),
                    TRITONSERVER_MEMORY_CPU, 0);
                if (e != nullptr) {
                    result.error_message = "Failed to set raw data for '" +
                                           inp.name + "': " +
                                           TRITONSERVER_ErrorMessage(e);
                    TRITONSERVER_ErrorDelete(e);
                    input_err = true;
                }
            }
        }, inp.data);

        if (input_err) {
            TRITONSERVER_InferenceRequestDelete(request);
            return result;
        }
    }

    // 3. Register requested output tensors
    for (const auto& out_name : output_names) {
        err = TRITONSERVER_InferenceRequestAddRequestedOutput(
            request, out_name.c_str());
        if (err != nullptr) {
            result.error_message = "Failed to request output '" + out_name +
                                   "': " + TRITONSERVER_ErrorMessage(err);
            TRITONSERVER_ErrorDelete(err);
            TRITONSERVER_InferenceRequestDelete(request);
            return result;
        }
    }

    // 4. Set up async completion (promise/future to block until response)
    InferContext infer_ctx;
    auto future = infer_ctx.promise.get_future();

    err = TRITONSERVER_InferenceRequestSetReleaseCallback(
        request, InferRequestRelease, nullptr);
    if (err != nullptr) {
        result.error_message = std::string("Failed to set release callback: ") +
                               TRITONSERVER_ErrorMessage(err);
        TRITONSERVER_ErrorDelete(err);
        TRITONSERVER_InferenceRequestDelete(request);
        return result;
    }

    err = TRITONSERVER_InferenceRequestSetResponseCallback(
        request, allocator_, nullptr,
        InferResponseComplete, &infer_ctx);
    if (err != nullptr) {
        result.error_message = std::string("Failed to set response callback: ") +
                               TRITONSERVER_ErrorMessage(err);
        TRITONSERVER_ErrorDelete(err);
        TRITONSERVER_InferenceRequestDelete(request);
        return result;
    }

    // 5. Fire inference -- direct in-process call, no gRPC/network overhead
    err = TRITONSERVER_ServerInferAsync(server_, request, nullptr);
    if (err != nullptr) {
        result.error_message = std::string("Inference failed: ") +
                               TRITONSERVER_ErrorMessage(err);
        TRITONSERVER_ErrorDelete(err);
        TRITONSERVER_InferenceRequestDelete(request);
        return result;
    }

    // 6. Wait for response (blocks until InferResponseComplete fires)
    TRITONSERVER_InferenceResponse* response = future.get();
    if (response == nullptr) {
        result.error_message = "Null inference response";
        return result;
    }

    err = TRITONSERVER_InferenceResponseError(response);
    if (err != nullptr) {
        result.error_message = std::string("Inference error: ") +
                               TRITONSERVER_ErrorMessage(err);
        TRITONSERVER_ErrorDelete(err);
        TRITONSERVER_InferenceResponseDelete(response);
        return result;
    }

    // 7. Extract output tensors from response
    uint32_t output_count = 0;
    TRITONSERVER_InferenceResponseOutputCount(response, &output_count);

    for (uint32_t i = 0; i < output_count; ++i) {
        const char* name = nullptr;
        TRITONSERVER_DataType datatype;
        const int64_t* shape = nullptr;
        uint64_t dim_count = 0;
        const void* base = nullptr;
        size_t byte_size = 0;
        TRITONSERVER_MemoryType mem_type;
        int64_t mem_id = 0;
        void* userp = nullptr;

        err = TRITONSERVER_InferenceResponseOutput(
            response, i, &name, &datatype, &shape, &dim_count,
            &base, &byte_size, &mem_type, &mem_id, &userp);
        if (err != nullptr) {
            TRITONSERVER_ErrorDelete(err);
            continue;
        }

        TensorOutput tensor_output;
        if (base != nullptr && byte_size > 0) {
            auto* raw = static_cast<const uint8_t*>(base);
            tensor_output.data.assign(raw, raw + byte_size);
        }
        result.outputs[name] = std::move(tensor_output);
    }

    TRITONSERVER_InferenceResponseDelete(response);
    result.success = true;
    return result;
}

} // namespace e3
