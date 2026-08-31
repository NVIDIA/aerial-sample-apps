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

#include "e3_manager.h"
#include <zmq.hpp>
#include <iostream>
#include <sstream>
#include <chrono>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <nlohmann/json.hpp>
#include <cerrno>

using json = nlohmann::json;

E3Manager::E3Manager(const std::string& bind_address,
                     const std::vector<E3AgentConfig>& agent_configs,
                     e3::InferenceEngine* engine,
                     const std::string& default_model_name,
                     const std::string& dapp_name,
                     const std::string& dapp_version,
                     const std::string& vendor,
                     uint32_t results_pub_port,
                     uint32_t subscription_response_timeout_s,
                     const std::string& shm_key,
                     bool shm_required)
    : bind_address_(bind_address), engine_(engine), 
      model_name_(default_model_name),
      dapp_name_(dapp_name), dapp_version_(dapp_version), vendor_(vendor),
      running_(false),
      results_pub_port_(results_pub_port),
      subscription_response_timeout_s_(subscription_response_timeout_s),
      shm_key_(shm_key),
      shm_required_(shm_required) {
    
    // Initialize agent states from configs
    for (const auto& config : agent_configs) {
        auto agent_state = std::make_unique<E3AgentState>();
        agent_state->config = config;
        agent_state->auto_setup_active = config.auto_setup;
        agent_state->auto_subscribe_active = config.auto_subscription.enabled;
        agents_[config.name] = std::move(agent_state);
    }
    
    std::cout << "E3 Manager initialized:" << std::endl;
    std::cout << "  dApp client interface: " << bind_address_ << std::endl;
    std::cout << "  E3 Agents configured: " << agents_.size() << std::endl;
    for (const auto& [name, agent] : agents_) {
        std::cout << "    " << name << ": " << agent->config.host << ":" 
                  << agent->config.agent_rep_port << " (REP), :" << agent->config.agent_pub_port << " (PUB), :"
                  << agent->config.agent_sub_port << " (SUB)"
                  << " [auto_setup=" << (agent->config.auto_setup ? "on" : "off")
                  << ", auto_subscribe=" << (agent->config.auto_subscription.enabled ? "on" : "off") << "]"
                  << std::endl;
    }
    std::cout << "  Default inference model: " << model_name_ << std::endl;
}

E3Manager::~E3Manager() {
    Stop();
}

void E3Manager::Start() {
    running_ = true;
    stopped_ = false;
    
    // Open shared memory for RAN buffers
    OpenRanSharedMemory();
    
    // Initialize inference engine and register shared memory if available
    if (engine_) {
        engine_->Initialize(ran_shm_size_);
        if (ran_shm_size_ > 0) {
            engine_shm_registered_ = engine_->RegisterSharedMemory(shm_key_, ran_shm_size_);
        }
    }
    
    // Initialize results publisher (best-effort, non-critical). Port 0 disables publishing.
    if (results_pub_port_ > 0) {
        try {
            results_context_ = std::make_shared<zmq::context_t>(1);
            results_publisher_ = std::make_unique<zmq::socket_t>(*results_context_, zmq::socket_type::pub);
            
            const std::string results_pub_addr = "tcp://*:" + std::to_string(results_pub_port_);
            results_publisher_->bind(results_pub_addr);
            std::cout << "Results publisher ready on " << results_pub_addr << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not initialize results publisher: " << e.what() << std::endl;
            results_publisher_.reset();
            results_context_.reset();
        }
    }
    
    // Initialize per-agent PUB sockets (Manager → Agent)
    for (auto& [name, agent] : agents_) {
        try {
            if (!agent->zmq_context) {
                agent->zmq_context = std::make_shared<zmq::context_t>(1);
            }
            CreateAgentPubSocket(*agent);
            std::cout << "Manager PUB connected to Agent " << name << " SUB at tcp://"
                      << agent->config.host << ":" << agent->config.agent_sub_port << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not initialize PUB socket for agent " << name << ": " << e.what() << std::endl;
            agent->pub_socket.reset();
        }
    }

    // Start threads
    client_thread_ = std::thread(&E3Manager::ClientRequestLoop, this);
    service_thread_ = std::thread(&E3Manager::ServiceLoop, this);

    std::cout << "E3 Manager started" << std::endl;
}

