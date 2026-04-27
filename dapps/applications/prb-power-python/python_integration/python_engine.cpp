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

#include "python_integration/python_engine.h"
#include <pybind11/stl.h>
#include <iostream>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

namespace e3 {

PythonEngine::PythonEngine(const std::string& module_path, const std::string& model_name)
    : module_path_(module_path), model_name_(model_name) {
}

PythonEngine::~PythonEngine() {
    Shutdown();
}

void PythonEngine::Initialize(size_t shm_size) {
    interpreter_ = std::make_unique<py::scoped_interpreter>();

    try {
        py::module_::import("sys").attr("path").attr("append")(module_path_);
        auto module = py::module_::import(model_name_.c_str());
        py_model_ = module.attr("InferenceModel")();
        py_model_.attr("initialize")();
    } catch (py::error_already_set& e) {
        std::cerr << "Failed to load Python model '" << model_name_
                  << "': " << e.what() << std::endl;
        throw std::runtime_error("PythonEngine initialization failed");
    }

    std::cout << "PythonEngine initialized (model: " << model_name_
              << ", path: " << module_path_ << ")" << std::endl;

    // Release the GIL so worker threads (E3 Manager) can acquire it
    gil_release_ = std::make_unique<py::gil_scoped_release>();
}

void PythonEngine::Shutdown() {
    if (interpreter_) {
        // Re-acquire the GIL before touching Python objects
        gil_release_.reset();

        try {
            if (!py_model_.is_none()) {
                py_model_.attr("finalize")();
            }
        } catch (py::error_already_set& e) {
            std::cerr << "Python finalize error: " << e.what() << std::endl;
        }
        py_model_ = py::none();
        interpreter_.reset();
    }

    // Unmap shared memory
    if (shm_base_ && shm_base_ != MAP_FAILED) {
        munmap(shm_base_, shm_size_);
        shm_base_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
}

bool PythonEngine::RegisterSharedMemory(const std::string& key, size_t size) {
    if (shm_base_) return true;  // already mapped

    shm_fd_ = shm_open(key.c_str(), O_RDONLY, 0);
    if (shm_fd_ < 0) {
        std::cerr << "PythonEngine: failed to open shared memory '" << key << "'" << std::endl;
        return false;
    }

    shm_base_ = mmap(nullptr, size, PROT_READ, MAP_SHARED, shm_fd_, 0);
    if (shm_base_ == MAP_FAILED) {
        std::cerr << "PythonEngine: failed to mmap shared memory (" << size << " bytes)" << std::endl;
        close(shm_fd_);
        shm_fd_ = -1;
        shm_base_ = nullptr;
        return false;
    }

    shm_size_ = size;
    std::cout << "PythonEngine: shared memory mapped (" << (size / 1024.0 / 1024.0) << " MB)" << std::endl;
    return true;
}

std::optional<ModelMetadata> PythonEngine::GetModelMetadata(const std::string& model_name) {
    if (py_model_.is_none()) return std::nullopt;

    if (!model_name.empty() && model_name != model_name_) {
        std::cerr << "GetModelMetadata: requested '" << model_name
                  << "' but only '" << model_name_ << "' is loaded" << std::endl;
        return std::nullopt;
    }

    try {
        py::gil_scoped_acquire acquire;
        py::dict meta = py_model_.attr("get_metadata")();

        ModelMetadata md;
        md.name = meta["name"].cast<std::string>();
        md.platform = "python";

        for (auto item : meta["inputs"].cast<py::list>()) {
            auto d = item.cast<py::dict>();
            md.inputs.push_back({
                d["name"].cast<std::string>(),
                d["type"].cast<std::string>(),
                d["shape"].cast<std::vector<int64_t>>()
            });
        }

        for (auto item : meta["outputs"].cast<py::list>()) {
            auto d = item.cast<py::dict>();
            md.outputs.push_back({
                d["name"].cast<std::string>(),
                d["type"].cast<std::string>(),
                d["shape"].cast<std::vector<int64_t>>()
            });
        }

        return md;
    } catch (py::error_already_set& e) {
        std::cerr << "GetModelMetadata failed: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::string PythonEngine::DtypeToNumpy(const std::string& e3_dtype) {
    if (e3_dtype == "FP16")   return "float16";
    if (e3_dtype == "FP32")   return "float32";
    if (e3_dtype == "FP64")   return "float64";
    if (e3_dtype == "INT16")  return "int16";
    if (e3_dtype == "INT32")  return "int32";
    if (e3_dtype == "UINT8")  return "uint8";
    if (e3_dtype == "UINT16") return "uint16";
    if (e3_dtype == "UINT32") return "uint32";
    return "float32";
}

InferenceEngine::InferenceResult PythonEngine::infer(
    const std::string& model_name,
    const std::vector<TensorInput>& inputs,
    const std::vector<std::string>& output_names) {

    InferenceResult result;
    result.success = false;

    if (py_model_.is_none()) {
        result.error_message = "Python engine not initialized";
        return result;
    }

    if (!model_name.empty() && model_name != model_name_) {
        result.error_message = "Requested model '" + model_name +
            "' but only '" + model_name_ + "' is loaded";
        return result;
    }

    try {
        py::gil_scoped_acquire acquire;
        py::dict py_inputs;

        for (const auto& input : inputs) {
            void* data_ptr = nullptr;
            size_t data_size = 0;

            if (auto* shm = std::get_if<ShmInfo>(&input.data)) {
                if (!shm_base_) {
                    result.error_message = "Shared memory not registered";
                    return result;
                }
                data_ptr = static_cast<uint8_t*>(shm_base_) + shm->offset;
                data_size = shm->size;
            } else if (auto* vec = std::get_if<std::vector<uint8_t>>(&input.data)) {
                data_ptr = const_cast<uint8_t*>(vec->data());
                data_size = vec->size();
            }

            // Zero-copy: numpy array views the existing memory (read-only)
            auto dtype = py::dtype(DtypeToNumpy(input.datatype));
            auto arr = py::array(dtype, input.shape, {}, data_ptr);

            py_inputs[input.name.c_str()] = arr;
        }

        // Call model.infer(inputs) -> dict of numpy arrays
        py::dict py_outputs = py_model_.attr("infer")(py_inputs);

        // Extract output arrays into TensorOutput byte vectors
        for (const auto& name : output_names) {
            if (!py_outputs.contains(name)) continue;

            auto arr = py::cast<py::array>(py_outputs[name.c_str()]);
            auto buf = arr.request();
            auto* ptr = static_cast<uint8_t*>(buf.ptr);
            size_t nbytes = static_cast<size_t>(buf.itemsize * buf.size);

            TensorOutput out;
            out.data.assign(ptr, ptr + nbytes);
            result.outputs[name] = std::move(out);
        }

        result.success = true;
    } catch (py::error_already_set& e) {
        result.error_message = std::string("Python inference error: ") + e.what();
        std::cerr << result.error_message << std::endl;
    }

    return result;
}

} // namespace e3
