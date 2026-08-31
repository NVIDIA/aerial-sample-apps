# Application Development Guide

How to build a new dApp application on this E3 framework.

## Architecture

Each application is a self-contained executable that connects to the shared E3 Manager core:

```
┌─────────────────────────────────────────────────────────────┐
│                     Your Application                        │
│                                                             │
│  main() ──► LoadConfig() ──► Create Engine ──► E3Manager    │
│                                 (optional)         │        │
│                                    │               │        │
│                                    │   SetIndicationHandler │
│                                    │         │              │
│                   InferenceEngine  │         │              │
│                   (Triton, Python, │         │              │
│                   TensorRT, ...)   │         │              │
│                                    │         ▼              │
│                                    │   IndicationHandler()  │
│                                    │   ┌─────────────────┐  │
│                                    └──►│ Your processing │  │
│                                        │ logic here      │  │
│                                        └─────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                         │
                  E3 (ZMQ + JSON)
                         │
┌─────────────────────────────────────────────────────────────┐
│  E3 Agent (NVIDIA Aerial cuBB L1, OAI L2, etc.)             │
│  Shared Memory: IQ samples, H estimates, SRS IQ/Hest/RbSNR  │
└─────────────────────────────────────────────────────────────┘
```

**Key components:**

- **E3 Manager** (`common/e3_manager/`): Handles E3AP protocol, multi-agent communication, shared memory, and application dispatch. You don't need to modify this.
- **Inference Engine** (`inference_engine.h`): Optional. Abstract interface for pluggable inference backends. Three implementations are provided: embedded Python (`prb-power-python`), Triton C API (`prb-power-triton`), and Triton gRPC (`prb-power-triton-grpc`). You can use any of them, implement your own, or skip it entirely if your app doesn't need inference.
- **Indication Handler**: Your application logic. A callback invoked for each data indication from an E3 Agent. You receive raw data (JSON payload + shared memory pointer) and can process it however you want.

## Available Data Streams

Data from the E3 Agent arrives in the indication payload JSON. Large data (e.g., IQ samples, channel estimates) is delivered via shared memory references; small data (scalars, metadata) is inline. Cell-level fields (timing, antenna count, shared memory references) are top-level keys in `protocolData`. Per-UE fields (RNTI, MCS, SINR, H-estimate metadata, SRS metrics, etc.) are delivered inside a `ue_metrics[]` array with one entry per scheduled UE.

Indications arrive on two independent paths:
- **PUSCH** — fires once per PUSCH slot
- **SRS** — fires once per SRS slot (sparser; periodicity depends on the gNB cell config)

Some streams are **shared** (delivered on both paths when subscribed); others are exclusive to one path. If a subscription contains only PUSCH-exclusive IDs, only PUSCH indications fire; if it contains only SRS-exclusive IDs, only SRS indications fire. Shared-only subscriptions fire on both paths. See `e3_message_schemas.json` for the per-field path affinity, and the firing matrix at the end of this section.

**Shared streams** (delivered on both PUSCH and SRS indication paths):

| ID | Field | Path | Scope | Transport | Description |
|:--:|-------|:----:|:-----:|-----------|-------------|
| 4 | `timestamp` | Shared | Cell | Inline | Software timestamp (ns) captured when Data Lake processes the slot |
| 5 | `sfn` | Shared | Cell | Inline | System Frame Number |
| 6 | `slot` | Shared | Cell | Inline | Slot Number |
| 7 | `cell_id` | Shared | Cell | Inline | Physical Cell ID |
| 9 | `n_rx_ant_srs` | Shared | Cell | Inline | Number of SRS RX antennas |
| 10 | `n_cells` | Shared | Cell | Inline | Number of cells |
| 33 | `rnti` | Shared | Per-UE | Inline | Radio Network Temporary Identifier |
| 78 | `timestamp_tai` | Shared | Cell | Inline | TAI timestamp (ns) aligned from SFN/slot via grandmaster clock |

**PUSCH streams** (delivered only on the PUSCH indication path):

