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
 * E3 Manager — core dApp framework handling E3AP protocol, multi-agent
 * communication, shared memory, and application dispatch.
 */

#ifndef E3_MANAGER_H
#define E3_MANAGER_H

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <functional>
#include <complex>
#include <vector>
#include "inference_engine.h"
#include <nlohmann/json.hpp>
#include <zmq.hpp>

// Shared memory header (200 bytes — must match producer struct in
// aerial_sdk/cuPHY-CP/data_lake/e3_agent.hpp). Field order matches the
// producer exactly (IQ → Hest → RbSNR for metadata). Note that the on-disk
// data layout is a different order: header, 2×FH, 2×PUSCH, 2×Hest,
// 2×SRS-IQ, 2×SRS-RbSNR, 2×SRS-Hest. Use the SRS_* offsets in the consumer
// to compute SRS region bases.
struct SharedMemoryHeader {
    uint32_t version;                    // 0x010100 = v1.1.0
    // PUSCH path
    uint32_t fh_buffer_size;
    uint32_t pusch_buffer_size;
    uint32_t hest_buffer_size;
    uint32_t num_fh_samples;
    uint32_t num_fh_rows;
    uint32_t num_pusch_rows;
    uint32_t num_hest_rows;
    uint32_t max_hest_samples_per_row;
    // SRS path
    uint32_t srs_iq_buffer_size;
    uint32_t num_srs_iq_samples;
    uint32_t num_srs_iq_rows;
    uint32_t srs_hest_buffer_size;
    uint32_t max_srs_hest_bytes_per_row;
    uint32_t num_srs_hest_rows;
    uint32_t srs_rb_snr_buffer_size;
    uint32_t max_srs_rb_snr_bytes_per_row;
    uint32_t num_srs_rb_snr_rows;
    uint32_t reserved[32];
};

static_assert(sizeof(SharedMemoryHeader) == 50 * sizeof(uint32_t),
              "SharedMemoryHeader size changed: bump version and update SHM contract.");

namespace e3 {

constexpr std::string_view E3AP_PROTOCOL_VERSION = "1.0.0";

enum class E3State {
    DISCONNECTED,
    CONNECTED
};

// Application indication handler
struct IndicationContext {
    const std::string& model_name;
    const std::string& agent_name;
    const nlohmann::json& payload;
    InferenceEngine* engine;
    void* ran_shm_ptr;
    zmq::socket_t* results_publisher;
};

using IndicationHandler = std::function<void(const IndicationContext& ctx)>;

// Debug utilities
void ProcessIQSampleDebug(const nlohmann::json& payload, void* ran_shm_ptr);
void ProcessHEstimatesDebug(const nlohmann::json& payload, void* ran_shm_ptr);
void PrintMetadataDebug(const nlohmann::json& payload);

void SaveChannelEstimatesCSV(
    const std::complex<float>* grp_ptr, uint32_t h_size,
    uint8_t n_bs_ants, uint8_t n_layers, uint16_t layer_offset,
    uint16_t n_subcarriers, uint8_t n_dmrs_estimates,
    uint16_t rnti, uint16_t sfn, uint16_t slot, uint64_t timestamp_ns);

void SaveChannelEstimatesBinary(
    const std::complex<float>* grp_ptr, uint32_t h_size,
    uint8_t n_bs_ants, uint8_t n_layers, uint16_t layer_offset,
    uint16_t n_subcarriers, uint8_t n_dmrs_estimates,
    uint16_t rnti, uint16_t ue_grp_idx,
    uint16_t sfn, uint16_t slot, uint64_t timestamp_ns);

} // namespace e3

class E3Manager {
public:
    // Per-agent auto-subscription options (applied on connect when enabled)
    struct AutoSubscription {
        bool enabled = false;
        std::string model;                  // Empty: use manager default model
        std::vector<uint32_t> telemetry_ids;
        std::vector<uint32_t> control_ids;
        uint32_t ran_function_id = 2;
        uint32_t periodicity_us = 100000;
        uint32_t subscription_time_s = 0;   // 0 = indefinite
    };

