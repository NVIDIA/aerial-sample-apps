# PRB Power dApp (Python Backend)

Real-time PRB (Physical Resource Block) power calculation from IQ samples using an embedded Python inference engine. The dApp receives RAN data via E3 from an E3 Agent, runs inference through Python models (e.g., NumPy, PyTorch), and optionally publishes results via ZMQ for visualization.

The inference engine embeds a Python interpreter directly in the C++ application via pybind11, providing low-latency in-process inference without external server dependencies.

## Directory Structure

```
.
├── config/e3_config.json          # Runtime configuration
├── prb_power_app.cpp              # Application entry point and handler
├── python_integration/            # Embedded Python inference engine
│   ├── python_engine.h
│   └── python_engine.cpp
├── models/                        # Python inference models
│   ├── prb_power_numpy.py         # NumPy (CPU)
│   ├── prb_power_torch.py         # PyTorch (GPU)
│   ├── prb_power_onnx.py          # ONNX Runtime (GPU)
│   └── prb_power_trt.py           # TensorRT (GPU)
├── visualizer/                    # PRB power web visualizer
├── CMakeLists.txt                 # Build configuration
├── Dockerfile                     # Container definition
├── compose.yml                    # Docker Compose configuration
├── start_services.sh              # Service startup script
└── restart_script.sh              # Container rebuild and restart
```

## Quick Start

### 1. Configure

Edit `config/e3_config.json`:
- Set `deployment.ipc_mode` to match your cuBB container name (e.g., `container:nv-cubb`)
- Set `e3_manager.e3_agents[].host` and ports to match your E3 Agent
- Set `python_engine.model_name` to the desired model (`prb_power_numpy`, `prb_power_torch`, `prb_power_onnx`, or `prb_power_trt`)

### 2. Build and Run

```bash
./restart_script.sh
```

This builds the container and starts:
- E3 Manager with the PRB Power application handler
- Embedded Python inference engine
- dApp client interface on port 5558

If `auto_setup` is enabled in the config (default: `true`), the E3 Manager will automatically connect to all enabled agents and open the shared memory region on startup.

### 3. Subscribe to Data

From another terminal:

```bash
docker exec -it dapp-prb-power-python \
    /opt/src/common/client/e3_e2e_test.py \
    -a NVIDIA_L1 -r 2 -t 1,4,5,6 -p 100000 -d 10
```

This subscribes to IQ samples (1), timestamp (4), SFN (5), and slot (6) from agent `NVIDIA_L1` using RAN Function ID 2 (`-r 2`, NVIDIA KPM), requesting indications every 100ms (`-p 100000`) for 10 seconds (`-d 10`). The model defaults to the one configured in `e3_config.json`.

Run the script with `--help` for full options and telemetry ID reference. See the [Application Development Guide](../../docs/application_development_guide.md) for interactive mode and granular control via `e3_client.py`.

## Configuration

The `config/e3_config.json` file controls all runtime settings:

```json
{
  "deployment": {
    "ipc_mode": "container:nv-cubb",
    "shared_memory": {
      "key": "/e3_ran_buffers",
      "required": true
    }
  },
  "application": {
    "name": "PRB Power Python",
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
    "auto_setup": true,
    "subscription_response_timeout_s": 10,
    "debug_enabled": false
  },
  "python_engine": {
    "module_path": "/opt/src/applications/prb-power-python/models",
    "model_name": "prb_power_numpy"
  }
}
```

**Key settings:**

| Section | Field | Description |
|---------|-------|-------------|
| `deployment` | `ipc_mode` | Docker IPC namespace. Set to `container:<cubb-container-name>` to share memory with cuBB (default: `nv-cubb`). |
| `deployment` | `shared_memory.key` | POSIX shared memory identifier (must match cuBB). |
| `deployment` | `shared_memory.required` | `true`: retry until SHM available. `false`: start without, attempt once. |
| `e3_manager` | `e3_agents[].host` | E3 Agent IP address. Must match the agent's ZMQ bind address. |
| `e3_manager` | `e3_agents[].agent_rep_port` | Agent REP port: dApp sends setup request, Agent replies. |
| `e3_manager` | `e3_agents[].agent_pub_port` | Agent PUB port: Agent publishes indications, dApp subscribes. |
| `e3_manager` | `e3_agents[].agent_sub_port` | Agent SUB port: dApp publishes subscribe/unsubscribe/control/release, Agent subscribes. |
| `e3_manager` | `auto_setup` | Automatically run E3 setup on startup. |
| `e3_manager` | `debug_enabled` | Print all indication metadata and raw IQ/H-estimate debug data. Can also be enabled via `--debug` CLI flag. |
| `application` | `results_pub_port` | ZMQ port for publishing inference results (used by visualizer). |
| `python_engine` | `module_path` | Filesystem path to the directory containing Python model files. |
| `python_engine` | `model_name` | Python module to load (filename without `.py`). |

