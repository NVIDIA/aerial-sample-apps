# Applications

| Application | Engine | Description |
|-------------|--------|-------------|
| [prb-power-python](prb-power-python/) | Embedded Python | Direct Python inference via pybind11, no Triton dependency. Easiest to develop and prototype. |
| [prb-power-triton](prb-power-triton/) | Triton Inference Server (C API) | Lowest latency. In-process Triton via C API, all models loaded at startup. |
| [prb-power-triton-grpc](prb-power-triton-grpc/) | Triton Inference Server (gRPC) | Client-server architecture with runtime model management, HTTP/gRPC endpoints, and Triton tooling. |

All three PRB Power dApps serve as reference implementations. They compute the same metric by consuming a full uplink slot of complex IQ samples, but differ in the inference engine: the embedded Python engine calls Python directly from C++ with no network overhead; while Triton Inference Server provides a managed model server (in-process via C API or client-server via gRPC). Choose based on your deployment needs. See each app's README for full configuration details.

## Inference Performance

Average end-to-end inference engine latency for the PRB Power dApp on an NVIDIA GH200 system with MIG partitioning:

| Model | Backend | Device | Python [μs] | Triton C API [μs] | Triton gRPC [μs] |
|-------|---------|--------|-------------|-------------------|-------------------|
| TRT | TensorRT | GPU | 296 | 167 | 372 |
| ONNX | ONNX Runtime | GPU | 372 | 198 | 390 |
| LibTorch | PyTorch native | GPU | -- | 222 | 420 |
| Torch | Python/PyTorch | GPU | 384 | 447 | 644 |
| NumPy | Python/NumPy | CPU | 1052 | 1197 | 1397 |

See [[1]](https://arxiv.org/pdf/2512.06493) for methodology and full results.
