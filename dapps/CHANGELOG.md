# Changelog

This file details the notable new features, changes, fixes, and limitations of each release of
dapps. Each release lists the platform versions it was validated with. For platform changes, see
the release notes for [Aerial CUDA-Accelerated RAN][acar] and [Aerial Testbed][atb].

[acar]: https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/release_notes/index.html
[atb]: https://docs.nvidia.com/aerial/testbed/latest/text/release_notes.html
[e3sa]: https://github.com/NVIDIA/aerial-cuda-accelerated-ran/tree/main/cuPHY-CP/e3agent-standalone

## Release 1.2.0 (October 2026)

Validated with:

- Aerial CUDA-Accelerated RAN 26-2
- Aerial Testbed 1.2

### New Features

- **Multi-cell indications.** The NVIDIA KPM SM payload carries a `cells[]` array with one entry
  per active cell in the slot, so a dApp sees every cell the E3 Agent serves instead of one.

### Breaking Changes

- **NVIDIA KPM SM restructured around `cells[]`.** Per-cell fields such as `cell_id` and
  `iq_samples`, plus the former `ue_metrics[]`, now `ues[]`, are nested in `cells[]`; only
  `timestamp`, `timestamp_tai`, `sfn`, `slot`, and `n_cells` stay at the root. The shared memory
  layout is unchanged.

### Known Issues and Limitations

- The reference PRB Power dApps read `cells[0]` only; a multi-cell dApp iterates `cells[]`.
- In synth mode, [`e3agent-standalone`][e3sa] from Aerial CUDA-Accelerated RAN 26-2 exits at
  config load when `replay.path` points to a missing file, as in the shipped example config.
  Create the file (for example `touch data/session.trace`) or remove the unused `replay:` block.

## Release 1.1.0 (August 2026)

Validated with:

- Aerial CUDA-Accelerated RAN 26-1.2
- Aerial Testbed 1.1

### New Features

- **Multi-UE indications.** Each indication carries a `ue_metrics[]` array, so a dApp sees every UE
  scheduled in the slot instead of one.
- **PUSCH telemetry.** 15 PUSCH streams are added to the NVIDIA KPM SM for link quality, timing,
  HARQ state, transport block sizing, and per-UE H-estimate locators, plus a TAI-aligned slot
  timestamp.
- **SRS telemetry.** A new SRS path adds 34 streams to the NVIDIA KPM SM covering raw I/Q, per-UE
  channel estimates, per-RB and wideband SNR, time of arrival, and resource configuration. With
  the PUSCH additions this takes the set from 28 to 78 streams.
- **Offline development.** dApps can be built and tested without a live RAN using
  [`e3agent-standalone`][e3sa], which runs the production E3 Agent from synthetic or replayed
  traffic.
- **Lifecycle handling.** The E3 Manager subscribes automatically per agent (`auto_subscribe`),
  re-subscribes after a reconnect, and releases its subscription cleanly on exit.
- **Licensing.** The vendored JSON library is replaced by the `nlohmann-json3-dev` distribution
  package, making the repository purely Apache 2.0.

### Breaking Changes

- **Per-UE fields moved into `ue_metrics[]`.** The 16 fields that were top-level in the NVIDIA KPM
  SM payload, such as `rsrp`, `mcs_index`, and `rb_start`, are now array entries.
- **`cqi` removed.** Link quality is reported by the new `sinr` and `noise_var` streams instead.
- **Shared memory layout v1.1.0.** H-estimate and SRS data are packed contiguously and located via
  byte offsets carried in the indication.

### Bug Fixes and Improvements

- The Triton C API engine releases every inference request, fixing a per-inference leak.
- The E3 Manager initializes the inference engine and registers shared memory before starting its
  threads, fixing a startup race with TensorRT models.
- The E3 Manager tears down its threads and sockets in order, and the reference startup scripts
  always complete their cleanup, fixing hangs and crashes on shutdown.
- The web visualizer sends only new samples and redraws in place, instead of resending the whole
  history on every update.

### Known Issues and Limitations

- The NVIDIA KPM SM supports only a single cell, since the payload carries one `cell_id`. Multi-cell
  support is planned for 1.2.0.

## Release 1.0.0 (April 2026)

Initial release. Validated with:

- Aerial CUDA-Accelerated RAN 26-1
- Aerial Testbed 1.0

### New Features

- **E3 Manager.** The manager implements a pre-standard E3 interface and E3AP over ZMQ: it connects
  to agents, handles subscriptions, attaches to shared memory, and dispatches data into application
  logic, for several agents at once.
- **Pluggable inference engines.** Application logic plugs in behind an `InferenceEngine` interface,
  with three reference PRB Power dApps built on embedded Python, Triton C API, and Triton gRPC.
- **NVIDIA KPM Service Model (SM).** The E3 Agent exposes 28 subscribable streams: PUSCH I/Q
  samples, DMRS channel estimates, and decoded PUSCH payloads in shared memory, plus per-slot
  frame, cell, antenna, channel quality, error, and resource allocation metadata.
- **Tooling and documentation.** A dApp client controls the lifecycle from outside the container, a
  web visualizer plots PRB power live, and the guides, schemas, and examples document the interface.

### Known Issues and Limitations

- The NVIDIA KPM SM exposes uplink streams only; no downlink data is available.
- Shared memory streams, such as I/Q samples and channel estimates, require the dApp container to
  run on the same host as the E3 Agent and share its IPC namespace (`ipc_mode` in `e3_config.json`).