void E3Manager::Stop() {
    if (stopped_.exchange(true)) return;

    // Release connected agents before tearing down.
    for (auto& [agent_name, agent_state] : agents_) {
        if (agent_state->state == e3::E3State::CONNECTED && agent_state->dapp_id != 0) {
            nlohmann::json release_msg;
            release_msg["type"] = "releaseMessage";
            release_msg["id"] = GenerateMessageId();
            release_msg["dAppIdentifier"] = agent_state->dapp_id;
            PublishToAgent(agent_name, release_msg.dump());
            std::cout << "Agent " << agent_name << ": Sent e3_release on shutdown" << std::endl;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let PUB flush before close

    running_ = false;
    
    // Stop all per-agent subscription threads
    for (auto& [agent_name, agent_state] : agents_) {
        if (agent_state->sub_thread.joinable()) {
            agent_state->sub_thread.join();
        }
    }
    
    // Join before teardown so no thread can touch the sockets or contexts.
    if (service_thread_.joinable()) service_thread_.join();
    if (client_thread_.joinable()) client_thread_.join();

    // Close sockets before their context: zmq_ctx_term blocks until every socket on it is closed.
    for (auto& [agent_name, agent_state] : agents_) {
        {
            std::lock_guard<std::mutex> lock(agent_state->pub_socket_mutex);
            if (agent_state->pub_socket) {
                agent_state->pub_socket->close();
                agent_state->pub_socket.reset();
            }
        }
        {
            std::lock_guard<std::mutex> lock(agent_state->req_socket_mutex);
            if (agent_state->req_socket) {
                agent_state->req_socket->close();
                agent_state->req_socket.reset();
            }
        }
        if (agent_state->zmq_context) {
            agent_state->zmq_context->close();
        }
    }

    // Clear any remaining subscription data and unload models
    for (auto& [agent_name, agent_state] : agents_) {
        if (agent_state->subscription.has_value()) {
            std::cout << "Clearing subscription for agent " << agent_name << " during shutdown..." << std::endl;
            std::string model_to_clear = agent_state->subscription->model_name;
            ClearSubscription(*agent_state, agent_name);
            if (engine_ && !model_to_clear.empty()) {
                std::cout << "Unloading model during shutdown: " << model_to_clear << std::endl;
                engine_->UnloadModel(model_to_clear);
            }
        }
    }

    // Shut down inference engine (after models are unloaded)
    if (engine_) {
        engine_->Shutdown();
    }
    
    // Close results publisher
    try {
        if (results_publisher_) {
            results_publisher_->close();
            results_publisher_.reset();
        }
        if (results_context_) {
            results_context_->close();
            results_context_.reset();
        }
        std::cout << "Results publisher closed" << std::endl;
    } catch (const zmq::error_t& e) {
        std::cerr << "Error closing results publisher: " << e.what() << std::endl;
    }
    
    CloseRanSharedMemory();
    std::cout << "E3 Manager stopped" << std::endl;
}

void E3Manager::OpenRanSharedMemory() {
    // Open the RAN shared memory created by cuBB
    ran_shm_fd_ = shm_open(shm_key_.c_str(), O_RDONLY, 0666);
    if (ran_shm_fd_ == -1) {
        if (shm_required_) {
            std::cerr << "Shared memory " << shm_key_ << ": Not available, will retry..." << std::endl;
        }
        return;
    }
    
    struct stat shm_stat;
    if (fstat(ran_shm_fd_, &shm_stat) == 0) {
        ran_shm_size_ = shm_stat.st_size;
        ran_shm_ptr_ = mmap(nullptr, ran_shm_size_, PROT_READ, MAP_SHARED, ran_shm_fd_, 0);
        if (ran_shm_ptr_ != MAP_FAILED) {
            std::cout << "Opened RAN shared memory " << shm_key_ << std::endl;
            std::cout << "  Size: " << ran_shm_size_ << " bytes (" 
                      << (ran_shm_size_ / 1024.0 / 1024.0) << " MB)" << std::endl;
            std::cout << "  Address: " << ran_shm_ptr_ << std::endl;
                      
            // Read header to understand buffer layout
            SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr_);
            std::cout << "  FH buffer size: " << header->fh_buffer_size << " bytes" << std::endl;
            std::cout << "  PUSCH buffer size: " << header->pusch_buffer_size << " bytes" << std::endl;
            std::cout << "  H estimates buffer size: " << header->hest_buffer_size << " bytes" << std::endl;
            std::cout << "  Num FH samples: " << header->num_fh_samples << std::endl;
            std::cout << "  Num FH rows: " << header->num_fh_rows << std::endl;
            std::cout << "  Num PUSCH rows: " << header->num_pusch_rows << std::endl;
            std::cout << "  Num H estimates rows: " << header->num_hest_rows << std::endl;
            std::cout << "  Max H estimates samples per row: " << header->max_hest_samples_per_row << std::endl;
        } else {
            std::cerr << "Failed to map RAN shared memory: " << strerror(errno) << std::endl;
            ran_shm_ptr_ = nullptr;
            ran_shm_size_ = 0;
            close(ran_shm_fd_);
            ran_shm_fd_ = -1;
        }
    } else {
        std::cerr << "Failed to stat RAN shared memory: " << strerror(errno) << std::endl;
        close(ran_shm_fd_);
        ran_shm_fd_ = -1;
    }
}

void E3Manager::CloseRanSharedMemory() {
    if (ran_shm_ptr_ != nullptr && ran_shm_ptr_ != MAP_FAILED) {
        munmap(ran_shm_ptr_, ran_shm_size_);
        ran_shm_ptr_ = nullptr;
    }
    if (ran_shm_fd_ != -1) {
        close(ran_shm_fd_);
        ran_shm_fd_ = -1;
    }
}

void E3Manager::ClearSubscription(E3AgentState& agent, const std::string& agent_name) {
    if (!agent.subscription.has_value()) return;

    std::string model_to_clear = agent.subscription->model_name;
    std::cout << "Agent " << agent_name << ": Clearing subscription" << std::endl;
    agent.subscription.reset();

    bool model_in_use = false;
    for (const auto& [name, other] : agents_) {
        if (name != agent_name && other->subscription.has_value() &&
            other->subscription->model_name == model_to_clear) {
            model_in_use = true;
            break;
        }
    }
    if (!model_in_use && !model_to_clear.empty()) {
        std::lock_guard<std::mutex> lock(model_metadata_mutex_);
        model_metadata_cache_.erase(model_to_clear);
    }
}

bool E3Manager::HandleE3Subscription(const std::vector<uint32_t>& telemetry_ids,
                                    const std::vector<uint32_t>& control_ids,
                                    uint32_t ran_function_id,
                                    uint32_t periodicity_us,
                                    uint32_t subscription_time_s,
                                    const std::string& model_name, 
                                    const std::string& agent_name,
                                    std::string& error_message) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) {
        error_message = "Agent '" + agent_name + "' not found";
        return false;
    }
    E3AgentState& agent = *it->second;
    
    // Check agent is connected
    if (agent.state != e3::E3State::CONNECTED) {
        error_message = "Agent '" + agent_name + "' not connected";
        return false;
    }
    
    // Check if agent already has an active subscription (limit one subscription per agent for now)
    if (agent.subscription.has_value()) {
        error_message = "Agent '" + agent_name + "' already has an active subscription (ID: " + 
                       std::to_string(agent.subscription->subscription_id) + ")";
        return false;
    }
    
    // Verify model is available before sending subscription to E3 Agent
    // (Skipped when no inference engine is configured. dApp handles inference natively)
    if (engine_) {
        std::cout << "Pre-verifying model '" << model_name << "' availability..." << std::endl;
        auto metadata = engine_->GetModelMetadata(model_name);
        
        if (!metadata) {
            error_message = "Model '" + model_name + "' not available";
            std::cerr << error_message << ". Subscription rejected without contacting E3 Agent." << std::endl;
            return false;
        }
        
        std::cout << "Model '" << model_name << "' verified (platform: '" << metadata->platform << "')" << std::endl;
        for (const auto& input : metadata->inputs) {
            std::cout << "  Input: " << input.name << ", Type: " << input.type << std::endl;
        }
        
        // Store model metadata
        {
            std::lock_guard<std::mutex> lock(model_metadata_mutex_);
            model_metadata_cache_[model_name] = *metadata;
        }
    } else {
        std::cout << "No inference engine configured, skipping model verification for '" << model_name << "'" << std::endl;
    }
    
    std::cout << "Requesting subscription for agent '" << agent_name << "' (dApp ID: " << agent.dapp_id 
              << ") with verified model '" << model_name << "' and periodicity " << periodicity_us << "us" << std::endl;
    
    // Build and publish subscription request via PUB-SUB (dApp PUB -> Agent SUB)
    json sub_req = CreateE3SubscriptionRequestMessage(telemetry_ids, control_ids, ran_function_id, periodicity_us, subscription_time_s);
    sub_req["dAppIdentifier"] = agent.dapp_id;
    uint32_t request_id = sub_req["id"].get<uint32_t>();
    
    std::cout << "Publishing E3 Subscription Request: " << sub_req.dump() << std::endl;
    PublishToAgent(agent_name, sub_req.dump());
    
    // Store as pending: sub loop will confirm or rollback on response/timeout
    {
        std::lock_guard<std::mutex> lock(agent.subscription_mutex);
        agent.subscription = {0, SubscriptionStatus::PENDING_REQUEST, ran_function_id, telemetry_ids, periodicity_us, subscription_time_s, std::chrono::steady_clock::now(), model_name};
        agent.pending_request_id = request_id;
        agent.pending_request_time = std::chrono::steady_clock::now();
    }
    
    std::cout << "Subscription request sent for agent '" << agent_name << "' (request_id: " << request_id << ")" << std::endl;
    return true;
}

