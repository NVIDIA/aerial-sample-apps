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
 * PRB Power dApp — Triton-based inference on IQ samples.
 * Entry point and application handler for the PRB Power application.
 */

#include "e3_manager.h"
#include "triton_integration/triton_engine.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <complex>
#include <cstring>
#include <signal.h>
#include <atomic>

using json = nlohmann::json;

static void ProcessPRBPower(const e3::IndicationContext& ctx) {
    const auto& model_name = ctx.model_name;
    const auto& indication_payload = ctx.payload;
    auto* engine = ctx.engine;
    auto* ran_shm_ptr = ctx.ran_shm_ptr;
    auto* results_publisher = ctx.results_publisher;

    auto call_start_time = std::chrono::high_resolution_clock::now();
    
    try {
        // 1. Get model metadata
        auto metadata = engine->GetModelMetadata(model_name);

        if (!metadata) {
            std::cerr << "Error: Could not get metadata for model '" << model_name << "'. Skipping inference." << std::endl;
            return;
        }

        // 2. Validate that all required model inputs are available
        std::vector<std::string> missing_inputs;
        for (const auto& model_input : metadata->inputs) {
            if (!indication_payload.contains(model_input.name)) {
                missing_inputs.push_back(model_input.name);
            }
        }
        
        if (!missing_inputs.empty()) {
            std::cerr << "Error: Model '" << model_name << "' requires ALL inputs to be present." << std::endl;
            std::cerr << "  Required inputs: ";
            for (size_t i = 0; i < metadata->inputs.size(); ++i) {
                if (i > 0) std::cerr << ", ";
                std::cerr << metadata->inputs[i].name;
            }
            std::cerr << "\n  Missing inputs: ";
            for (size_t i = 0; i < missing_inputs.size(); ++i) {
                if (i > 0) std::cerr << ", ";
                std::cerr << missing_inputs[i];
            }
            std::cerr << "\nSkipping inference due to missing inputs." << std::endl;
            return;
        }
        
        // 3. Prepare inputs for Triton inference
        std::vector<e3::TensorInput> triton_inputs;
        for (const auto& model_input : metadata->inputs) {
            const auto& payload_data = indication_payload[model_input.name];
            e3::TensorInput triton_input;
            triton_input.name = model_input.name;
            triton_input.datatype = model_input.type;
            
            // Handle dynamic batch dimensions
            std::vector<int64_t> actual_shape = model_input.shape;
            if (!actual_shape.empty() && actual_shape[0] == -1) {
                actual_shape[0] = 1;  // Set batch size to 1
            }
            triton_input.shape = actual_shape;
            
            // Check if this is a shared memory block
            if (payload_data.is_object()) {
                if (payload_data.contains("fh_buffer_index")) {
                    // IQ samples from shared memory
                    SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr);
                    uint8_t fh_buf_idx = payload_data.value("fh_buffer_index", 0u);
                    uint32_t fh_write_idx = payload_data.value("fh_write_index", 0u);
                    
                    size_t base_offset = sizeof(SharedMemoryHeader);
                    size_t single_row_size_bytes = header->num_fh_samples * sizeof(int16_t);
                    size_t buffer_base_offset = base_offset + (fh_buf_idx * header->fh_buffer_size);
                    size_t row_offset = fh_write_idx * single_row_size_bytes;
                    size_t total_offset = buffer_base_offset + row_offset;
                    
                    // ZERO-COPY: Direct shared memory reference
                    triton_input.data = e3::ShmInfo{total_offset, header->num_fh_samples * sizeof(int16_t)};
                    
                } else if (payload_data.contains("pusch_buffer_index")) {
                    // PUSCH data from shared memory
                    SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr);
                    uint8_t pusch_buf_idx = payload_data.value("pusch_buffer_index", 0u);
                    uint32_t pusch_write_idx = payload_data.value("pusch_write_index", 0u);
                    
                    size_t pusch_base_offset = sizeof(SharedMemoryHeader) + (2 * header->fh_buffer_size);
                    size_t pusch_buffer_offset = pusch_buf_idx * header->pusch_buffer_size;
                    triton_input.data = e3::ShmInfo{pusch_base_offset + pusch_buffer_offset, (size_t)pusch_write_idx};
                    
                } else if (payload_data.contains("hest_buffer_index")) {
                    // Channel estimates (H) from shared memory (all UE groups concatenated)
                    SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr);
                    uint8_t hest_buf_idx = payload_data.value("hest_buffer_index", 0u);
                    uint32_t hest_row_byte_off = payload_data.value("hest_row_byte_offset", 0u);

                    // Total H-est size: max(h_offset + h_size) across ue_metrics[]
                    uint32_t total_h_size = 0;
                    if (indication_payload.contains("ue_metrics")) {
                        for (const auto& ue : indication_payload["ue_metrics"]) {
                            uint32_t end = ue.value("h_offset", 0u) + ue.value("h_size", 0u);
                            if (end > total_h_size) total_h_size = end;
                        }
                    }
                    if (total_h_size == 0) return;

                    size_t base_offset = sizeof(SharedMemoryHeader);
                    size_t hest_base_offset = base_offset + (2 * header->fh_buffer_size) + (2 * header->pusch_buffer_size);
                    size_t hest_buffer_offset = hest_buf_idx * header->hest_buffer_size;
                    size_t final_offset = hest_base_offset + hest_buffer_offset + hest_row_byte_off;

                    triton_input.data = e3::ShmInfo{final_offset, total_h_size * sizeof(std::complex<float>)};
                } else {
                    std::cerr << "Warning: Unknown shared memory type for '" << model_input.name << "'. Skipping." << std::endl;
                    continue;
                }
            } else {
                // Handle scalar inputs
                std::vector<uint8_t> raw_data;
                
                if (model_input.type == "UINT8") {
                    uint8_t value = payload_data.get<uint8_t>();
                    raw_data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(value));
                } else if (model_input.type == "UINT16") {
                    uint16_t value = payload_data.get<uint16_t>();
                    raw_data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(value));
                } else if (model_input.type == "INT16") {
                    int16_t value = payload_data.get<int16_t>();
                    raw_data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(value));
                } else if (model_input.type == "UINT32") {
                    uint32_t value = payload_data.get<uint32_t>();
                    raw_data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(value));
                } else if (model_input.type == "INT32") {
                    int32_t value = payload_data.get<int32_t>();
                    raw_data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(value));
                } else if (model_input.type == "FP16") {
                    uint16_t value = payload_data.get<uint16_t>();
                    raw_data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(value));
                } else if (model_input.type == "FP32") {
                    float value = payload_data.get<float>();
                    raw_data.assign(reinterpret_cast<uint8_t*>(&value), reinterpret_cast<uint8_t*>(&value) + sizeof(value));
                } else {
                    std::cerr << "Warning: Unhandled data type '" << model_input.type << "' for input '" << model_input.name << "'" << std::endl;
                    continue;
                }
                
                triton_input.data = raw_data;
            }
            triton_inputs.push_back(triton_input);
        }
        
        // std::cout << "Prepared all " << triton_inputs.size() << " required inputs for Triton inference" << std::endl;

        // 4. Get output names from metadata
        std::vector<std::string> output_names;
        for (const auto& model_output : metadata->outputs) {
            output_names.push_back(model_output.name);
        }
        if (output_names.empty()) {
            std::cout << "Warning: Model '" << model_name << "' has no specified outputs in metadata. Will request default 'output'." << std::endl;
            output_names.push_back("output");
        }
        
        // 5. Perform inference
        auto result = engine->infer(model_name, triton_inputs, output_names);
        
        if (result.success) {
            // auto total_latency = std::chrono::duration_cast<std::chrono::microseconds>(
            //     std::chrono::high_resolution_clock::now() - call_start_time).count();
            // std::cout << "Triton inference successful! Total client time: " << total_latency << "us" << std::endl;
            
            // Check if we have PRB power output to publish
            // Check if we have PRB power and timing outputs to publish
            bool has_prb_power = false;
            bool has_timing = false;
            const e3::TensorOutput* prb_output = nullptr;
            const e3::TensorOutput* timing_output = nullptr;
            
            for (const auto& output_pair : result.outputs) {
                // std::cout << "  Output '" << output_pair.first << "' received with size " 
                //           << output_pair.second.data.size() << " bytes." << std::endl;
                
                if (output_pair.first == "prb_power") {
                    has_prb_power = true;
                    prb_output = &output_pair.second;
                } else if (output_pair.first == "timing_us") {
                    has_timing = true;
                    timing_output = &output_pair.second;
                }
            }
            
            // Publish PRB power and timing data via ZMQ if available
            if (has_prb_power && prb_output && results_publisher) {
                try {
                    uint16_t sfn = indication_payload.value("sfn", 0u);
                    uint16_t slot = indication_payload.value("slot", 0u);
                    
                    // Calculate number of PRBs from data size (FP32 = 4 bytes)
                    size_t num_prbs = prb_output->data.size() / sizeof(float);
                    
                    // Metadata: "prb_power|sfn|slot|num_prbs"
                    char metadata_str[64];
                    snprintf(metadata_str, sizeof(metadata_str), "prb_power|%u|%u|%zu", sfn, slot, num_prbs);
                    
                    // Send metadata first
                    zmq::message_t meta_msg(metadata_str, strlen(metadata_str));
                    results_publisher->send(meta_msg, zmq::send_flags::sndmore);
                    
                    // Send PRB power data
                    zmq::message_t prb_msg(prb_output->data.data(), prb_output->data.size());
                    results_publisher->send(prb_msg, zmq::send_flags::sndmore);
                    
                    // Prepare timing data: [client_time, model_time (optional)]
                    // - client_time: Total C++ client latency (always available)
                    // - model_time: Model's internal inference time (from timing_us[0])
                    auto pub_latency = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - call_start_time).count();
                    std::vector<double> timing_data;
                    timing_data.push_back(static_cast<double>(pub_latency));
                    
                    if (has_timing && timing_output) {
                        // Add model's internal timing (first value is total model inference time)
                        const double* timing_ptr = reinterpret_cast<const double*>(timing_output->data.data());
                        timing_data.push_back(timing_ptr[0]);  // Model inference time
                    } else {
                        // No model timing available - use -1 to indicate N/A
                        timing_data.push_back(-1.0);
                    }
                    
                    // Send timing data
                    zmq::message_t timing_msg(timing_data.data(), timing_data.size() * sizeof(double));
                    results_publisher->send(timing_msg, zmq::send_flags::none);

                    //std::cout << "[PRB Power Triton] Published " << num_prbs
                    //          << " PRBs, timing: " << pub_latency << "us" << std::endl;
                    
                } catch (const std::exception& e) {
                    std::cerr << "[PRB Power Triton] ZMQ publish error: " << e.what() << std::endl;
                }
            }

            auto total_latency = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - call_start_time).count();
            uint16_t sfn = indication_payload.value("sfn", (uint16_t)0);
            uint16_t slot = indication_payload.value("slot", (uint16_t)0);
            std::cout << "[PRB Power Triton] [" << model_name << "] SFN/Slot: " << sfn << "/" << slot
                      << " Inference successful. Latency: " << total_latency << "us" << std::endl;
        } else {
            std::cerr << "[PRB Power] Triton inference failed for model '" << model_name << "': " << result.error_message << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception during processing: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Application configuration and entry point
// ---------------------------------------------------------------------------

static std::atomic<bool> g_stop{false};

static void signal_handler(int) {
    g_stop.store(true);
}

struct AppConfig {
    std::vector<E3Manager::E3AgentConfig> agents;
    std::string bind_addr = "tcp://*:5558";
    std::string dapp_name = "PRB Power";
    std::string dapp_version = "1.1.0";
    std::string vendor = "NVIDIA";
    std::string model_name = "prb_power_numpy";
    uint32_t subscription_response_timeout_s = 10;
    uint32_t results_pub_port = 5559;
    bool enable_results_publishing = true;
    std::string shm_key = "/e3_ran_buffers";
    bool shm_required = true;
    bool debug_enabled = false;

    // Triton
    std::string model_repo = "/models";
};

static AppConfig LoadConfig(const std::string& config_file) {
    AppConfig config;

    try {
        std::ifstream file(config_file);
        if (!file.is_open()) {
            std::cerr << "Warning: Could not open config file: " << config_file << std::endl;
            return config;
        }

        json j;
        file >> j;

        // Agents
        if (j.contains("e3_manager") && j["e3_manager"].contains("e3_agents")) {
            for (const auto& a : j["e3_manager"]["e3_agents"]) {
                if (!a.value("enabled", true)) continue;
                E3Manager::E3AgentConfig agent;
                agent.name = a.value("name", "default");
                agent.host = a.value("host", "localhost");
                agent.agent_rep_port = a.value("agent_rep_port", 5555);
                agent.agent_pub_port = a.value("agent_pub_port", 5556);
                agent.agent_sub_port = a.value("agent_sub_port", 5557);
                agent.enabled = true;
                agent.auto_setup = a.value("auto_setup", true);
                if (a.contains("subscription_options")) {
                    const auto& so = a["subscription_options"];
                    auto& sub = agent.auto_subscription;
                    sub.enabled = so.value("auto_subscribe", false);
                    sub.model = so.value("model", std::string{});
                    sub.telemetry_ids = so.value("telemetry_ids", std::vector<uint32_t>{});
                    sub.control_ids = so.value("control_ids", std::vector<uint32_t>{});
                    sub.ran_function_id = so.value("ran_function_id", 2u);
                    sub.periodicity_us = so.value("periodicity_us", 100000u);
                    sub.subscription_time_s = so.value("subscription_time_s", 0u);
                }
                config.agents.push_back(agent);
            }
        }

        // Application identity and settings
        if (j.contains("application")) {
            const auto& app = j["application"];
            config.dapp_name = app.value("name", config.dapp_name);
            config.dapp_version = app.value("version", config.dapp_version);
            config.vendor = app.value("vendor", config.vendor);
            config.results_pub_port = app.value("results_pub_port", config.results_pub_port);
            config.enable_results_publishing = app.value("enable_results_publishing", config.enable_results_publishing);
        }

        // E3 Manager settings
        if (j.contains("e3_manager")) {
            const auto& m = j["e3_manager"];
            config.bind_addr = m.value("dapp_client_endpoint", config.bind_addr);
            config.model_name = m.value("default_model", config.model_name);
            config.subscription_response_timeout_s = m.value("subscription_response_timeout_s", config.subscription_response_timeout_s);
            config.debug_enabled = m.value("debug_enabled", config.debug_enabled);
        }

        // Shared memory
        if (j.contains("deployment") && j["deployment"].contains("shared_memory")) {
            const auto& shm = j["deployment"]["shared_memory"];
            config.shm_key = shm.value("key", config.shm_key);
            config.shm_required = shm.value("required", config.shm_required);
        }

        // Triton
        if (j.contains("triton")) {
            const auto& t = j["triton"];
            config.model_repo = t.value("model_repository", config.model_repo);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << std::endl;
    }

    return config;
}

int main(int argc, char* argv[]) {
    std::string config_file = "/opt/config/e3_config.json";
    bool cli_debug = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "--debug") == 0) {
            cli_debug = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            std::cout << "PRB Power dApp (Triton)\n"
                      << "Usage: " << argv[0] << " [--config <file>] [--debug]\n";
            return 0;
        }
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    auto config = LoadConfig(config_file);
    if (cli_debug) config.debug_enabled = true;
    if (config.agents.empty()) {
        std::cerr << "Error: No agents in config: " << config_file << std::endl;
        return 1;
    }

    std::cout << "Starting PRB Power dApp (Triton C API)..." << std::endl;
    std::cout << "  Config:     " << config_file << std::endl;
    std::cout << "  Agents:     " << config.agents.size() << std::endl;
    std::cout << "  Model repo: " << config.model_repo << std::endl;
    std::cout << "  Model:      " << config.model_name << std::endl;

    // Create Triton inference engine (in-process via C API)
    e3::TritonEngine engine(config.model_repo);

    // Create E3 Manager
    const uint32_t pub_port = config.enable_results_publishing ? config.results_pub_port : 0;
    E3Manager manager(config.bind_addr, config.agents, &engine, config.model_name,
                      config.dapp_name, config.dapp_version, config.vendor,
                      pub_port, config.subscription_response_timeout_s,
                      config.shm_key, config.shm_required);

    manager.SetIndicationHandler(ProcessPRBPower);
    manager.debug_enabled = config.debug_enabled;

    manager.Start();

    std::cout << "PRB Power dApp (Triton C API) running. Press Ctrl+C to stop." << std::endl;
    while (manager.IsRunning() && !g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (g_stop.load()) {
        std::cout << "\nReceived shutdown signal, stopping..." << std::endl;
        manager.Stop();
    }

    std::cout << "PRB Power dApp shutdown complete." << std::endl;
    return 0;
}