| ID | Field | Path | Scope | Transport | Description |
|:--:|-------|:---:|:-----:|-----------|-------------|
| 1 | `iq_samples` | PUSCH | Cell | SHM | Post-FFT frequency-domain IQ Samples, before equalization (`fh_buffer_index`, `fh_write_index`) |
| 2 | `pdu_data` | PUSCH | Cell | SHM | PUSCH PDU bytes (`pusch_buffer_index`, `pusch_write_index`) |
| 3 | `h_estimates` | PUSCH | Cell | SHM | PUSCH DMRS channel estimates, before equalization (`hest_buffer_index`, `hest_write_index`, `hest_row_byte_offset`) |
| 8 | `n_rx_ant` | PUSCH | Cell | Inline | Number of RX antennas |
| 11 | `n_bs_ants` | PUSCH | Cell | Inline | Number of base station antennas in H-estimates |
| 12 | `n_layers` | PUSCH | Per-UE | Inline | Spatial layers for this UE |
| 13 | `n_subcarriers` | PUSCH | Per-UE | Inline | Subcarriers in the group allocation |
| 14 | `n_dmrs_estimates` | PUSCH | Per-UE | Inline | DMRS symbol count in the group |
| 15 | `dmrs_symb_pos` | PUSCH | Per-UE | Inline | DMRS symbol position bitmask |
| 16 | `tb_crc_fail` | PUSCH | Per-UE | Inline | Transport block CRC failure indicator |
| 17 | `cb_errors` | PUSCH | Per-UE | Inline | Code block error count |
| 18 | `rsrp` | PUSCH | Per-UE | Inline | Reference Signal Received Power (dB) |
| 19 | `noise_var` | PUSCH | Per-UE | Inline | Noise+interference variance (dB), pre- or post-eq per `enable_pusch_sinr` (pre by default) |
| 20 | `cb_count` | PUSCH | Per-UE | Inline | Code block count |
| 21 | `rssi` | PUSCH | Per-UE | Inline | Received Signal Strength Indicator (dB, per-group) |
| 22 | `qam_mod_order` | PUSCH | Per-UE | Inline | QAM modulation order |
| 23 | `mcs_index` | PUSCH | Per-UE | Inline | MCS index |
| 24 | `mcs_table_index` | PUSCH | Per-UE | Inline | MCS table index |
| 25 | `rb_start` | PUSCH | Per-UE | Inline | Resource block start (per-group) |
| 26 | `rb_size` | PUSCH | Per-UE | Inline | Resource block allocation size (per-group) |
| 27 | `start_symbol_index` | PUSCH | Per-UE | Inline | Start OFDM symbol index (per-group) |
| 28 | `nr_of_symbols` | PUSCH | Per-UE | Inline | PUSCH duration in symbols (per-group) |
| 29 | `tb_size` | PUSCH | Per-UE | Inline | Transport block size (bytes) |
| 30 | `pdu_len` | PUSCH | Per-UE | Inline | PDU length (tb_size on CRC pass, 0 on fail) |
| 31 | `target_code_rate` | PUSCH | Per-UE | Inline | Target code rate x10 |
| 32 | `new_data_indicator` | PUSCH | Per-UE | Inline | New data indicator |
| 34 | `n_ue` | PUSCH | Cell | Inline | Number of PUSCH UEs in this slot |
| 35 | `layer_offset` | PUSCH | Per-UE | Inline | Starting layer in group H matrix (0 for FDM) |
| 36 | `ue_grp_idx` | PUSCH | Per-UE | Inline | cuPHY internal UE group index |
| 37 | `h_offset` | PUSCH | Per-UE | Inline | Element offset (float2) of group H data in SHM row |
| 38 | `h_size` | PUSCH | Per-UE | Inline | Element count (float2) of group H blob |
| 39 | `sinr` | PUSCH | Per-UE | Inline | SINR (dB), pre- or post-eq per `enable_pusch_sinr` (pre by default) |
| 40 | `timing_advance` | PUSCH | Per-UE | Inline | Timing advance estimate |
| 41 | `harq_process_id` | PUSCH | Per-UE | Inline | HARQ process ID (0-15) |
| 42 | `rv_index` | PUSCH | Per-UE | Inline | Redundancy version index (0-3) |
| 43 | `cfo_hz` | PUSCH | Per-UE | Inline | Carrier Frequency Offset in Hz |

**SRS streams** (delivered only on the SRS indication path):