bool E3Manager::HandleE3Setup(const std::string& agent_name, zmq::context_t& ctx, std::string& error_message) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) {
        error_message = "Agent '" + agent_name + "' not found";
        return false;
    }
    E3AgentState& agent = *it->second;

    if (agent.state == e3::E3State::CONNECTED) {
        error_message = "Agent '" + agent_name + "' already connected";
        return false;
    }

    try {
        zmq::socket_t socket(ctx, ZMQ_REQ);
        socket.set(zmq::sockopt::rcvtimeo, 1000);
        socket.set(zmq::sockopt::sndtimeo, 1000);
        socket.set(zmq::sockopt::linger, 0);
        socket.set(zmq::sockopt::sndhwm, 1);
        socket.set(zmq::sockopt::rcvhwm, 1);
        socket.set(zmq::sockopt::immediate, 1);

        socket.set(zmq::sockopt::tcp_keepalive, 1);
        socket.set(zmq::sockopt::tcp_keepalive_idle, static_cast<int>(TCP_KEEPALIVE_IDLE_S));
        socket.set(zmq::sockopt::tcp_keepalive_intvl, static_cast<int>(TCP_KEEPALIVE_INTERVAL_S));
        socket.set(zmq::sockopt::tcp_keepalive_cnt, static_cast<int>(TCP_KEEPALIVE_COUNT));

        socket.connect("tcp://" + agent.config.host + ":" + std::to_string(agent.config.agent_rep_port));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        json setup_req = CreateE3SetupRequestMessage();
        auto send_result = socket.send(zmq::buffer(setup_req.dump()), zmq::send_flags::dontwait);
        if (!send_result) {
            error_message = "No E3 Agent available";
            socket.close();
            return false;
        }

        zmq::message_t setup_response;
        if (!socket.recv(setup_response)) {
            error_message = "E3 Setup response timeout";
            socket.close();
            return false;
        }

        std::string resp_str(static_cast<char*>(setup_response.data()), setup_response.size());
        uint32_t received_dapp_id = 0;

        if (!ParseE3SetupResponseMessage(resp_str, received_dapp_id)) {
            error_message = "E3 Setup failed";
            socket.close();
            return false;
        }

        if (received_dapp_id != 0) {
            agent.dapp_id = received_dapp_id;
            std::cout << "Agent " << agent_name << ": E3 Agent assigned dApp ID: " << agent.dapp_id << std::endl;
        }

        agent.state = e3::E3State::CONNECTED;

        // Clear stale subscription from previous connection
        {
            std::lock_guard<std::mutex> lock(agent.subscription_mutex);
            ClearSubscription(agent, agent_name);
        }

        // Wait for old subscription thread to finish
        if (agent.sub_thread.joinable()) {
            agent.sub_thread.join();
        }

        // Clean up old resources
        {
            std::lock_guard<std::mutex> pub_lock(agent.pub_socket_mutex);
            agent.pub_socket.reset();
        }
        {
            std::lock_guard<std::mutex> req_lock(agent.req_socket_mutex);
            agent.req_socket.reset();
        }
        agent.zmq_context.reset();

        // Create new ZMQ context for this agent
        agent.zmq_context = std::make_shared<zmq::context_t>(1);

        // Initialize persistent REQ socket for this agent
        {
            std::lock_guard<std::mutex> req_lock(agent.req_socket_mutex);
            agent.req_socket = CreateAgentReqSocket(agent);
        }

        // Recreate PUB socket with new context
        CreateAgentPubSocket(agent);

        // Start new subscription thread
        agent.sub_thread = std::thread(&E3Manager::E3AgentSubscriptionLoop, this, agent_name);
        std::cout << "Agent " << agent_name << ": Started indication subscription thread" << std::endl;

        socket.close();
        return true;

    } catch (const zmq::error_t& e) {
        error_message = "ZMQ error during setup: " + std::string(e.what());
        return false;
    }
}

bool E3Manager::HandleE3SubscriptionDelete(const std::string& agent_name, std::string& error_message) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) {
        error_message = "Agent '" + agent_name + "' not found";
        std::cerr << error_message << std::endl;
        return false;
    }
    E3AgentState& agent = *it->second;
    
    // Check if agent has active subscription
    if (!agent.subscription.has_value()) {
        error_message = "Agent '" + agent_name + "' has no active subscription";
        std::cerr << error_message << std::endl;
        return false;
    }
    
    // Guard: reject delete if subscription is not confirmed
    // Current design: cannot delete a subscription before receiving confirmation.
    if (agent.subscription->status != SubscriptionStatus::CONFIRMED) {
        error_message = "Agent '" + agent_name + "': subscription not confirmed (status=" +
                       std::to_string(static_cast<int>(agent.subscription->status)) + "), retry shortly";
        std::cerr << error_message << std::endl;
        return false;
    }
    
    // Explicit unsubscribe overrides auto-subscription (would otherwise re-subscribe next cycle)
    agent.auto_subscribe_active = false;

    uint32_t sub_id_to_cancel = agent.subscription->subscription_id;
    std::cout << "Requesting subscription delete for agent '" << agent_name 
              << "' subscription_id: " << sub_id_to_cancel << std::endl;
    
    // Build and publish subscription delete via PUB-SUB
    json del_req = CreateE3SubscriptionDeleteMessage(sub_id_to_cancel);
    del_req["dAppIdentifier"] = agent.dapp_id;
    uint32_t request_id = del_req["id"].get<uint32_t>();
    
    std::cout << "Publishing E3 Subscription Delete: " << del_req.dump() << std::endl;
    PublishToAgent(agent_name, del_req.dump());
    
    // Mark pending delete — sub loop will confirm deletion or timeout will clean up
    {
        std::lock_guard<std::mutex> lock(agent.subscription_mutex);
        agent.subscription->status = SubscriptionStatus::PENDING_DELETE;
        agent.pending_request_id = request_id;
        agent.pending_request_time = std::chrono::steady_clock::now();
    }
    
    std::cout << "Subscription delete request sent for agent '" << agent_name 
              << "' (request_id: " << request_id << ")" << std::endl;
    return true;
}

