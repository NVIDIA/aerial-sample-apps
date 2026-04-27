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
│  Shared Memory: IQ samples, H estimates                     │
└─────────────────────────────────────────────────────────────┘
```

**Key components:**

- **E3 Manager** (`common/e3_manager/`): Handles E3AP protocol, multi-agent communication, shared memory, and application dispatch. You don't need to modify this.
- **Inference Engine** (`inference_engine.h`): Optional. Abstract interface for pluggable inference backends. Three implementations are provided: embedded Python (`prb-power-python`), Triton C API (`prb-power-triton`), and Triton gRPC (`prb-power-triton-grpc`). You can use any of them, implement your own, or skip it entirely if your app doesn't need inference.
- **Indication Handler**: Your application logic. A callback invoked for each data indication from an E3 Agent. You receive raw data (JSON payload + shared memory pointer) and can process it however you want.

## Available Data Streams

Data from the E3 Agent arrives in the indication payload JSON. All streams below are uplink data captured from the PUSCH pipeline. Large data (e.g., IQ samples, channel estimates) is delivered via shared memory references; small data (scalars, metadata) is inline.

| Telemetry ID | Field | Transport | Description |
|:---:|-------|-----------|-------------|
| 1 | `iq_samples` | Shared memory | Post-FFT frequency-domain IQ data, pre-equalization (`fh_buffer_index`, `fh_write_index`) |
| 2 | `pdu_data` | Shared memory | PUSCH data (`pusch_buffer_index`, `pusch_write_index`) |
| 3 | `h_estimates` | Shared memory | DMRS-based channel estimates, pre-equalization (`hest_buffer_index`, `hest_write_index`, `hest_data_size`) |
| 4 | `timestamp_ns` | Inline JSON | Agent-side timestamp (nanoseconds) |
| 5 | `sfn` | Inline JSON | System Frame Number |
| 6 | `slot` | Inline JSON | Slot Number |
| 7 | `cell_id` | Inline JSON | Physical Cell ID |
| 8 | `n_rx_ant` | Inline JSON | Number of RX antennas |
| 9 | `n_rx_ant_srs` | Inline JSON | Number of RX antennas for SRS |
| 10 | `n_cells` | Inline JSON | Number of cells |
| 11 | `n_bs_ants` | Inline JSON | Number of base station antennas |
| 12 | `n_layers` | Inline JSON | Number of MIMO layers |
| 13 | `n_subcarriers` | Inline JSON | Number of subcarriers |
| 14 | `n_dmrs_estimates` | Inline JSON | Number of DMRS estimates |
| 15 | `dmrs_symb_pos` | Inline JSON | DMRS symbol positions |
| 16 | `tb_crc_fail` | Inline JSON | Transport block CRC failure indicator |
| 17 | `cb_errors` | Inline JSON | Code block error count |
| 18 | `rsrp` | Inline JSON | Reference Signal Received Power |
| 19 | `cqi` | Inline JSON | Channel Quality Indicator |
| 20 | `cb_count` | Inline JSON | Code block count |
| 21 | `rssi` | Inline JSON | Received Signal Strength Indicator |
| 22 | `qam_mod_order` | Inline JSON | QAM modulation order |
| 23 | `mcs_index` | Inline JSON | Modulation and Coding Scheme index |
| 24 | `mcs_table_index` | Inline JSON | MCS table index |
| 25 | `rb_start` | Inline JSON | Resource block start position |
| 26 | `rb_size` | Inline JSON | Resource block allocation size |
| 27 | `start_symbol_index` | Inline JSON | Start OFDM symbol index |
| 28 | `nr_of_symbols` | Inline JSON | Number of OFDM symbols |

See `data_representation.md` for shared memory layout details and `e3_message_schemas.json` for the full E3AP and E3SM NVIDIA KPM message formats.

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

Your handler has full control over what happens with the data. You can run inference through an engine, process data directly in C++/Python, or anything else.

#### Minimal handler example

Reads inline metadata from the indication payload and logs it (assuming you have subscribed to those telemetry streams):

```cpp
static void ProcessMyApp(const e3::IndicationContext& ctx) {
    int sfn = ctx.payload.value("sfn", 0);
    int slot = ctx.payload.value("slot", 0);
    int mcs = ctx.payload.value("mcs_index", -1);
    LOG_INFO("sfn={} slot={} mcs={}", sfn, slot, mcs);
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
            // Shared memory reference (zero-copy)
            SharedMemoryHeader* hdr = static_cast<SharedMemoryHeader*>(ctx.ran_shm_ptr);
            uint8_t buf_idx = data.value("fh_buffer_index", 0u);
            uint32_t write_idx = data.value("fh_write_index", 0u);

            size_t base = sizeof(SharedMemoryHeader) + buf_idx * hdr->fh_buffer_size;
            size_t row = write_idx * hdr->num_fh_samples * sizeof(int16_t);
            tin.data = e3::ShmInfo{base + row, hdr->num_fh_samples * sizeof(int16_t)};
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
                      config.shm_key, config.shm_required, config.auto_setup);

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
        "enabled": true
      }
    ],
    "default_model": "my_model",
    "auto_setup": true,
    "subscription_response_timeout_s": 10,
    "debug_enabled": false
  }
}
```

**Note:** Agent ports (`agent_rep_port`, `agent_pub_port`, `agent_sub_port`) must match the E3 Agent's port configuration (e.g., `e3_rep_port`, `e3_pub_port`, `e3_sub_port` in the cuphycontroller YAML for Aerial L1).

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

### Extensibility

The data streams listed in the Available Data Streams table above represent the current NVIDIA KPM Service Model (RAN Function ID 2). This set is not fixed:

- **New telemetry streams** can be added to the E3 Agent to expose additional data. New fields appear in the indication JSON payload automatically without changes to the dApp framework. See the E3 Agent source code or open a GitHub issue to request new streams.
- **Multiple E3 Agents**: A single dApp can connect to multiple E3 Agents simultaneously (configured in `e3_config.json` under `e3_agents`). Each agent can expose different data from different network layers, and the dApp receives indications from all of them.
- **Additional Service Models**: The framework is not limited to NVIDIA KPM (RAN Function ID 2). E3 Agents can expose other service models with different RAN Function IDs, providing additional telemetry IDs and/or control IDs, and dApps can subscribe to any of them.

### Debugging and Data Inspection

Set `debug_enabled` to `true` in `e3_config.json` (or pass `--debug` at startup) to print all incoming indication metadata and raw data summaries to the console. This is useful for verifying which telemetry streams are arriving, inspecting payload contents, and understanding the shared memory data layout before writing custom processing logic.

The debug handler (`common/e3_manager/debug_utils.cpp`) includes additional flags for offline analysis of channel estimates:

- `ENABLE_HEST_BINARY_SAVE`: Write raw H-estimate data to binary files
- `ENABLE_HEST_CSV_SAVE`: Write H-estimate data to CSV files

Output is saved to `/tmp/e3_debug/` inside the container. See `data_representation.md` for the shared memory layout and field interpretation.

You can also capture raw E3 messages on the wire using tcpdump on the ZMQ port:

```bash
sudo tcpdump -i lo -A -s 0 'tcp port 5556'
```

## Further Reading

- [e3_message_schemas.json](e3_message_schemas.json): E3AP and E3SM message schemas
- [e3_message_examples.json](e3_message_examples.json): Example messages and client commands
- [data_representation.md](data_representation.md): Shared memory layout for IQ samples and H estimates
- [prb-power-python](../applications/prb-power-python/): Reference implementation (embedded Python)
- [prb-power-triton](../applications/prb-power-triton/): Reference implementation (Triton C API)
- [prb-power-triton-grpc](../applications/prb-power-triton-grpc/): Reference implementation (Triton gRPC)