| ID | Field | Path | Scope | Transport | Description |
|:--:|-------|:---:|:-----:|-----------|-------------|
| 44 | `srs_iq_samples` | SRS | Cell | SHM | Raw SRS IQ (`srs_iq_buffer_index`, `srs_iq_write_index`, `srs_iq_row_byte_offset`) |
| 45 | `srs_hest` | SRS | Cell | SHM | SRS H-estimates (`srs_hest_buffer_index`, `srs_hest_write_index`) |
| 46 | `srs_hest_n_prb_grps` | SRS | Per-UE | Inline | PRB groups in SRS H-estimate grid |
| 47 | `srs_hest_offset` | SRS | Per-UE | Inline | Byte offset of UE H-estimate blob in SHM buffer |
| 48 | `srs_hest_size` | SRS | Per-UE | Inline | Byte count of UE H-estimate blob |
| 49 | `srs_rb_snr` | SRS | Cell | SHM | SRS per-PRG SNR in dB (`srs_rb_snr_buffer_index`, `srs_rb_snr_write_index`) |
| 50 | `srs_rb_snr_offset` | SRS | Per-UE | Inline | Byte offset of UE RbSNR blob in SHM buffer |
| 51 | `srs_rb_snr_size` | SRS | Per-UE | Inline | Byte count of UE RbSNR blob |
| 52 | `srs_cell_start_sym` | SRS | Cell | Inline | SRS starting OFDM symbol index |
| 53 | `srs_cell_n_srs_sym` | SRS | Cell | Inline | Number of SRS OFDM symbols |
| 54 | `n_srs_ue` | SRS | Cell | Inline | Number of SRS UEs in this slot |
| 55 | `srs_wideband_snr` | SRS | Per-UE | Inline | Wideband SNR in dB |
| 56 | `srs_signal_energy` | SRS | Per-UE | Inline | Wideband signal energy |
| 57 | `srs_noise_energy` | SRS | Per-UE | Inline | Wideband noise energy (noise floor) |
| 58 | `srs_toa` | SRS | Per-UE | Inline | Timing-of-arrival estimate (microseconds) |
| 59 | `srs_hd_ant_flag` | SRS | Per-UE | Inline | High-density antenna port flag (1 = SNR unreliable) |
| 60 | `srs_sc_corr` | SRS | Per-UE | Inline | Wideband subcarrier correlation [re, im] as array(float32) |
| 61 | `srs_cs_corr_ratio_db` | SRS | Per-UE | Inline | Correlation energy on used vs unused cyclic shifts (dB) |
| 62 | `srs_ant_ports` | SRS | Per-UE | Inline | SRS antenna ports (1, 2, or 4) |
| 63 | `srs_n_syms` | SRS | Per-UE | Inline | Number of SRS symbols (1, 2, or 4) |
| 64 | `srs_n_repetitions` | SRS | Per-UE | Inline | Repetition factor (1, 2, or 4) |
| 65 | `srs_comb_size` | SRS | Per-UE | Inline | Comb size (2 or 4) |
| 66 | `srs_comb_offset` | SRS | Per-UE | Inline | Comb offset (0-3) |
| 67 | `srs_start_sym` | SRS | Per-UE | Inline | UE starting OFDM symbol (0-13) |
| 68 | `srs_cyclic_shift` | SRS | Per-UE | Inline | Cyclic shift (0-11) |
| 69 | `srs_freq_position` | SRS | Per-UE | Inline | Frequency-domain position (0-67) |
| 70 | `srs_freq_shift` | SRS | Per-UE | Inline | Frequency-domain shift (0-268) |
| 71 | `srs_freq_hopping` | SRS | Per-UE | Inline | Frequency hopping b_hop (0-3) |
| 72 | `srs_resource_type` | SRS | Per-UE | Inline | 0=aperiodic, 1=semi-persistent, 2=periodic |
| 73 | `srs_t_srs` | SRS | Per-UE | Inline | Periodicity in slots |
| 74 | `srs_t_offset` | SRS | Per-UE | Inline | Slot offset |
| 75 | `srs_usage` | SRS | Per-UE | Inline | Bitmask: 0x1=beamMgmt, 0x2=codebook, 0x4=nonCodebook, 0x8=antennaSwitching |
| 76 | `srs_n_valid_prg` | SRS | Per-UE | Inline | Number of valid PRB groups (= rb_snr length) |
| 77 | `srs_prg_size` | SRS | Per-UE | Inline | PRBs per PRB-group |