// Release: clean up local state and terminate subscription loop for this agent
void E3Manager::HandleE3Release(const std::string& agent_name) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) return;
    E3AgentState& agent = *it->second;

    {
        std::lock_guard<std::mutex> lock(agent.subscription_mutex);
        ClearSubscription(agent, agent_name);
    }

    {
        std::lock_guard<std::mutex> lock(agent.pub_socket_mutex);
        agent.pub_socket.reset();
    }

    {
        std::lock_guard<std::mutex> lock(agent.req_socket_mutex);
        if (agent.req_socket) {
            agent.req_socket->close();
            agent.req_socket.reset();
        }
    }

    // Close ZMQ context to terminate subscription loop (triggers ETERM)
    if (agent.zmq_context) {
        agent.zmq_context->close();
        agent.zmq_context.reset();
    }

    // Wait for subscription thread to finish
    if (agent.sub_thread.joinable()) {
        agent.sub_thread.join();
    }

    agent.dapp_id = 0;
    agent.state = e3::E3State::DISCONNECTED;
    agent.auto_setup_active = false;
    agent.auto_subscribe_active = false;
    std::cout << "Agent " << agent_name << ": Released (auto_setup disabled)" << std::endl;
}

// dApp-initiated release: publish e3_release on PUB socket and clean up locally
void E3Manager::SendE3Release(const std::string& agent_name) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) {
        std::cerr << "Agent '" << agent_name << "' not found for release" << std::endl;
        return;
    }
    E3AgentState& agent = *it->second;

    if (agent.state != e3::E3State::CONNECTED || agent.dapp_id == 0) {
        std::cerr << "Agent '" << agent_name << "' not connected, nothing to release" << std::endl;
        return;
    }

    nlohmann::json release_msg;
    release_msg["type"] = "releaseMessage";
    release_msg["id"] = GenerateMessageId();
    release_msg["dAppIdentifier"] = agent.dapp_id;

    PublishToAgent(agent_name, release_msg.dump());
    std::cout << "Agent " << agent_name << ": Sent e3_release (dAppIdentifier=" << agent.dapp_id << ")" << std::endl;

    HandleE3Release(agent_name);
}