    // Structure to hold E3 Agent configuration
    struct E3AgentConfig {
        std::string name;         // Agent name (e.g., "NVIDIA_L1", "OAI_L2")
        std::string host;
        uint16_t agent_rep_port;  // Agent REP port: dApp sends setup REQ, Agent replies
        uint16_t agent_pub_port;  // Agent PUB port: Agent publishes indications, dApp subscribes
        uint16_t agent_sub_port;  // Agent SUB port: dApp publishes subscribe/unsubscribe/control/release, Agent subscribes
        bool enabled = true;      // Whether this agent should be connected
        bool auto_setup = true;   // Auto-send E3 Setup on startup / reconnect
        AutoSubscription auto_subscription;  // Auto-subscribe after setup
    };

    // Multi-agent constructor (supports 1 to N agents)
    E3Manager(const std::string& bind_address,
              const std::vector<E3AgentConfig>& agents,
              e3::InferenceEngine* engine,
              const std::string& default_model_name,
              const std::string& dapp_name = "PRB Power",
              const std::string& dapp_version = "1.0.0",
              const std::string& vendor = "NVIDIA",
              uint32_t results_pub_port = 5559,
              uint32_t subscription_response_timeout_s = 10,
              const std::string& shm_key = "/e3_ran_buffers",
              bool shm_required = true);
              
    ~E3Manager();
    
    void Start();
    void Stop();
    bool IsRunning() const { return running_; }
    
    void SetIndicationHandler(e3::IndicationHandler handler);

    // Manager → Agent publisher
    void PublishToAgent(const std::string& agent_name, const std::string& message);
    
    // Debug control
    bool debug_enabled = false;
    
private:
    enum class SubscriptionStatus {
        PENDING_REQUEST,   // Subscribe sent, awaiting confirmation
        CONFIRMED,         // Agent confirmed, subscription active
        PENDING_DELETE,    // Delete sent, awaiting confirmation
        // PENDING_UPDATE  // Reserved for future use
    };
    
    struct DAppSubscription {
        uint32_t subscription_id = 0;           // Assigned by E3 Agent (0 until confirmed)
        SubscriptionStatus status = SubscriptionStatus::PENDING_REQUEST;
        uint32_t ran_function_id = 0;
        std::vector<uint32_t> telemetry_ids;    // Telemetry IDs on the wire
        uint32_t periodicity_us;
        uint32_t subscription_time_s = 0;       // 0 = indefinite
        std::chrono::steady_clock::time_point start_time;
        std::string model_name;
    };
    
    // Per-agent state structure
    struct E3AgentState {
        E3AgentConfig config;
        std::string agent_id;                          // ID received from agent during setup
        e3::E3State state{e3::E3State::DISCONNECTED};
        std::atomic<bool> auto_setup_active{true};      // Runtime gate (disabled on release)
        std::atomic<bool> auto_subscribe_active{false}; // Runtime gate (disabled on reject/timeout/unsubscribe/release)
        std::optional<DAppSubscription> subscription;
        std::thread sub_thread;                        // Subscription thread for this agent
        std::shared_ptr<zmq::context_t> zmq_context;   // ZMQ context for this agent's subscription
        std::shared_ptr<zmq::socket_t> req_socket;     // Persistent REQ socket for requests
        std::unique_ptr<zmq::socket_t> pub_socket;     // PUB socket for commands to this agent
        uint32_t dapp_id = 0;                          // dApp ID assigned by this specific agent
        mutable std::mutex subscription_mutex;         // Mutex for subscription state access (multiple threads)
        mutable std::mutex req_socket_mutex;           // Mutex for REQ socket access
        mutable std::mutex pub_socket_mutex;           // Mutex for PUB socket access
        
        // Async subscription request tracking
        // Design: single pending request per agent (ClientRequestLoop is single-threaded,
        // one subscription per agent enforced). If multiple in-flight requests per agent
        // are needed in the future, replace with std::unordered_map<uint32_t, time_point>.
        uint32_t pending_request_id = 0;
        std::chrono::steady_clock::time_point pending_request_time;

        // Auto-subscribe retry deadline base; epoch = not attempted yet.
        std::chrono::steady_clock::time_point auto_subscribe_first_attempt{};
    };

    // Main loops
    void ClientRequestLoop();
    void E3AgentSubscriptionLoop(const std::string& agent_name);
    void ServiceLoop();
    
    void ClearSubscription(E3AgentState& agent, const std::string& agent_name);