**Indication firing matrix** (which path(s) fire for a given subscription mix):

| Subscription mix | PUSCH fires | SRS fires |
|------------------|:-----------:|:---------:|
| Only shared (e.g. `sfn` + `slot`) | yes | yes |
| Shared + PUSCH-only (e.g. `sfn` + `rsrp`) | yes | no |
| Shared + SRS-only (e.g. `rnti` + `srs_wideband_snr`) | no | yes |
| PUSCH-only + SRS-only | yes | yes |
| Only PUSCH-only | yes | no |
| Only SRS-only | no | yes |

See `data_representation.md` for shared memory layout details (PUSCH and SRS), and `e3_message_schemas.json` for the full E3AP and E3SM NVIDIA KPM message formats.

## Building a New Application

**Quickstart:** Copy a reference app directory (`applications/prb-power-python/`, `applications/prb-power-triton/`, or `applications/prb-power-triton-grpc/`) and modify it.

The steps below walk through each component, both as a guide for building from scratch and as a reference for understanding and modifying the copied app. The E3 Manager (`common/e3_manager/`) and dApp client (`common/client/`) are shared infrastructure, so you can use them as-is. Everything else (handler logic, engine choice, Dockerfile, config) is yours to customize.

### Step 1: Create the Application Directory

```
applications/
└── my-app/
    ├── my_app.cpp               # Entry point + handler
    ├── CMakeLists.txt           # Build config
    ├── config/
    │   └── e3_config.json         # Runtime config
    ├── Dockerfile               # Container definition
    ├── compose.yml              # Docker Compose
    ├── start_services.sh        # Service startup
    └── restart_script.sh        # Build + restart
```

### Step 2: Implement the Indication Handler

The handler is your application's processing function, called each time the E3 Manager receives an indication from an E3 Agent. It receives an IndicationContext struct with the following elements, and you are free to use the ones you need in your application logic:

```cpp
struct IndicationContext {
    const std::string& model_name;      // from dApp client -m flag or e3_config.json default
    const std::string& agent_name;      // name of the E3 Agent that sent this indication
    const nlohmann::json& payload;      // JSON with all subscribed telemetry data
    InferenceEngine* engine;            // inference engine (nullptr if none configured)
    void* ran_shm_ptr;                  // pointer to mapped shared memory region
    zmq::socket_t* results_publisher;   // ZMQ PUB socket for publishing results (null if disabled)
};

using IndicationHandler = std::function<void(const IndicationContext& ctx)>;
```

The `agent_name` field identifies which E3 Agent produced the indication (e.g. `"NVIDIA_L1"`, `"OAI_L2"`). This is essential for multi-agent dApps that receive data from different network layers and need to dispatch to different extraction logic per agent.

PUSCH and SRS indications invoke the same handler; distinguish the path by checking for a field that only one path provides (e.g. `iq_samples` for PUSCH, `srs_iq_samples` for SRS). Subscriptions made only of shared streams, such as `sfn` and `slot`, fire on both paths and give indistinguishable payloads.

Your handler has full control over what happens with the data. You can run inference through an engine, process data directly in C++/Python, or anything else.

#### Minimal handler example

Reads cell-level and per-UE metadata from the indication payload (assuming you have subscribed to those telemetry streams):

```cpp
static void ProcessMyApp(const e3::IndicationContext& ctx) {
    int sfn = ctx.payload.value("sfn", 0);       // cell-level
    int slot = ctx.payload.value("slot", 0);      // cell-level
    for (const auto& ue : ctx.payload["ue_metrics"]) {
        int rnti = ue.value("rnti", 0);           // per-UE
        int mcs = ue.value("mcs_index", -1);      // per-UE
        LOG_INFO("sfn={} slot={} rnti={} mcs={}", sfn, slot, rnti, mcs);
    }
}
```

#### Inference-based handler example (any engine)