void E3Manager::ClientRequestLoop() {
    try {
        zmq::context_t context(1);
        zmq::socket_t socket(context, ZMQ_REP);
        socket.bind(bind_address_);
        
        std::cout << "dApp client interface listening on " << bind_address_ << std::endl;
        
        while (running_) {
            zmq::message_t request;
            if (socket.recv(request, zmq::recv_flags::dontwait)) {
                
                json response_json;
                try {
                    json req_json = json::parse(std::string(static_cast<char*>(request.data()), request.size()));
                    std::cout << "\nReceived request from external client: " << req_json.dump() << std::endl;
                
                    std::string cmd = req_json.value("cmd", "");

                    if (cmd == "subscribe") {
                        if (!req_json.contains("telemetryIdentifierList")) {
                            response_json["status"] = "error";
                            response_json["message"] = "subscribe request requires 'telemetryIdentifierList'";
                        } else {
                            std::vector<uint32_t> telemetry_ids = req_json["telemetryIdentifierList"].get<std::vector<uint32_t>>();
                            std::vector<uint32_t> control_ids = req_json.value("controlIdentifierList", std::vector<uint32_t>{});
                            uint32_t ran_function_id = req_json.value("ranFunctionIdentifier", 2u);
                            uint32_t periodicity = req_json.value("periodicity", 100000u);
                            uint32_t subscription_time = req_json.value("subscriptionTime", 0u);
                            std::string model_name = req_json.value("model_name", model_name_);
                            std::string agent_name = req_json.value("agent", "");
                            
                            if (!indication_handler_) {
                                response_json["status"] = "error";
                                response_json["message"] = "No indication handler set";
                            } else if (agent_name.empty()) {
                                response_json["status"] = "error";
                                response_json["message"] = "No connected agents available";
                            } else {
                                std::string subscription_error;
                                bool success = HandleE3Subscription(telemetry_ids, control_ids, ran_function_id, periodicity, subscription_time, model_name, agent_name, subscription_error);
                                if (success) {
                                    response_json["status"] = "sent";
                                    response_json["message"] = "subscription request sent";
                                    response_json["agent"] = agent_name;
                                } else {
                                    response_json["status"] = "error";
                                    response_json["message"] = subscription_error;
                                }
                            }
                        }

                    } else if (cmd == "unsubscribe") {
                        std::string agent_name = req_json.value("agent", "");
                        if (agent_name.empty()) {
                            response_json["status"] = "error";
                            response_json["message"] = "No agent specified";
                        } else {
                            std::string error_msg;
                            bool success = HandleE3SubscriptionDelete(agent_name, error_msg);
                            if (success) {
                                response_json["status"] = "sent";
                                response_json["message"] = "subscription delete sent";
                                response_json["agent"] = agent_name;
                            } else {
                                response_json["status"] = "error";
                                response_json["message"] = error_msg;
                            }
                        }
                    } else if (cmd == "status") {
                        response_json["status"] = "ok";
                        response_json["agents"] = json::array();
                        
                        // Report status for each agent
                        for (const auto& [name, agent] : agents_) {
                            json agent_status;
                            agent_status["name"] = name;
                            agent_status["state"] = (agent->state == e3::E3State::CONNECTED) ? "connected" : "disconnected";
                            agent_status["is_subscribed"] = agent->subscription.has_value();
                            if (agent->subscription.has_value()) {
                                agent_status["subscription_id"] = agent->subscription->subscription_id;
                                agent_status["subscription_status"] = 
                                    agent->subscription->status == SubscriptionStatus::CONFIRMED ? "confirmed" :
                                    agent->subscription->status == SubscriptionStatus::PENDING_REQUEST ? "pending_request" :
                                    "pending_delete";
                                agent_status["model"] = agent->subscription->model_name;
                            }
                            response_json["agents"].push_back(agent_status);
                        }
                    } else if (cmd == "e3_setup") {
                        std::string agent_name = req_json.value("agent", "");
                        zmq::context_t setup_ctx(1);
                        if (agent_name.empty()) {
                            // Setup all disconnected agents
                            response_json["status"] = "ok";
                            response_json["results"] = json::array();
                            for (const auto& [name, _] : agents_) {
                                json result;
                                result["agent"] = name;
                                std::string setup_error;
                                if (HandleE3Setup(name, setup_ctx, setup_error)) {
                                    result["result"] = "connected";
                                } else {
                                    result["result"] = setup_error;
                                }
                                response_json["results"].push_back(result);
                            }
                        } else {
                            std::string setup_error;
                            if (HandleE3Setup(agent_name, setup_ctx, setup_error)) {
                                response_json["status"] = "ok";
                                response_json["message"] = "setup complete";
                                response_json["agent"] = agent_name;
                            } else {
                                response_json["status"] = "error";
                                response_json["message"] = setup_error;
                            }
                        }

                    } else if (cmd == "e3_release") {
                        std::string agent_name = req_json.value("agent", "");
                        if (agent_name.empty()) {
                            // Release all agents
                            response_json["status"] = "ok";
                            response_json["results"] = json::array();
                            for (const auto& [name, agent] : agents_) {
                                json result;
                                result["agent"] = name;
                                if (agent->state == e3::E3State::CONNECTED) {
                                    SendE3Release(name);
                                    result["result"] = "released";
                                } else {
                                    result["result"] = "not connected";
                                }
                                response_json["results"].push_back(result);
                            }
                        } else {
                            auto it = agents_.find(agent_name);
                            if (it == agents_.end()) {
                                response_json["status"] = "error";
                                response_json["message"] = "agent not found";
                            } else if (it->second->state != e3::E3State::CONNECTED) {
                                response_json["status"] = "error";
                                response_json["message"] = "agent not connected";
                                response_json["agent"] = agent_name;
                            } else {
                                SendE3Release(agent_name);
                                response_json["status"] = "ok";
                                response_json["message"] = "released";
                                response_json["agent"] = agent_name;
                            }
                        }

                    } else if (cmd == "list_agents") {
                        response_json["status"] = "ok";
                        response_json["agents"] = json::array();
                        
                        for (const auto& [name, agent] : agents_) {
                            json agent_info;
                            agent_info["name"] = name;
                            agent_info["host"] = agent->config.host;
                            agent_info["agent_rep_port"] = agent->config.agent_rep_port;
                            agent_info["agent_pub_port"] = agent->config.agent_pub_port;
                            agent_info["agent_sub_port"] = agent->config.agent_sub_port;
                            agent_info["state"] = (agent->state == e3::E3State::CONNECTED) ? "connected" : "disconnected";
                            response_json["agents"].push_back(agent_info);
                        }
                    } else {
                        response_json["status"] = "error";
                        response_json["message"] = "unknown command";
                    }
                } catch (json::parse_error& e) {
                    std::cerr << "Failed to parse client request: " << e.what() << std::endl;
                    response_json["status"] = "error";
                    response_json["message"] = "invalid JSON format";
                }
                socket.send(zmq::buffer(response_json.dump()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } catch (const zmq::error_t& e) {
        std::cerr << "Client loop error: " << e.what() << std::endl;
    }
}

void E3Manager::E3AgentSubscriptionLoop(const std::string& agent_name) {
    // Get agent reference
    E3AgentState* agent_ptr = nullptr;
    {
        auto it = agents_.find(agent_name);
        if (it == agents_.end()) {
            std::cerr << "Agent " << agent_name << " not found for subscription loop" << std::endl;
            return;
        }
        agent_ptr = it->second.get();
    }
    
    E3AgentState& agent = *agent_ptr;
    
    try {
        zmq::socket_t socket(*agent.zmq_context, ZMQ_SUB);
        
        // Enable socket monitoring to detect disconnections
        std::string monitor_addr = "inproc://monitor-" + agent_name;
        zmq_socket_monitor(socket.handle(), monitor_addr.c_str(), ZMQ_EVENT_DISCONNECTED | ZMQ_EVENT_CLOSED);
        
        // Create monitor socket to receive connection events
        zmq::socket_t monitor(*agent.zmq_context, ZMQ_PAIR);
        monitor.connect(monitor_addr);
        
        std::stringstream addr;
        addr << "tcp://" << agent.config.host << ":" << agent.config.agent_pub_port;
        socket.connect(addr.str());
        
        socket.set(zmq::sockopt::rcvtimeo, 50);   // 50ms for responsive disconnect detection
        monitor.set(zmq::sockopt::rcvtimeo, 0);   // Non-blocking monitor
        
        socket.set(zmq::sockopt::tcp_keepalive, 1);
        socket.set(zmq::sockopt::tcp_keepalive_idle, static_cast<int>(TCP_KEEPALIVE_IDLE_S));
        socket.set(zmq::sockopt::tcp_keepalive_intvl, static_cast<int>(TCP_KEEPALIVE_INTERVAL_S));
        socket.set(zmq::sockopt::tcp_keepalive_cnt, static_cast<int>(TCP_KEEPALIVE_COUNT));
        
        std::cout << "Agent " << agent_name << ": Connected to E3 Agent indications on " << addr.str() << std::endl;
        
        // Subscribe to all messages from this agent
        socket.set(zmq::sockopt::subscribe, "");
        std::cout << "Agent " << agent_name << ": Subscribed to all messages" << std::endl;
        
        while (running_) {
            
            // Check monitor socket for disconnection events (non-blocking)
            // Drain all pending events to stay current
            bool should_disconnect = false;
            zmq::message_t event_msg, endpoint_msg;
            while (monitor.recv(event_msg, zmq::recv_flags::dontwait).has_value()) {
                // Monitor events come as two frames: event struct + endpoint string
                auto endpoint_result = monitor.recv(endpoint_msg, zmq::recv_flags::dontwait);  // Must read 2nd frame
                if (!endpoint_result.has_value()) continue;  // Skip if 2nd frame missing
                
                // Parse event safely (struct: uint16_t event, int32_t value)
                if (event_msg.size() >= sizeof(uint16_t)) {
                    struct zmq_event_t {
                        uint16_t event;
                        int32_t value;
                    } event;
                    memcpy(&event, event_msg.data(), sizeof(uint16_t));  // Just need event type
                    
                    if (event.event == ZMQ_EVENT_DISCONNECTED || event.event == ZMQ_EVENT_CLOSED) {
                        std::cerr << "Agent " << agent_name << ": Connection lost (ZMQ event: " 
                                  << event.event << ")" << std::endl;
                        should_disconnect = true;
                    }
                }
            }
            
            if (should_disconnect) {
                agent.state = e3::E3State::DISCONNECTED;
                break;
            }
            
            // Receive messages (indications or release)
            zmq::message_t message;
            auto result = socket.recv(message, zmq::recv_flags::none);
            
            if (result.has_value()) {
                try {
                    const json msg = json::parse(std::string(static_cast<const char*>(message.data()), message.size()));
                    const std::string type = msg.value("type", "");

                    if (type == "subscriptionResponse") {
                        ProcessE3SubscriptionResponse(msg, agent_name);
                    } else if (type == "indicationMessage") {
                        ProcessE3Indication(msg, agent_name);
                    } else if (type == "messageAck") {
                        // Optional ack from agent (e.g. control message confirmation)
                    } else if (type == "releaseMessage") {
                        if (ProcessE3Release(msg, agent_name)) break;
                    }
                } catch (const json::exception& e) {
                    std::cerr << "Agent " << agent_name << ": Failed to parse PUB message: " << e.what() << std::endl;
                }
            }
        }
    } catch (const zmq::error_t& e) {
        if (e.num() != ETERM) {    // ETERM = expected unblock on context teardown
            std::cerr << "Agent " << agent_name << ": Notification loop error: " << e.what() << std::endl;
        }
        ClearSubscription(agent, agent_name);
        agent.state = e3::E3State::DISCONNECTED;
    }
}

void E3Manager::ProcessE3SubscriptionResponse(const json& response, const std::string& agent_name) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) return;
    E3AgentState& agent = *it->second;

    uint32_t msg_dapp_id = response.value("dAppIdentifier", 0u);
    if (msg_dapp_id != agent.dapp_id) return;
    
    uint32_t request_id = response.value("requestId", 0u);
    std::string response_code = response.value("responseCode", "");
    
    std::lock_guard<std::mutex> lock(agent.subscription_mutex);
    
    // Ignore stale responses (request_id doesn't match pending)
    if (request_id == 0 || request_id != agent.pending_request_id) {
        std::cerr << "Agent " << agent_name << ": Ignoring stale subscription response"
                  << " (got requestId=" << request_id 
                  << ", expected=" << agent.pending_request_id << ")" << std::endl;
        return;
    }
    
    agent.pending_request_id = 0;  // Clear pending state
    
    if (!agent.subscription.has_value()) return;
    
    if (response_code == "positive") {
        if (agent.subscription->status == SubscriptionStatus::PENDING_REQUEST) {
            // Confirm subscription: update with granted values from agent
            agent.subscription->subscription_id = response.value("subscriptionId", 0u);
            agent.subscription->status = SubscriptionStatus::CONFIRMED;
            if (response.contains("telemetryGrantedList")) {
                agent.subscription->telemetry_ids = 
                    response["telemetryGrantedList"].get<std::vector<uint32_t>>();
            }
            if (response.contains("periodicity")) {
                agent.subscription->periodicity_us = response["periodicity"].get<uint32_t>();
            }
            std::cout << "Agent " << agent_name << ": Subscription confirmed (id=" 
                      << agent.subscription->subscription_id << ", responseCode=" << response_code << ")" << std::endl;
                      
        } else if (agent.subscription->status == SubscriptionStatus::PENDING_DELETE) {
            ClearSubscription(agent, agent_name);
            std::cout << "Agent " << agent_name << ": Subscription deleted (responseCode=" << response_code << ")" << std::endl;
        }
    } else if (response_code == "negative") {
        std::string err_msg = response.value("message", "unknown error");
        std::cerr << "Agent " << agent_name << ": Subscription response error: " 
                  << err_msg << std::endl;
        if (agent.subscription->status == SubscriptionStatus::PENDING_REQUEST) {
            ClearSubscription(agent, agent_name);
            // Explicit rejection is deterministic: stop auto-retrying
            agent.auto_subscribe_active = false;
            std::cout << "Agent " << agent_name << ": Rolled back pending subscription" << std::endl;
        }
        // If pending delete failed, revert to confirmed (subscription still exists on agent)
        if (agent.subscription.has_value() && 
            agent.subscription->status == SubscriptionStatus::PENDING_DELETE) {
            agent.subscription->status = SubscriptionStatus::CONFIRMED;
            std::cout << "Agent " << agent_name << ": Delete failed, reverted to confirmed" << std::endl;
        }
    }
}

bool E3Manager::ProcessE3Release(const json& release, const std::string& agent_name) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) return false;
    E3AgentState& agent = *it->second;

    uint32_t msg_dapp_id = release.value("dAppIdentifier", 0u);
    if (msg_dapp_id != agent.dapp_id) return false;
    
    std::cout << "Agent " << agent_name << ": Received releaseMessage from Agent" << std::endl;
    {
        std::lock_guard<std::mutex> lock(agent.subscription_mutex);
        ClearSubscription(agent, agent_name);
    }
    {
        std::lock_guard<std::mutex> lock(agent.pub_socket_mutex);
        agent.pub_socket.reset();
    }
    {
        std::lock_guard<std::mutex> lock(agent.req_socket_mutex);
        if (agent.req_socket) {
            agent.req_socket->close();
            agent.req_socket.reset();
        }
    }
    agent.dapp_id = 0;
    agent.state = e3::E3State::DISCONNECTED;
    agent.auto_setup_active = false;
    agent.auto_subscribe_active = false;
    return true;  // Signal caller to break out of subscription loop
}