## Models

### Included Models

| Model | Backend | Device | Description |
|-------|---------|--------|-------------|
| `prb_power_numpy` | NumPy | CPU | PRB power using NumPy |
| `prb_power_torch` | PyTorch | GPU | PRB power using PyTorch with CUDA |
| `prb_power_onnx` | ONNX Runtime | GPU | PRB power using ONNX Runtime with CUDAExecutionProvider (requires `onnxruntime-gpu` wheel, see note below) |
| `prb_power_trt` | TensorRT | GPU | PRB power using TensorRT (auto-builds engine from ONNX) |

All models return:
- `prb_power`: shape (273,), float32, power per PRB
- `timing_us`: shape (2,), float64, `[total_us, compute_us]` inference timing

ONNX and TensorRT models generate their inference artifacts (`.onnx`, `.engine`) on first `initialize()` call if not already present.

**Note:** The `prb_power_onnx` model requires `onnxruntime-gpu`, which as of March 2026 has no pre-built pip wheel for `aarch64`. The wheel must be [built from source](https://onnxruntime.ai/docs/build/eps.html#cuda) inside an NGC container (e.g., `nvcr.io/nvidia/pytorch:26.01-py3`) and installed manually. The `prb_power_trt` model uses TensorRT's Python API directly and does not require ONNX Runtime.

### Switching Models

To switch between models, update `python_engine.model_name` in `config/e3_config.json`:

```json
"python_engine": {
    "module_path": "/opt/src/applications/prb-power-python/models",
    "model_name": "prb_power_torch"
}
```

Rebuild and restart the container for the change to take effect.

### Writing Custom Models

Create a Python file in the `models/` directory implementing the `InferenceModel` class:

```python
import numpy as np

class InferenceModel:
    def initialize(self):
        """Called once at startup."""
        pass

    def get_metadata(self):
        """Return model name, input/output tensor specifications."""
        return {
            "name": "my_model",
            "inputs": [
                {"name": "iq_samples", "type": "FP16", "shape": [4, 14, 273, 12, 2]}
            ],
            "outputs": [
                {"name": "prb_power", "type": "FP32", "shape": [273]},
                {"name": "timing_us", "type": "FP64", "shape": [2]}
            ]
        }

    def infer(self, inputs):
        """Run inference. inputs is a dict of name -> numpy array."""
        iq = inputs["iq_samples"]
        # ... your computation ...
        return {
            "prb_power": result_array,
            "timing_us": np.array([total_us, compute_us], dtype=np.float64)
        }

    def finalize(self):
        """Called once at shutdown."""
        pass
```

Input arrays are zero-copy NumPy views over shared memory. Output arrays are copied back to C++ automatically.

Supported type strings: `FP16`, `FP32`, `FP64`, `INT8`, `INT16`, `INT32`, `INT64`, `UINT8`, `UINT16`, `UINT32`, `UINT64`, `BOOL`.

## Visualizer

Real-time web-based visualization of PRB power distribution and inference timing. Connects to the E3 Manager's ZMQ results publisher to display live PRB power data.

```bash
docker exec -it dapp-prb-power-python bash /opt/src/applications/prb-power-python/visualizer/start_prb_visualizer.sh
```

Open browser to `http://localhost:5001`.

**Options:**
- `--port`: Web server port (default: 5001)
- `--zmq-port`: ZMQ subscriber port (default: 5559)

Example output:

```
ZMQ receiver connected to localhost:5559
Waiting for PRB power data...
[PRB] SFN: 123, Slot: 4, Max Power: 0.542 at PRB 156, Mean: 0.023
[TIMING] Total: 145.3 us
[PRB] SFN: 123, Slot: 5, Max Power: 0.538 at PRB 156, Mean: 0.023
[TIMING] Total: 142.1 us
```

**Features:**
- Bar chart of power distribution across all 273 PRBs
- 2D heatmap grid visualization (21x13)
- Time-series plots of mean/max power and inference latency
- Live statistics: inference time, max/mean PRB power, active PRBs, frames processed

<div align="center">
  <img src="visualizer/prb_power_visualizer_example.png" alt="PRB Visualizer" width="1000"><br>
  <em>Real-time PRB power distribution and inference timing visualization</em>
</div>

---