Both reference apps (Triton and embedded Python) use a convention where model input names from `GetModelMetadata()` match the E3 data stream field names in the indication payload (e.g., the model declares an input called `iq_samples`, which matches the field name in the JSON payload). The handler iterates model inputs, checks if the corresponding field exists in the payload, and builds the inference inputs automatically. This is a design choice in the reference apps, not a framework constraint. Your handler can access payload fields directly by name without metadata-driven mapping.

```cpp
#include "e3_manager.h"

static void ProcessMyApp(const e3::IndicationContext& ctx) {
    // 1. Get model metadata (cached internally)
    auto metadata = ctx.engine->GetModelMetadata(ctx.model_name);
    if (!metadata) return;

    // 2. Build inputs from the indication payload
    std::vector<e3::TensorInput> inputs;
    for (const auto& model_input : metadata->inputs) {
        if (!ctx.payload.contains(model_input.name)) return;  // missing data

        e3::TensorInput tin;
        tin.name = model_input.name;
        tin.datatype = model_input.type;
        tin.shape = model_input.shape;

        const auto& data = ctx.payload[model_input.name];

        if (data.is_object() && data.contains("fh_buffer_index")) {
            // PUSCH FH IQ shared memory reference (zero-copy)
            SharedMemoryHeader* hdr = static_cast<SharedMemoryHeader*>(ctx.ran_shm_ptr);
            uint8_t buf_idx = data.value("fh_buffer_index", 0u);
            uint32_t write_idx = data.value("fh_write_index", 0u);

            size_t base = sizeof(SharedMemoryHeader) + buf_idx * hdr->fh_buffer_size;
            size_t row = write_idx * hdr->num_fh_samples * sizeof(int16_t);
            tin.data = e3::ShmInfo{base + row, hdr->num_fh_samples * sizeof(int16_t)};
        } else if (data.is_object() && data.contains("srs_iq_buffer_index")) {
            // SRS IQ / SRS Hest / SRS RbSNR follow the same pattern with
            // their own buffer_index/write_index fields and header sizes
            // (srs_iq_buffer_size, srs_hest_buffer_size, srs_rb_snr_buffer_size).
            // See data_representation.md for full offset computation.
        } else {
            // Scalar value (inline JSON)
            // Convert to raw bytes based on model_input.type
        }

        inputs.push_back(tin);
    }

    // 3. Run inference
    std::vector<std::string> output_names;
    for (const auto& o : metadata->outputs) output_names.push_back(o.name);

    auto result = ctx.engine->infer(ctx.model_name, inputs, output_names);

    // 4. Process results and optionally publish via ZMQ
    if (result.success && ctx.results_publisher) {
        // Publish to visualizer, external consumer, etc.
    }
}
```

See `prb-power-python/prb_power_app.cpp` (embedded Python), `prb-power-triton/prb_power_app.cpp` (Triton C API), or `prb-power-triton-grpc/prb_power_app.cpp` (Triton gRPC) for complete references with all shared memory types, scalar handling, and ZMQ result publishing.

### Step 3: Write the Entry Point

Each application owns its `main()`. `LoadConfig()` is defined in the reference apps (not in the shared core), you can adapt it for your config structure. The pattern is:

```cpp
int main(int argc, char* argv[]) {
    // 1. Parse CLI args (--config, --debug)
    std::string config_file = ParseArgs(argc, argv);

    // 2. Load config from JSON
    auto config = LoadConfig(config_file);

    // 3. Create your inference engine
    e3::TritonEngine engine(config.triton_host);  // or PythonEngine, or your own

    // 4. Create E3Manager
    E3Manager manager(config.bind_addr, config.agents, &engine,
                      config.model_name, config.dapp_name,
                      config.dapp_version, config.vendor,
                      pub_port, config.subscription_response_timeout_s,
                      config.shm_key, config.shm_required);

    // 5. Register your handler and start
    manager.SetIndicationHandler(ProcessMyApp);
    manager.Start();

    while (manager.IsRunning())
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
```

### Step 4: Build Configuration

The build system automatically discovers all applications in `applications/`. Copy a reference app's `CMakeLists.txt` (e.g., from `prb-power-triton/`) and adapt it.

If your app requires specific libraries, add a guard at the top so it skips gracefully when dependencies are missing:

```cmake
# Skip if required library not available
if(NOT TRITONSERVER_LIB)
    message(STATUS "Skipping ${CMAKE_CURRENT_SOURCE_DIR}")
    return()
endif()

add_executable(my_app_dapp
    my_app.cpp
)

target_include_directories(my_app_dapp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(my_app_dapp PRIVATE aerial_dapp_core)

set_target_properties(my_app_dapp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
```

### Step 5: Configuration

Create `config/e3_config.json`. The structure follows the same pattern as the PRB Power dApp:

```json
{
  "deployment": {
    "ipc_mode": "container:nv-cubb",
    "shared_memory": { "key": "/e3_ran_buffers", "required": true }
  },
  "application": {
    "name": "My App",
    "version": "1.0.0",
    "vendor": "NVIDIA",
    "results_pub_port": 5559,
    "enable_results_publishing": true
  },
  "e3_manager": {
    "dapp_client_endpoint": "tcp://*:5558",
    "e3_agents": [
      {
        "name": "NVIDIA_L1",
        "host": "127.0.0.1",
        "agent_rep_port": 5555,
        "agent_pub_port": 5556,
        "agent_sub_port": 5557,
        "enabled": true,
        "auto_setup": true,
        "subscription_options": {
          "auto_subscribe": true,
          "telemetry_ids": [1, 4, 5, 6],
          "control_ids": [],
          "ran_function_id": 2,
          "periodicity_us": 100000,
          "subscription_time_s": 0
        }
      }
    ],
    "default_model": "my_model",
    "subscription_response_timeout_s": 10,
    "debug_enabled": false
  }
}
```

**Note:** Agent ports (`agent_rep_port`, `agent_pub_port`, `agent_sub_port`) must match the E3 Agent's port configuration (e.g., `e3_rep_port`, `e3_pub_port`, `e3_sub_port` in the cuphycontroller YAML for Aerial L1).

**Auto setup and auto subscription (per agent):** `auto_setup` (default `true`) makes the E3 Manager send the E3 Setup to that agent on startup and re-send it automatically if the connection drops. `subscription_options.auto_subscribe` (default `false`) extends this to the subscription: once the agent is connected, the manager subscribes with the configured `telemetry_ids` / `control_ids` / `ran_function_id` / `periodicity_us` / `subscription_time_s`, with no external client required. After a RAN restart, the manager re-runs setup and then re-subscribes. `model` is optional; omit it to use `default_model`. If the inference model or backend is not ready yet, the manager retries auto-subscribe for up to 60s before disabling it; an explicit rejection, a response timeout, or an `unsubscribe`/`release` stops it immediately. On shutdown the manager sends an E3 Release to all connected agents. Both flags are per agent, so different agents can use different lifecycle policies (e.g., one auto-driven, one client-driven).

### Step 6: Containerize

Create a `Dockerfile` following either reference implementation:

1. Install dependencies (system packages, Python packages)
2. Copy shared core and your application sources
3. Build with CMake
4. Copy models and runtime files
5. Set environment variables and entrypoint

Create `compose.yml`, `start_services.sh`, and `restart_script.sh` by adapting the PRB Power dApp versions.

#### Docker Base Image Selection

| Base Image | Size | Use Case |
|------------|------|----------|
| `ubuntu:22.04` | ~80 MB | CPU-only inference, minimal footprint |
| `nvcr.io/nvidia/cuda:12.x-runtime-ubuntu22.04` | ~3 GB | GPU inference with custom frameworks |
| `nvcr.io/nvidia/pytorch:25.xx-py3` | ~15 GB | Full GPU stack (PyTorch, ONNX Runtime, TensorRT pre-installed) |
| `nvcr.io/nvidia/tritonserver:25.xx-py3` | ~12 GB | Triton-based dApps with model server |

Match the image to your model requirements. For multi-backend support (NumPy + PyTorch + TensorRT), the NGC PyTorch image avoids building dependencies from source. Consider multi-stage builds to separate build-time from runtime dependencies and reduce image size.

## Implementing a Custom Inference Engine

If you don't use Triton or the embedded Python engine, implement the `e3::InferenceEngine` interface:

```cpp
#include "inference_engine.h"

class MyEngine : public e3::InferenceEngine {
public:
    void Initialize(size_t shm_size) override;
    void Shutdown() override;
    bool RegisterSharedMemory(const std::string& key, size_t size) override;
    std::optional<e3::ModelMetadata> GetModelMetadata(const std::string& model_name) override;
    InferenceResult infer(
        const std::string& model_name,
        const std::vector<e3::TensorInput>& inputs,
        const std::vector<std::string>& output_names) override;
    bool UnloadModel(const std::string& model_name) override;
};
```

Key methods:
- `RegisterSharedMemory`: Called by E3 Manager when shared memory is opened. Register the memory region with your engine for zero-copy access.
- `GetModelMetadata`: Return input/output tensor names, types, and shapes. The handler uses this to map indication data to model inputs.
- `infer`: Execute inference. `TensorInput::data` is either a `std::vector<uint8_t>` (raw data) or `e3::ShmInfo` (shared memory offset + size for zero-copy).

## E3 Protocol Flow

The E3 Manager handles the full protocol lifecycle automatically. Your handler only needs to process indications.

```
E3Manager  →  E3Agent     : setupRequest
E3Agent    →  E3Manager   : setupResponse (dApp ID assigned)
E3Manager  →  E3Agent     : subscriptionRequest
E3Agent    →  E3Manager   : subscriptionResponse (subscription ID)
E3Manager subscribes to ZMQ topic: "{dapp_id}:{subscription_id}"
E3Agent    →  E3Manager   : indicationMessage (repeats at requested periodicity)
                              ↓
                     Your IndicationHandler() is called
                              ↓
E3Manager  →  E3Agent     : dAppControlAction (optional)
E3Manager  →  E3Agent     : unsubscriptionRequest
E3Agent    →  E3Manager   : unsubscriptionResponse
```

## Testing and Client Tools

Two client scripts are provided in `common/client/` for interacting with the E3 Manager:

**`e3_e2e_test.py`** runs the full E3 lifecycle in sequence: list agents, subscribe, wait, status check, unsubscribe. Useful for quick validation that everything works end-to-end.

```bash
docker exec -it <container> \
    /opt/src/common/client/e3_e2e_test.py \
    -a NVIDIA_L1 -m my_model \
    -r 2 -t 1,4,5,6 -p 100000 -d 10
```

Key flags: `-a` agent name, `-m` model, `-r` RAN Function ID (2 = NVIDIA KPM), `-t` comma-separated telemetry IDs, `-p` periodicity in microseconds, `-d` duration in seconds.

Use `-i` (interactive mode) to pause between steps with a press-Enter prompt. In interactive mode, the subscription stays active indefinitely until you press Enter to proceed to the unsubscribe step, useful for long-running tests or live debugging.

**`e3_client.py`** sends individual commands for granular control. Use when you need to run operations independently (e.g., subscribe without auto-unsubscribing, or check status while data is flowing).

```bash
docker exec -it <container> bash
cd /opt/src/common/client

e3_client.py status                                  # Show agent states and subscriptions
e3_client.py setup -a NVIDIA_L1                      # Setup specific agent
e3_client.py subscribe -a NVIDIA_L1 -t 1,4,5,6       # Subscribe with default periodicity
e3_client.py subscribe -a NVIDIA_L1 -t 1,3,5,6 -p 0  # Subscribe every slot
e3_client.py unsubscribe -a NVIDIA_L1                # Delete active subscription
e3_client.py release -a NVIDIA_L1                    # Release agent connection
```

Run either script with `--help` for all options and telemetry ID reference.

## Practical Considerations

### Tuning Indication Periodicity

The `periodicity` parameter in the subscription request controls how often the E3 Agent sends indication messages (in microseconds). Use it to match the rate of incoming data to your dApp's processing capacity.

With a 30 kHz subcarrier spacing, a new slot arrives every 0.5 ms. If your dApp's end-to-end processing (data extraction, inference, result publishing) takes longer than that, indications will queue up and the dApp will fall behind real-time.

The TDD pattern affects the effective data rate. With a typical DDDDDDSUUU pattern, only the 3 uplink slots per half-frame carry data for the dApp. The downlink and special slots in between provide natural gaps for the dApp to catch up on processing, so the effective interval between consecutive indications is often longer than 0.5 ms in practice.