void E3Manager::ProcessE3Indication(const json& indication, const std::string& agent_name) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) return;
    E3AgentState& agent = *it->second;

    uint32_t msg_dapp_id = indication.value("dAppIdentifier", 0u);
    if (msg_dapp_id != agent.dapp_id) return;
    
    try {
        DAppSubscription sub;
        {
            std::lock_guard<std::mutex> lock(agent.subscription_mutex);
            if (!agent.subscription.has_value()) {
                return; // No active subscription for this agent
            }
            sub = *agent.subscription;  // Safe copy while locked
        }

        if (!indication.contains("protocolData")) {
            std::cerr << "E3 Indication missing 'protocolData'" << std::endl;
            return;
        }
        
        const json& payload = indication["protocolData"];

        if (debug_enabled) {
            std::cout << "\nProcessing E3 Indication from agent " << agent_name << " (dApp " << agent.dapp_id 
                      << ") model '" << sub.model_name << std::endl;
            e3::ProcessIQSampleDebug(payload, ran_shm_ptr_);
            e3::ProcessHEstimatesDebug(payload, ran_shm_ptr_);
            e3::PrintMetadataDebug(payload);
        }

        if (agent.state == e3::E3State::CONNECTED && indication_handler_) {
            e3::IndicationContext ctx{sub.model_name, agent_name, payload,
                                      engine_, ran_shm_ptr_,
                                      results_publisher_.get()};
            indication_handler_(ctx);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error processing E3 Indication: " << e.what() << std::endl;
    }
}