    // E3 Protocol handlers
    bool HandleE3Subscription(const std::vector<uint32_t>& telemetry_ids,
                             const std::vector<uint32_t>& control_ids,
                             uint32_t ran_function_id,
                             uint32_t periodicity_us,
                             uint32_t subscription_time_s,
                             const std::string& model_name, 
                             const std::string& agent_name,
                             std::string& error_message);
    bool HandleE3Setup(const std::string& agent_name, zmq::context_t& ctx, std::string& error_message);
    bool HandleE3SubscriptionDelete(const std::string& agent_name, std::string& error_message);
    void HandleE3Release(const std::string& agent_name);
    void SendE3Release(const std::string& agent_name);
    void ProcessE3SubscriptionResponse(const nlohmann::json& response, const std::string& agent_name);
    bool ProcessE3Release(const nlohmann::json& release, const std::string& agent_name);
    void ProcessE3Indication(const nlohmann::json& indication, const std::string& agent_name);
    void SendControlMessage(uint16_t sfn, uint16_t slot, const std::string& agent_name);
    
    // Socket management helpers
    void CreateAgentPubSocket(E3AgentState& agent);
    std::shared_ptr<zmq::socket_t> CreateAgentReqSocket(const E3AgentState& agent);
    
    // E3AP Message helpers
    uint32_t GenerateMessageId();
    
    nlohmann::json CreateE3SetupRequestMessage();
    nlohmann::json CreateE3SubscriptionRequestMessage(const std::vector<uint32_t>& telemetry_ids, const std::vector<uint32_t>& control_ids, uint32_t ran_function_id, uint32_t periodicity_us, uint32_t subscription_time_s);
    nlohmann::json CreateE3SubscriptionDeleteMessage(uint32_t subscription_id);
    nlohmann::json CreateE3ControlRequestMessage(uint16_t sfn, uint16_t slot, uint32_t dapp_id);
    bool ParseE3SetupResponseMessage(const std::string& response_str, uint32_t& received_dapp_id);
    // Subscription and control responses handled in E3AgentSubscriptionLoop
    
    // Shared memory management
    void OpenRanSharedMemory();
    void CloseRanSharedMemory();
    
    std::string bind_address_;
    std::string model_name_;    // Default model if not specified by a client
    std::string dapp_name_;     // dApp identity for E3 Setup
    std::string dapp_version_;
    std::string vendor_;
    
    // Multi-agent support (map is immutable after construction)
    std::map<std::string, std::unique_ptr<E3AgentState>> agents_;  // Key: agent name
    
    // Message sequence management
    std::atomic<uint32_t> message_counter_{1};
    
    // Threads
    std::thread client_thread_;
    std::thread service_thread_;
    
    // State management
    std::atomic<bool> running_;
    std::atomic<bool> stopped_{false};
    
    // Inference engine (non-owning, app manages lifecycle)
    e3::InferenceEngine* engine_;
    std::unordered_map<std::string, e3::ModelMetadata> model_metadata_cache_;
    std::mutex model_metadata_mutex_;
    
    // Application dispatch
    e3::IndicationHandler indication_handler_;
    
    // Results publisher for external consumers
    std::shared_ptr<zmq::context_t> results_context_;
    std::unique_ptr<zmq::socket_t> results_publisher_;
    
    int ran_shm_fd_ = -1;
    void* ran_shm_ptr_ = nullptr;
    size_t ran_shm_size_ = 0;
    std::string shm_key_ = "/e3_ran_buffers";      // Shared memory key
    bool shm_required_ = true;                     // Whether shared memory is required
    bool engine_shm_registered_ = false;           // Engine SHM registration succeeded (retried until true)
    
    // Connection monitoring configuration
    static constexpr uint32_t TCP_KEEPALIVE_IDLE_S = 5;  // Start probes after N seconds idle
    static constexpr uint32_t TCP_KEEPALIVE_INTERVAL_S = 2;  // Interval between probes
    static constexpr uint32_t TCP_KEEPALIVE_COUNT = 3;  // Number of probes before declaring dead
    static constexpr uint32_t CONNECTION_CHECK_INTERVAL_MS = 1000;  // How often to check disconnected agents
    static constexpr uint32_t AUTO_SUBSCRIBE_READY_TIMEOUT_S = 60;  // Give up auto-subscribe if inference backend/model not ready within this
    uint32_t subscription_response_timeout_s_ = 10;  // Timeout for pending subscription requests
    uint32_t results_pub_port_ = 5559;             // Results publisher port
};

#endif // E3_MANAGER_H