If the natural TDD gaps are not sufficient, set the periodicity to at least your worst-case processing time. For example, if inference takes ~4 ms, request a periodicity of 5000 us (`-p 5000`) instead of receiving one indication every slot. Alternatively, the handler could accumulate multiple indications and batch them before running inference, which can improve throughput for models that benefit from batched input.

SRS indications arrive much less frequently than PUSCH: the period is `T_SRS` slots (per `srs_t_srs` in `ue_metrics[]`, typically 40-160 slots, i.e. 20-80 ms with 30 kHz SCS). A subscription with `-p 0` on SRS streams will still only fire on actual SRS slots, not every slot.

### Extensibility

The data streams listed in the Available Data Streams tables above represent the current NVIDIA KPM Service Model (RAN Function ID 2). This set is not fixed:

- **New telemetry streams** can be added to the E3 Agent to expose additional data. New fields appear in the indication JSON payload automatically without changes to the dApp framework. See the E3 Agent source code or open a GitHub issue to request new streams.
- **Multiple E3 Agents**: A single dApp can connect to multiple E3 Agents simultaneously (configured in `e3_config.json` under `e3_agents`). Each agent can expose different data from different network layers, and the dApp receives indications from all of them.
- **Additional Service Models**: The framework is not limited to NVIDIA KPM (RAN Function ID 2). E3 Agents can expose other service models with different RAN Function IDs, providing additional telemetry IDs and/or control IDs, and dApps can subscribe to any of them.

### Debugging and Data Inspection

Set `debug_enabled` to `true` in `e3_config.json` (or pass `--debug` at startup) to print all incoming indication metadata and raw data summaries to the console. This is useful for verifying which telemetry streams are arriving, inspecting payload contents, and understanding the shared memory data layout before writing custom processing logic.

The debug handler (`common/e3_manager/debug_utils.cpp`) includes additional flags for offline analysis of PUSCH channel estimates:

- `ENABLE_HEST_BINARY_SAVE`: Write raw H-estimate data to binary files
- `ENABLE_HEST_CSV_SAVE`: Write H-estimate data to CSV files

Output is saved to `/tmp/e3_debug/` inside the container. See `data_representation.md` for the shared memory layout and field interpretation.

You can also capture raw E3 messages on the wire using tcpdump on the ZMQ port:

```bash
sudo tcpdump -i lo -A -s 0 'tcp port 5556'
```

## Standalone Development and Testing

The [E3 Agent Standalone](https://github.com/NVIDIA/aerial-cuda-accelerated-ran/tree/main/cuPHY-CP/e3agent-standalone) tool builds and tests dApps without a full RAN. It runs the production E3 Agent and feeds its shared memory from a local source, either a config-driven synthetic generator or a recorded telemetry trace. The dApp sees the exact same subscriptions, indication schema, and shared memory layout as against a live cuBB L1, making it a useful addition to the development workflow:

- **Fast development without a full RAN**: iterate on dApp logic without cuPHY, FAPI, live radio, or a GPU pipeline.
- **Deterministic replay and debugging**: replay a fixed trace, paced by its captured TAI timing, for reproducible runs.
- **Comparable tests across dApps**: drive different dApps from the same trace to compare behavior on identical input.
- **Smooth path to live deployment**: logic developed here runs unchanged against a real L1.

See the [E3 Agent Standalone README](https://github.com/NVIDIA/aerial-cuda-accelerated-ran/tree/main/cuPHY-CP/e3agent-standalone) for setup and usage.

## Further Reading

- [e3_message_schemas.json](e3_message_schemas.json): E3AP and E3SM message schemas
- [e3_message_examples.json](e3_message_examples.json): Example messages and client commands
- [data_representation.md](data_representation.md): Shared memory layout for PUSCH and SRS data streams (e.g., IQ and H estimates)
- [E3 Agent Standalone](https://github.com/NVIDIA/aerial-cuda-accelerated-ran/tree/main/cuPHY-CP/e3agent-standalone): Standalone E3 Agent for dApp development and testing (synthetic generation or trace replay)
- [prb-power-python](../applications/prb-power-python/): Reference implementation (embedded Python)
- [prb-power-triton](../applications/prb-power-triton/): Reference implementation (Triton C API)
- [prb-power-triton-grpc](../applications/prb-power-triton-grpc/): Reference implementation (Triton gRPC)

