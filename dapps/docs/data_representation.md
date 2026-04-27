# Data Representation in E3 Shared Memory

This document describes how IQ samples and channel estimates (H estimates) are organized in POSIX shared memory for zero-copy access between the RAN (E3 Agent) and dApp inference engines.

## Overview

Data are delivered on a **slot basis** per E3 Indication message. Large data streams (IQ samples, H estimates) are stored in shared memory and referenced by buffer/write indices in the indication payload -- the dApp reads the data directly from the mapped region (zero-copy). Small scalar metadata (sfn, slot, timestamp, mcs_index, etc.) is delivered inline as JSON fields within the `protocolData` object.

All shared memory data types use row-major memory layout with ping-pong buffering.

## Shared Memory Layout

The E3 Agent creates a single POSIX shared memory region (default key: `/e3_ran_buffers`) with the following structure:

```
+----------------------------+  offset 0
|   SharedMemoryHeader       |  64 bytes (version, buffer sizes, row counts)
+----------------------------+  sizeof(SharedMemoryHeader)
|   FH Buffer 0 (ping)       |  fh_buffer_size bytes
+----------------------------+
|   FH Buffer 1 (pong)       |  fh_buffer_size bytes
+----------------------------+
|   PUSCH Buffer 0 (ping)    |  pusch_buffer_size bytes
+----------------------------+
|   PUSCH Buffer 1 (pong)    |  pusch_buffer_size bytes
+----------------------------+
|   HEST Buffer 0 (ping)     |  hest_buffer_size bytes
+----------------------------+
|   HEST Buffer 1 (pong)     |  hest_buffer_size bytes
+----------------------------+
```

The header fields (`fh_buffer_size`, `pusch_buffer_size`, `hest_buffer_size`, etc.) are written by the E3 Agent at startup and read by the dApp to compute offsets into each buffer region.

---

## IQ Samples Data Representation

Post-FFT frequency-domain OFDM resource elements received across all base station antennas, organized per slot. These are the uplink received signals after O-RAN fronthaul decompression and FFT processing, prior to channel equalization. Each complex I/Q value represents a single subcarrier on a specific OFDM symbol and antenna.

### Data Format

- **Storage in shared memory**: `int16` (16-bit signed integer, FP16 encoded)
- **Components**: Interleaved I (Real) and Q (Imaginary) values

### Dimensions

```
iq_samples = [N_ANTS][N_SYMBOLS][N_PRBS][N_SC_PER_PRB][2]
```

**Where:**
- `N_ANTS`: Number of receive antennas (typically 4)
- `N_SYMBOLS`: OFDM symbols per slot = 14
- `N_PRBS`: Physical Resource Blocks = 273
- `N_SC_PER_PRB`: Subcarriers per PRB = 12
- `2`: I/Q components (Real, Imaginary)

**Total samples**: 4 * 14 * 273 * 12 * 2 = **366,912 int16 values**
**Total size**: 733,824 bytes (~717 KB per slot)

### Memory Layout (Row-Major)

Data stored as flat `int16` array in this order:

```
[ant_0][sym_0][prb_0][sc_0][I,Q]
[ant_0][sym_0][prb_0][sc_1][I,Q]
...
[ant_0][sym_0][prb_0][sc_11][I,Q]
[ant_0][sym_0][prb_1][sc_0][I,Q]
...
[ant_0][sym_13][prb_272][sc_11][I,Q]
[ant_1][sym_0][prb_0][sc_0][I,Q]
...
[ant_3][sym_13][prb_272][sc_11][I,Q]
```

### Index Formula

```cpp
// For accessing element at [antenna][symbol][prb][subcarrier][iq_component]:
idx = antenna * (N_SYMBOLS * N_PRBS * N_SC_PER_PRB * 2) +
      symbol * (N_PRBS * N_SC_PER_PRB * 2) +
      prb * (N_SC_PER_PRB * 2) +
      subcarrier * 2 +
      iq_component  // 0=I, 1=Q

// Simplified (N_SYMBOLS=14, N_PRBS=273, N_SC_PER_PRB=12):
idx = antenna * 91728 + symbol * 6552 + prb * 24 + subcarrier * 2 + iq_component
```

### Memory Access Pattern

```cpp
// See prb_power_app.cpp, debug_utils.cpp
size_t base_offset = sizeof(SharedMemoryHeader);
size_t single_row_size_bytes = header->num_fh_samples * sizeof(int16_t);
size_t buffer_base_offset = base_offset + (fh_buf_idx * header->fh_buffer_size);
size_t row_offset = fh_write_idx * single_row_size_bytes;
size_t total_offset = buffer_base_offset + row_offset;

const int16_t* iq_data = reinterpret_cast<const int16_t*>(
    static_cast<const uint8_t*>(ran_shm_ptr) + total_offset);
```

---

## H Estimates (Channel Estimates) Data Representation

DMRS-based PUSCH channel estimates computed by cuPHY via LS/MMSE estimation on uplink pilot symbols. Estimates are provided at each DMRS symbol position and interpolated across all allocated subcarriers. Available only when a UE has an active PUSCH allocation. These are pre-equalization estimates representing the frequency response of the wireless channel per antenna and spatial layer.

### Data Format

- **Storage in shared memory**: `float2` (complex 32-bit float) = `std::complex<float>`
- **Components**: Real and Imaginary parts (32-bit float each)