void E3Manager::PublishToAgent(const std::string& agent_name, const std::string& message) {
    if (!running_) return;
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) return;
    E3AgentState& agent = *it->second;

    std::lock_guard<std::mutex> lock(agent.pub_socket_mutex);
    if (!agent.pub_socket) return;

    try {
        agent.pub_socket->send(zmq::buffer(message), zmq::send_flags::dontwait);
    } catch (const zmq::error_t& e) {
        std::cerr << "Error publishing to agent " << agent_name << ": " << e.what() << std::endl;
    }
}

void E3Manager::SendControlMessage(uint16_t sfn, uint16_t slot, const std::string& agent_name) {
    auto it = agents_.find(agent_name);
    if (it == agents_.end()) return;
    E3AgentState& agent = *it->second;
    
    json control_req = CreateE3ControlRequestMessage(sfn, slot, agent.dapp_id);
    PublishToAgent(agent_name, control_req.dump());
}

void E3Manager::ServiceLoop() {
    std::cout << "E3 Connection Manager started for " << agents_.size() << " agent(s)." << std::endl;

    // Single ZMQ context reused across all setup attempts in this loop
    zmq::context_t context(1);
    
    while (running_) {
        // Check shared memory connection (if required)
        if (shm_required_ && ran_shm_fd_ == -1) {
            OpenRanSharedMemory();
        }

        // Retry engine SHM registration until it succeeds (backend may be slow to ready).
        if (engine_ && ran_shm_fd_ != -1 && !engine_shm_registered_) {
            engine_shm_registered_ = engine_->RegisterSharedMemory(shm_key_, ran_shm_size_);
        }
        
        // Check each agent's connection state
        for (auto& [agent_name, agent_state] : agents_) {
            E3AgentState& agent = *agent_state;
            
            // --- Handle Setup for Disconnected Agents ---
            if (agent.auto_setup_active && agent.state != e3::E3State::CONNECTED) {
                agent.auto_subscribe_first_attempt = {};  // fresh readiness window per (re)connect
                std::string setup_error;
                if (!HandleE3Setup(agent_name, context, setup_error)) {
                    std::cout << "Agent " << agent_name << ": " << setup_error << ", will retry..." << std::endl;
                }
            }
            // --- Auto-subscribe once connected ---
            // Defers the first subscribe by one cycle after setup to let the PUB/SUB link settle.
            else if (agent.auto_subscribe_active &&
                     agent.state == e3::E3State::CONNECTED &&
                     agent.pending_request_id == 0 &&
                     !agent.subscription.has_value()) {
                const auto& opts = agent.config.auto_subscription;
                const std::string& model = opts.model.empty() ? model_name_ : opts.model;
                std::string sub_error;
                if (!HandleE3Subscription(opts.telemetry_ids, opts.control_ids, opts.ran_function_id,
                                          opts.periodicity_us, opts.subscription_time_s, model,
                                          agent_name, sub_error)) {
                    // Backend/model may still be loading: retry until the deadline, then give up.
                    auto now = std::chrono::steady_clock::now();
                    if (agent.auto_subscribe_first_attempt.time_since_epoch().count() == 0) {
                        agent.auto_subscribe_first_attempt = now;
                        std::cout << "Agent " << agent_name << ": Auto-subscribe deferred (" << sub_error
                                  << "), retrying" << std::endl;
                    } else if (now - agent.auto_subscribe_first_attempt >
                               std::chrono::seconds(AUTO_SUBSCRIBE_READY_TIMEOUT_S)) {
                        std::cerr << "Agent " << agent_name << ": Auto-subscribe failed (" << sub_error
                                  << "), disabling" << std::endl;
                        agent.auto_subscribe_active = false;
                    }
                }
            }
            
            // --- Stale subscription request cleanup ---
            // If a pending request hasn't received a response within the timeout, roll back.
            if (agent.pending_request_id != 0) {
                auto elapsed = std::chrono::steady_clock::now() - agent.pending_request_time;
                if (elapsed > std::chrono::seconds(subscription_response_timeout_s_)) {
                    std::cerr << "Agent " << agent_name << ": Subscription request " 
                              << agent.pending_request_id << " timed out after "
                              << std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() 
                              << "s" << std::endl;
                    std::lock_guard<std::mutex> lock(agent.subscription_mutex);
                    agent.pending_request_id = 0;
                    if (agent.subscription.has_value()) {
                        if (agent.subscription->status == SubscriptionStatus::PENDING_REQUEST) {
                            ClearSubscription(agent, agent_name);
                            // Give up auto-subscribe after a timeout
                            agent.auto_subscribe_active = false;
                            std::cout << "Agent " << agent_name << ": Rolled back pending subscription" << std::endl;
                        } else if (agent.subscription->status == SubscriptionStatus::PENDING_DELETE) {
                            agent.subscription->status = SubscriptionStatus::CONFIRMED;
                            std::cout << "Agent " << agent_name << ": Delete timed out, reverted to confirmed" << std::endl;
                        }
                    }
                }
            }

            // --- Subscription expiry ---
            {
                std::lock_guard<std::mutex> lock(agent.subscription_mutex);
                if (agent.subscription.has_value() &&
                    agent.subscription->status == SubscriptionStatus::CONFIRMED &&
                    agent.subscription->subscription_time_s > 0) {
                    auto elapsed = std::chrono::steady_clock::now() - agent.subscription->start_time;
                    if (elapsed > std::chrono::seconds(agent.subscription->subscription_time_s)) {
                        std::cout << "Agent " << agent_name << ": Subscription expired after "
                                  << agent.subscription->subscription_time_s << "s" << std::endl;
                        ClearSubscription(agent, agent_name);
                    }
                }
            }
        }
        
        // Wait before next connection check round
        std::this_thread::sleep_for(std::chrono::milliseconds(CONNECTION_CHECK_INTERVAL_MS));
    }
}