### Dimensions

```
h_estimates = [NH][NF][N_BS_ANTS][N_LAYERS]
```

**Where:**
- `NH`: Number of DMRS estimates = `dmrsAddlnPos + 1` (typically 3)
- `NF`: Number of allocated subcarriers = `12 * allocated_PRBs` (up to 273 PRBs = 3276 subcarriers)
- `N_BS_ANTS`: Number of base station antennas (typically 4)
- `N_LAYERS`: Number of spatial layers (typically 1)

### Memory Layout (Row-Major)

Data stored in shared memory as flat array of `std::complex<float>`:

```
[dmrs_0][sc_0][ant_0][layer_0][Real,Imag]
[dmrs_0][sc_0][ant_0][layer_1][Real,Imag]  // if N_LAYERS > 1
...
[dmrs_0][sc_0][ant_1][layer_0][Real,Imag]
[dmrs_0][sc_0][ant_2][layer_0][Real,Imag]
[dmrs_0][sc_0][ant_3][layer_0][Real,Imag]
[dmrs_0][sc_1][ant_0][layer_0][Real,Imag]
...
[dmrs_0][sc_NF-1][ant_N_BS_ANTS-1][layer_N_LAYERS-1][Real,Imag]
[dmrs_1][sc_0][ant_0][layer_0][Real,Imag]
...
[dmrs_NH-1][sc_NF-1][ant_N_BS_ANTS-1][layer_N_LAYERS-1][Real,Imag]
```

### Index Formula

```cpp
// For accessing element at [dmrs][subcarrier][antenna][layer]:
idx = dmrs * (NF * N_BS_ANTS * N_LAYERS) +
      subcarrier * (N_BS_ANTS * N_LAYERS) +
      antenna * N_LAYERS +
      layer

// Simplified for single layer (N_LAYERS=1, dmrs=0):
idx = antenna + N_BS_ANTS * subcarrier
```

### Memory Access Pattern

```cpp
// See prb_power_app.cpp, debug_utils.cpp
size_t base_offset = sizeof(SharedMemoryHeader);
size_t hest_base_offset = base_offset + (2 * header->fh_buffer_size) + (2 * header->pusch_buffer_size);
size_t hest_buffer_offset = hest_buf_idx * header->hest_buffer_size;
size_t row_offset = hest_write_idx * header->max_hest_samples_per_row * sizeof(std::complex<float>);
size_t final_offset = hest_base_offset + hest_buffer_offset + row_offset;

const std::complex<float>* hest_data = reinterpret_cast<const std::complex<float>*>(
    static_cast<const uint8_t*>(ran_shm_ptr) + final_offset);
```

---

## Key Differences Summary

| Feature | IQ Samples | H Estimates |
|---------|------------|-------------|
| **Storage Type** | `int16` (FP16 encoded) | `std::complex<float>` |
| **Typical Size** | ~717 KB per slot | Varies by allocation |
| **Dimensions** | `[N_ANTS, 14, 273, 12, 2]` | `[NH, NF, N_BS_ANTS, N_LAYERS]` |
| **Update Rate** | Every slot | Per PUSCH allocation |

---

## Code References

### Application Handler (PRB Power)

Processes shared memory data and dispatches to inference engine:
- **Location**: `applications/prb-power-triton/prb_power_app.cpp`
- **Function**: `ProcessPRBPower()`
- **Features**: Zero-copy shared memory access, prepares inputs for inference engine

### Debug Utilities

Diagnostic tool for data validation:
- **Location**: `common/e3_manager/debug_utils.cpp`
- **Functions**: `ProcessIQSampleDebug()`, `ProcessHEstimatesDebug()`
- **Purpose**: Print raw data samples for validation

### Python Model Example

Reference implementation showing data consumption:
- **Location**: `applications/prb-power-triton/models/prb_power_numpy/1/model.py`
- **Config**: `applications/prb-power-triton/models/prb_power_numpy/config.pbtxt`
- **Operation**: Computes average power per PRB from IQ samples

---

## Message Format References

### E3 Indication protocolData Fields

IQ samples (shared memory reference):
```json
"iq_samples": {
  "shm_name": "/e3_ran_buffers",
  "fh_buffer_index": 0,
  "fh_write_index": 42
}
```

H estimates (shared memory reference):
```json
"h_estimates": {
  "shm_name": "/e3_ran_buffers",
  "hest_buffer_index": 1,
  "hest_write_index": 42,
  "hest_data_size": 3072
}
```

**Full schemas**: See `docs/e3_message_schemas.json`
**Examples**: See `docs/e3_message_examples.json`

---

## Integration Notes

### Zero-Copy Design

Large data streams (IQ samples, H estimates) are delivered via shared memory references. The E3 Indication message carries only the buffer index and write index, and the dApp reads the actual data directly from the mapped shared memory region. This enables:
- Minimal latency
- Efficient memory usage
- Direct memory access when the inference engine supports system shared memory (e.g., Triton)

Small scalar metadata (sfn, slot, timestamp, mcs_index, rb_size, etc.) is delivered inline as JSON fields within the `protocolData` object of the indication message.

### Ping-Pong Buffering

All data types use dual buffers (`buffer_index: 0 or 1`). Buffer sizes are configurable based on the amount of data to keep in memory before entries are overwritten (or pushed to a database by the NVIDIA Aerial Data Lake tool, if enabled).