// Socket Management Helper Functions
void E3Manager::CreateAgentPubSocket(E3AgentState& agent) {
    if (!agent.zmq_context) return;
    std::lock_guard<std::mutex> lock(agent.pub_socket_mutex);
    agent.pub_socket = std::make_unique<zmq::socket_t>(*agent.zmq_context, zmq::socket_type::pub);
    agent.pub_socket->set(zmq::sockopt::linger, 200);
    agent.pub_socket->set(zmq::sockopt::sndhwm, 1000);
    agent.pub_socket->connect("tcp://" + agent.config.host + ":" + std::to_string(agent.config.agent_sub_port));
}

std::shared_ptr<zmq::socket_t> E3Manager::CreateAgentReqSocket(const E3AgentState& agent) {
    if (!agent.zmq_context) {
        return nullptr;
    }
    
    auto socket = std::make_shared<zmq::socket_t>(*agent.zmq_context, ZMQ_REQ);
    socket->set(zmq::sockopt::rcvtimeo, 2000);
    socket->set(zmq::sockopt::sndtimeo, 2000);
    socket->set(zmq::sockopt::linger, 0);
    
    socket->set(zmq::sockopt::tcp_keepalive, 1);
    socket->set(zmq::sockopt::tcp_keepalive_idle, static_cast<int>(TCP_KEEPALIVE_IDLE_S));
    socket->set(zmq::sockopt::tcp_keepalive_intvl, static_cast<int>(TCP_KEEPALIVE_INTERVAL_S));
    socket->set(zmq::sockopt::tcp_keepalive_cnt, static_cast<int>(TCP_KEEPALIVE_COUNT));
    
    socket->connect("tcp://" + agent.config.host + ":" + std::to_string(agent.config.agent_rep_port));
    return socket;
}

// E3AP Message Helper Functions
uint32_t E3Manager::GenerateMessageId() {
    return message_counter_.fetch_add(1);
}

nlohmann::json E3Manager::CreateE3SubscriptionRequestMessage(const std::vector<uint32_t>& telemetry_ids, const std::vector<uint32_t>& control_ids, uint32_t ran_function_id, uint32_t periodicity_us, uint32_t subscription_time_s) {
    json e3_sub_req;

    e3_sub_req["type"] = "subscriptionRequest";
    e3_sub_req["id"] = GenerateMessageId();
    e3_sub_req["ranFunctionIdentifier"] = ran_function_id;
    e3_sub_req["telemetryIdentifierList"] = telemetry_ids;
    e3_sub_req["controlIdentifierList"] = control_ids;
    e3_sub_req["periodicity"] = periodicity_us;
    e3_sub_req["subscriptionTime"] = subscription_time_s;

    return e3_sub_req;
}

nlohmann::json E3Manager::CreateE3SubscriptionDeleteMessage(uint32_t subscription_id) {
    json e3_del_req;

    e3_del_req["type"] = "subscriptionDelete";
    e3_del_req["id"] = GenerateMessageId();
    // dapp_id is added per-agent in HandleE3SubscriptionDelete
    e3_del_req["subscriptionId"] = subscription_id;

    return e3_del_req;
}

nlohmann::json E3Manager::CreateE3SetupRequestMessage() {
    json e3_setup_request;
    
    e3_setup_request["type"] = "setupRequest";
    e3_setup_request["id"] = GenerateMessageId();
    e3_setup_request["e3apProtocolVersion"] = std::string(e3::E3AP_PROTOCOL_VERSION);
    e3_setup_request["dAppName"] = dapp_name_;
    e3_setup_request["dAppVersion"] = dapp_version_;
    e3_setup_request["vendor"] = vendor_;
    
    return e3_setup_request;
}

bool E3Manager::ParseE3SetupResponseMessage(const std::string& response_str, uint32_t& received_dapp_id) {
    try {
        json response = json::parse(response_str);
        std::cout << "Received E3 Setup Response (responseCode=" << response.value("responseCode", "") << ")" << std::endl;
        if (response.value("type", "") != "setupResponse") {
            std::cerr << "Invalid message type for E3 Setup Response: " << response.value("type", "none") << std::endl;
            return false;
        }
        
        std::string response_code = response.value("responseCode", "");
        if (response_code != "positive") {
            std::cerr << "E3 Setup failed: " << response.value("message", "unknown error") << std::endl;
            return false;
        }
        
        // Extract dApp ID assigned by E3 Agent
        received_dapp_id = response.value("dAppIdentifier", 0u);
        
        // Log available RAN functions
        if (response.contains("ranFunctionList")) {
            std::cout << "Available RAN functions:" << std::endl;
            for (const auto& func : response["ranFunctionList"]) {
                std::cout << "  RAN Function " << func.value("ranFunctionIdentifier", 0u) << ":" << std::endl;
                std::cout << "    telemetryIdentifierList: " << func.value("telemetryIdentifierList", json::array()).dump() << std::endl;
                std::cout << "    controlIdentifierList: " << func.value("controlIdentifierList", json::array()).dump() << std::endl;
                if (func.contains("ranFunctionData")) {
                    std::cout << "    ranFunctionData: " << func["ranFunctionData"].dump() << std::endl;
                }
            }
        }
        
        std::cout << "E3 Setup successful, dAppIdentifier=" << received_dapp_id << std::endl;
        return true;
        
    } catch (const json::parse_error& e) {
        std::cerr << "Failed to parse E3 Setup Response: " << e.what() << std::endl;
        return false;
    } catch (const json::exception& e) {
        std::cerr << "Error processing E3 Setup Response: " << e.what() << std::endl;
        return false;
    }
}

nlohmann::json E3Manager::CreateE3ControlRequestMessage(uint16_t sfn, uint16_t slot, uint32_t dapp_id) {
    json e3_control_req;

    e3_control_req["type"] = "dAppControlAction";
    e3_control_req["id"] = GenerateMessageId();
    e3_control_req["dAppIdentifier"] = dapp_id;
    e3_control_req["ranFunctionIdentifier"] = 2;
    e3_control_req["controlIdentifier"] = 0;

    json control_message;
    control_message["sfn"] = sfn;
    control_message["slot"] = slot;
    e3_control_req["actionData"] = control_message;

    return e3_control_req;
}

void E3Manager::SetIndicationHandler(e3::IndicationHandler handler) {
    indication_handler_ = std::move(handler);
    std::cout << "Indication handler set" << std::endl;
}
