# Data Representation in E3 Shared Memory

This document describes how IQ samples and channel estimates (H estimates) are organized in POSIX shared memory for zero-copy access between the RAN (E3 Agent) and dApp inference engines.

## Overview

Data are delivered on a **slot basis** per E3 Indication message. Large data streams (IQ samples, H estimates, SRS IQ, SRS channel estimates, SRS per-RB SNR) are stored in shared memory and referenced by buffer/write indices in the indication payload. The dApp reads the data directly from the mapped region (zero-copy). Small scalar metadata (sfn, slot, timestamp, mcs_index, srs metrics, etc.) is delivered inline as JSON fields within the `protocolData` object.

Indications come on two paths:
- **PUSCH indication**: one per PUSCH slot. Carries FH IQ, PUSCH PDU bytes, PUSCH H-estimates, and the per-UE PUSCH metrics.
- **SRS indication**: one per SRS slot. Carries raw SRS IQ, SRS channel estimates, per-RB SNR, and the per-UE SRS metrics.

Subscriptions that mix the two paths receive separate indications from each. See `e3_message_examples.json` for sample payloads.

All shared memory data types use row-major memory layout with ping-pong buffering.

## Shared Memory Layout

The E3 Agent creates a single POSIX shared memory region (default key: `/e3_ran_buffers`) with the following structure:

```
+--------------------------------+  offset 0
|   SharedMemoryHeader (v1.1.0)  |  200 bytes (version, PUSCH + SRS buffer sizes, row counts)
+--------------------------------+  sizeof(SharedMemoryHeader)
|   FH Buffer 0 (ping)           |  fh_buffer_size bytes
+--------------------------------+
|   FH Buffer 1 (pong)           |  fh_buffer_size bytes
+--------------------------------+
|   PUSCH Buffer 0 (ping)        |  pusch_buffer_size bytes
+--------------------------------+
|   PUSCH Buffer 1 (pong)        |  pusch_buffer_size bytes
+--------------------------------+
|   HEST Buffer 0 (ping)         |  hest_buffer_size bytes
+--------------------------------+
|   HEST Buffer 1 (pong)         |  hest_buffer_size bytes
+--------------------------------+
|   SRS IQ Buffer 0 (ping)       |  srs_iq_buffer_size bytes
+--------------------------------+
|   SRS IQ Buffer 1 (pong)       |  srs_iq_buffer_size bytes
+--------------------------------+
|   SRS RbSNR Buffer 0 (ping)    |  srs_rb_snr_buffer_size bytes
+--------------------------------+
|   SRS RbSNR Buffer 1 (pong)    |  srs_rb_snr_buffer_size bytes
+--------------------------------+
|   SRS Hest Buffer 0 (ping)     |  srs_hest_buffer_size bytes
+--------------------------------+
|   SRS Hest Buffer 1 (pong)     |  srs_hest_buffer_size bytes
+--------------------------------+
```

The header fields (`fh_buffer_size`, `pusch_buffer_size`, `hest_buffer_size`, `srs_iq_buffer_size`, `srs_rb_snr_buffer_size`, `srs_hest_buffer_size`, etc.) are written by the E3 Agent (RAN Node) at startup and read by the dApp to compute offsets into each buffer region.

---

## PUSCH Data Streams

The streams below are delivered on the **PUSCH indication** path. They are collected on the PUSCH worker thread per PUSCH slot and cover the FH (fronthaul) IQ samples plus the per-UE PUSCH H-estimates produced by the cuPHY PUSCH pipeline.

### IQ Samples

Post-FFT frequency-domain OFDM resource elements received across all base station antennas, organized per slot. These are the uplink received signals after O-RAN fronthaul decompression and FFT processing, prior to channel equalization. Each complex I/Q value represents a single subcarrier on a specific OFDM symbol and antenna.

#### Data Format

- **Storage in shared memory**: `int16` (16-bit signed integer, FP16 encoded)
- **Components**: Interleaved I (Real) and Q (Imaginary) values

#### Dimensions

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

#### Memory Layout (Row-Major)

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

#### Index Formula

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

#### Memory Access Pattern

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

### H Estimates (Uplink DMRS Channel Estimates)

Uplink DMRS-based channel estimates in the frequency domain, computed by cuPHY before equalization. Each estimate captures the complex channel response between each base station antenna and each UE spatial layer, sampled at DMRS subcarrier positions across the allocated bandwidth.

By default in the shipped Aerial configs (`pusch_select_chestalgo: 1`) the estimator is two-stage LS + MMSE, and the H Estimates stream carries the MMSE output. Interpolation is done in frequency across all allocated subcarriers (12 per PRB, up to 3276 for 273 PRB/100 MHz) but not in time, giving one estimate per DMRS symbol occasion (the `N_DMRS` axis). See the [cuPHY channel estimation docs](https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/cubb/cuphy_developer_guide/cuphy_components.html#channel-estimation).

In multi-UE slots, cuPHY groups UEs by frequency allocation (FDM) or spatial multiplexing (MU-MIMO). Each group's H-estimate is stored as a contiguous blob in the SHM row, and per-UE metadata in the `ue_metrics[]` JSON array provides everything needed to extract each UE's channel matrix.

#### Data Format

- **Storage in shared memory**: `float2` (complex 32-bit float) = `std::complex<float>`
- **Components**: Real and Imaginary parts (32-bit float each)

#### Per-Group Dimensions

Each group's H-estimate blob has shape:

```
h_group = [N_DMRS][N_SUBCARRIERS][N_BS_ANTS][N_LAYERS_GROUP]
```

**Where:**
- `N_DMRS`: Number of DMRS estimates (from `n_dmrs_estimates` in `ue_metrics[]`, typically 3)
- `N_SUBCARRIERS`: Allocated subcarriers for the group = `12 * rb_size` (up to 273 PRBs = 3276 subcarriers for 100 MHz bandwidth)
- `N_BS_ANTS`: Number of base station antennas (from `n_bs_ants` cell-level field, typically 4)
- `N_LAYERS_GROUP`: Total spatial layers in the group (sum of all UEs' `n_layers` in that group, typically 1)

#### SHM Row Layout

The SHM row for one slot contains all groups' H-estimate blobs concatenated:

```
SHM Row (one per slot):
┌─────────────────────────────────────────────┬─────────────────────────────────────────────┐
│  Group 0 H-est blob                         │  Group 1 H-est blob                         │
│  [N_DMRS][N_SUBCARRIERS_0][N_BS_ANTS][NL_0] │  [N_DMRS][N_SUBCARRIERS_1][N_BS_ANTS][NL_1] │
└─────────────────────────────────────────────┴─────────────────────────────────────────────┘
^                                              ^
h_offset=0                                     h_offset=X
h_size=X                                       h_size=Y
```

**FDM example (2 UEs, separate PRBs):**
```
UE 0 (RNTI 2831):  group 0, RBs 0-4,   h_offset=0,   h_size=720,  layer_offset=0
UE 1 (RNTI 47668): group 1, RBs 5-9,   h_offset=720, h_size=720,  layer_offset=0
```

**MU-MIMO example (2 UEs, shared PRBs, different layers):**
```
UE 0 (RNTI 2831):  group 0, RBs 0-9,   h_offset=0,   h_size=2880, layer_offset=0
UE 1 (RNTI 47668): group 0, RBs 0-9,   h_offset=0,   h_size=2880, layer_offset=1
```

#### Per-UE Metadata Fields

Each element of `ue_metrics[]` in the indication JSON provides:

| Field | Type | Description |
|-------|------|-------------|
| `h_offset` | uint32 | Start element (float2) of this group's blob in SHM row |
| `h_size` | uint32 | Element count (float2) of this group's blob |
| `layer_offset` | uint16 | This UE's starting layer index within the group (0 for FDM) |
| `n_layers` | uint8 | Number of spatial layers for this UE |
| `n_subcarriers` | uint16 | Subcarriers in the group allocation |
| `n_dmrs_estimates` | uint8 | DMRS symbol count in the group |
| `dmrs_symb_pos` | uint16 | DMRS symbol position bitmask |
| `ue_grp_idx` | uint16 | cuPHY internal group index |

#### Memory Layout (Row-Major)

Within each group blob, data is stored as a flat array of `std::complex<float>`:

```
[dmrs_0][sc_0][ant_0][layer_0][Real,Imag]
[dmrs_0][sc_0][ant_0][layer_1][Real,Imag]  // if N_LAYERS_GROUP > 1
...
[dmrs_0][sc_0][ant_1][layer_0][Real,Imag]
[dmrs_0][sc_0][ant_2][layer_0][Real,Imag]
[dmrs_0][sc_0][ant_3][layer_0][Real,Imag]
[dmrs_0][sc_1][ant_0][layer_0][Real,Imag]
...
[dmrs_0][sc_N_SUBCARRIERS-1][ant_N_BS_ANTS-1][layer_N_LAYERS_GROUP-1][Real,Imag]
[dmrs_1][sc_0][ant_0][layer_0][Real,Imag]
...
[dmrs_N_DMRS-1][sc_N_SUBCARRIERS-1][ant_N_BS_ANTS-1][layer_N_LAYERS_GROUP-1][Real,Imag]
```

#### Index Formula

```cpp
// Within a group blob, element at [dmrs][subcarrier][antenna][layer]:
idx = dmrs * (N_SUBCARRIERS * N_BS_ANTS * N_LAYERS_GROUP) +
      subcarrier * (N_BS_ANTS * N_LAYERS_GROUP) +
      antenna * N_LAYERS_GROUP +
      layer
```

#### Memory Access Pattern (C++)

```cpp
// 1. Compute the slot's H-estimate base pointer using hest_row_byte_offset from the indication
SharedMemoryHeader* header = static_cast<SharedMemoryHeader*>(ran_shm_ptr);
size_t base_offset = sizeof(SharedMemoryHeader);
size_t hest_base = base_offset + (2 * header->fh_buffer_size) + (2 * header->pusch_buffer_size);
size_t hest_buf  = hest_buf_idx * header->hest_buffer_size;
size_t row_off   = indication_payload["h_estimates"].value("hest_row_byte_offset", 0u);

auto* row_base = reinterpret_cast<std::complex<float>*>(
    static_cast<uint8_t*>(ran_shm_ptr) + hest_base + hest_buf + row_off);

// 2. Per-UE H-estimate access
uint8_t n_bs_ants = indication_payload.value("n_bs_ants", 4u);
for (const auto& ue : indication_payload["ue_metrics"]) {
    uint32_t h_offset = ue.value("h_offset", 0u);
    uint32_t h_size   = ue.value("h_size", 0u);
    if (h_size == 0) continue;

    // Pointer to this UE's H data + dimensions
    std::complex<float>* h_ptr = row_base + h_offset;
    uint16_t n_sc         = ue.value("n_subcarriers", 0u);
    uint8_t  n_dmrs       = ue.value("n_dmrs_estimates", 0u);
    uint8_t  n_layers_ue  = ue.value("n_layers", 0u);
    uint16_t layer_off    = ue.value("layer_offset", 0u);   // always 0 for FDM
    uint32_t n_layers_grp = h_size / (n_dmrs * n_sc * n_bs_ants);

    // h_ptr -> contiguous block shaped [n_dmrs][n_sc][n_bs_ants][n_layers_grp] (row-major)
    if (n_layers_ue == n_layers_grp) {
        // FDM (common case): entire block is this UE's H — zero-copy
        process(h_ptr, h_size * sizeof(std::complex<float>));
    } else {
        // MU-MIMO (not typical in current Aerial testbed): extract this UE's layers
        std::vector<std::complex<float>> ue_h(n_dmrs * n_sc * n_bs_ants * n_layers_ue);
        for (uint32_t i = 0; i < n_dmrs * n_sc * n_bs_ants; ++i)
            for (uint8_t l = 0; l < n_layers_ue; ++l)
                ue_h[i * n_layers_ue + l] = h_ptr[i * n_layers_grp + layer_off + l];
        process(ue_h.data(), ue_h.size() * sizeof(std::complex<float>));
    }
}
```

#### Per-UE Extraction (Python / ClickHouse)

```python
def get_ue_hest(hest_blob, ue_row, n_bs_ants):
    h_off  = int(ue_row['hOffset'])
    h_size = int(ue_row['hSize'])
    n_sc   = int(ue_row['nSubcarriers'])
    n_dmrs = int(ue_row['nDmrsEstimates'])
    n_layers_grp = h_size // (n_dmrs * n_sc * n_bs_ants)

    # hest_blob is float32 with interleaved real/imag; h_off/h_size are in float2 units
    grp = hest_blob[h_off * 2 : (h_off + h_size) * 2]
    grp = grp.reshape(-1, 2).view(np.complex64).reshape(n_dmrs, n_sc, n_bs_ants, n_layers_grp)

    lo = int(ue_row['layerOffset'])
    return grp[:, :, :, lo : lo + int(ue_row['nrOfLayers'])]
```

For a complete working Python example, see the [`datalake_hest_per_ue.ipynb`](https://github.com/NVIDIA/aerial-cuda-accelerated-ran/blob/main/pyaerial/notebooks/datalake_hest_per_ue.ipynb) notebook in the [NVIDIA Aerial CUDA Accelerated RAN (ACAR)](https://github.com/NVIDIA/aerial-cuda-accelerated-ran) repository under `pyaerial/notebooks/`.

---

## SRS Data Streams

The streams below are delivered on the **SRS indication** path. They are collected on a dedicated SRS worker thread per SRS slot. SRS slots are sparser than PUSCH slots (typical periodicity is once every few slots, configured in the gNB cell config), so this path fires less frequently. Three streams are exposed: the raw post-FFT SRS IQ samples (cell-level), the SRS channel estimates (per-UE), and the SRS per-RB SNR (per-UE).

### SRS IQ Samples

Raw post-FFT frequency-domain samples for the SRS symbols of the uplink slot. One row per cell per SRS slot. Each complex I/Q value is `__half2` (FP16 complex), serialised in shared memory as interleaved `int16` pairs (bit-exact reinterpret of the FP16 bit pattern).

#### Data Format

- **Storage in shared memory**: `int16` (16-bit signed integer, FP16 encoded)
- **Components**: Interleaved I (Real) and Q (Imaginary) values, two `int16` per `__half2` sample
- **Granularity**: one row per cell per SRS slot

#### Dimensions

```
srs_iq = [N_ANTS][N_SRS_SYMBOLS][N_PRBS][N_SC_PER_PRB][2]
```

**Where:**
- `N_ANTS`: Number of receive antennas used for SRS (from `n_rx_ant_srs` at indication level, typically 4)
- `N_SRS_SYMBOLS`: OFDM symbols allocated for SRS per slot = 6 (cuPHY allocates the 3GPP worst-case). This axis is indexed relative to the SRS region, not to the slot: index 0 is the symbol at `srs_cell_start_sym`, and only indices 0 to `srs_cell_n_srs_sym - 1` are populated. The remaining entries are zero / undefined and should be ignored.
- `N_PRBS`: Physical Resource Blocks = 273
- `N_SC_PER_PRB`: Subcarriers per PRB = 12
- `2`: I/Q components (Real, Imaginary)

**Total samples (4 ant)**: 4 * 6 * 273 * 12 * 2 = **157,248 int16 values**
**Total size (4 ant)**: 314,496 bytes (~307 KB per cell per slot)

Note: the buffer is sized for the worst-case maximum (273 PRB, 6 symbols, all SRS antennas), regardless of how much of the slot is actually populated by SRS sequences. dApps should always read the full row stride and clip to the first `srs_cell_n_srs_sym` symbols if only the populated SRS region is of interest. Use `srs_cell_start_sym` to place those symbols within the 14-symbol slot, for example when correlating with PUSCH data.

#### Memory Layout (Row-Major)

Data stored as a flat `int16` array, same antenna-outermost convention as the PUSCH FH IQ stream but with `N_SRS_SYMBOLS = 6` symbols instead of 14 (worst-case allocation; typical 5G TDD configs use 1-4 SRS symbols, read `srs_cell_n_srs_sym` to size the valid region):

```
[ant_0][srs_sym_0][prb_0][sc_0][I,Q]
[ant_0][srs_sym_0][prb_0][sc_1][I,Q]
...
[ant_0][srs_sym_0][prb_272][sc_11][I,Q]
[ant_0][srs_sym_1][prb_0][sc_0][I,Q]
...
[ant_N_ANTS-1][srs_sym_5][prb_272][sc_11][I,Q]
```

#### Index Formula

```cpp
// For accessing element at [antenna][srs_symbol][prb][subcarrier][iq_component]:
idx = antenna * (N_SRS_SYMBOLS * N_PRBS * N_SC_PER_PRB * 2) +
      srs_symbol * (N_PRBS * N_SC_PER_PRB * 2) +
      prb * (N_SC_PER_PRB * 2) +
      subcarrier * 2 +
      iq_component  // 0=I, 1=Q

// Simplified (N_SRS_SYMBOLS=6, N_PRBS=273, N_SC_PER_PRB=12):
idx = antenna * 39312 + srs_symbol * 6552 + prb * 24 + subcarrier * 2 + iq_component
```

#### Memory Access Pattern

```cpp
size_t base_offset = sizeof(SharedMemoryHeader);
size_t srs_iq_base = base_offset
                   + (2 * header->fh_buffer_size)
                   + (2 * header->pusch_buffer_size)
                   + (2 * header->hest_buffer_size);
size_t buf_off     = srs_iq_buf_idx * header->srs_iq_buffer_size;
size_t row_off     = indication_payload["srs_iq_samples"].value("srs_iq_row_byte_offset", 0u);

const int16_t* srs_iq = reinterpret_cast<const int16_t*>(
    static_cast<const uint8_t*>(ran_shm_ptr) + srs_iq_base + buf_off + row_off);
// Reinterpret the int16 bit pattern as half-precision floats:
const __half* srs_iq_fp16 = reinterpret_cast<const __half*>(srs_iq);
```

---

### SRS Channel Estimates

Per-UE SRS channel estimates produced by the cuPHY SRS pipeline. Each estimate captures the complex channel response between each receive antenna and each SRS antenna port, sampled at PRG (Physical Resource Block Group) granularity across the UE's allocated bandwidth. Stored as `int16` complex pairs (`short2`) for compactness and ClickHouse compatibility.

One blob per UE per SRS slot, packed contiguously in the buffer. The actual valid byte count is reported in `srs_hest_size`, and its position by `srs_hest_offset`. Within a slot with multiple UEs, each UE occupies a contiguous region; offsets and sizes are in `ue_metrics[]`.

#### Data Format

- **Storage in shared memory**: `int16` (16-bit signed integer, `short2` complex)
- **Components**: Interleaved I (Real) and Q (Imaginary) values, two `int16` per complex sample
- **Granularity**: one row per UE per SRS slot

#### Per-UE Dimensions

```
srs_hest_ue = [N_ANT_PORTS][N_RX_ANT_SRS][N_PRG]   (column-major in cuPHY)
```

**Where:**
- `N_PRG`: Physical Resource Block Groups for this UE (from `srs_hest_n_prb_grps` in `ue_metrics[]`, ≤ 273)
- `N_RX_ANT_SRS`: Receive antennas used for SRS (from `n_rx_ant_srs` at indication level, typically 4)
- `N_ANT_PORTS`: SRS antenna ports for this UE (from `srs_ant_ports` in `ue_metrics[]`, typically 1 or 2)

**Valid bytes per UE**: `N_PRG * N_RX_ANT_SRS * N_ANT_PORTS * sizeof(short2)` reported in `srs_hest_size`
**Row stride (worst case)**: 273 * 4 * 4 * 4 = 17,472 bytes per UE

#### Per-UE Metadata Fields

Each element of `ue_metrics[]` in the SRS indication JSON provides:

| Field | Type | Description |
|-------|------|-------------|
| `srs_hest_offset` | uint32 | **Byte** offset of this UE's blob within the SHM buffer |
| `srs_hest_size` | uint32 | Valid **byte** count of this UE's blob |
| `srs_n_valid_prg` | uint16 | Number of valid PRGs for this UE |
| `srs_ant_ports` | uint8 | SRS antenna ports for this UE |

The receive-antenna count `n_rx_ant_srs` is published at the indication (cell) level, not per UE.

#### Memory Layout

cuPHY emits the per-UE estimate as a **column-major** tensor with PRG as the fastest-changing axis. When reading into NumPy (row-major by default), reshape with the dimensions reversed and then transpose (or use `order='F'`).

UE blobs are packed contiguously within the buffer. Consumers must use `srs_hest_offset` and `srs_hest_size` to bound reads.

#### Memory Access Pattern (C++)

```cpp
size_t srs_hest_base = sizeof(SharedMemoryHeader)
                     + (2 * header->fh_buffer_size)
                     + (2 * header->pusch_buffer_size)
                     + (2 * header->hest_buffer_size)
                     + (2 * header->srs_iq_buffer_size)
                     + (2 * header->srs_rb_snr_buffer_size);
size_t buf_off = srs_hest_buf_idx * header->srs_hest_buffer_size;

const uint8_t* base = static_cast<const uint8_t*>(ran_shm_ptr)
                    + srs_hest_base + buf_off;

for (const auto& ue : indication_payload["ue_metrics"]) {
    uint32_t off   = ue.value("srs_hest_offset", 0u);   // byte offset into buffer
    uint32_t bytes = ue.value("srs_hest_size",   0u);
    if (bytes == 0) continue;

    // Zero-copy: ue_h points directly into the mapped SHM region.
    // Column-major blob shaped [srs_ant_ports][n_rx_ant_srs][srs_hest_n_prb_grps] of short2.
    const int16_t* ue_h = reinterpret_cast<const int16_t*>(base + off);
    process(ue_h, bytes);
}
```

#### Per-UE Extraction (Python / ClickHouse)

```python
def get_ue_srs_hest(hest_blob_int16, ue_row, n_rx_ant_srs):
    off     = int(ue_row['srsHestOffset']) // 2    # int16 element index
    n_int16 = int(ue_row['srsHestSize'])   // 2    # int16 element count
    n_prg   = int(ue_row['nPrbGrps'])
    n_ports = int(ue_row['srsAntPorts'])

    raw = hest_blob_int16[off : off + n_int16]                  # int16, I/Q interleaved
    cx  = raw.reshape(-1, 2).view(np.dtype([('re','i2'),('im','i2')]))
    # cuPHY column-major [n_ports][n_rx_ant][n_prg] -> NumPy: reshape reversed + transpose
    cx  = cx.reshape(n_prg, n_rx_ant_srs, n_ports).transpose(2, 1, 0)
    return cx   # shape [n_ports, n_rx_ant_srs, n_prg]
```

For a complete working Python example, see the [`datalake_srs_per_ue.ipynb`](https://github.com/NVIDIA/aerial-cuda-accelerated-ran/blob/main/pyaerial/notebooks/datalake_srs_per_ue.ipynb) notebook in the [NVIDIA Aerial CUDA Accelerated RAN (ACAR)](https://github.com/NVIDIA/aerial-cuda-accelerated-ran) repository under `pyaerial/notebooks/`.

---

### SRS Per-RB SNR

Per-UE per-PRG wideband SNR estimate produced by the cuPHY SRS pipeline. One row per UE per SRS slot, stored as flat `float32` values. Useful for tracking per-band channel quality independently of the channel estimate itself.

#### Data Format

- **Storage in shared memory**: `float32` (IEEE 754 single precision)
- **Granularity**: one row per UE per SRS slot

#### Per-UE Dimensions

```
srs_rb_snr_ue = [N_VALID_PRG]
```

**Where:**
- `N_VALID_PRG`: Number of valid PRGs for the UE (from `srs_n_valid_prg` in `ue_metrics[]`, ≤ 273)

UE blobs are packed contiguously within the buffer. Consumers must use `srs_rb_snr_offset` and `srs_rb_snr_size` to bound reads.

#### Per-UE Metadata Fields

Each element of `ue_metrics[]` in the SRS indication JSON provides:

| Field | Type | Description |
|-------|------|-------------|
| `srs_rb_snr_offset` | uint32 | **Byte** offset of this UE's blob within the SHM buffer |
| `srs_rb_snr_size` | uint32 | Valid **byte** count = `srs_n_valid_prg * sizeof(float)` |
| `srs_n_valid_prg` | uint16 | PRG count |

#### Memory Access Pattern (C++)

Same zero-copy pattern as PUSCH H-estimates and SRS H-estimates: the SHM row is the mapped buffer itself, and `ue_snr` is a direct pointer into that region (no copy, no allocation).

```cpp
size_t srs_rb_snr_base = sizeof(SharedMemoryHeader)
                       + (2 * header->fh_buffer_size)
                       + (2 * header->pusch_buffer_size)
                       + (2 * header->hest_buffer_size)
                       + (2 * header->srs_iq_buffer_size);
size_t buf_off = srs_rb_snr_buf_idx * header->srs_rb_snr_buffer_size;

const uint8_t* base = static_cast<const uint8_t*>(ran_shm_ptr)
                    + srs_rb_snr_base + buf_off;

for (const auto& ue : indication_payload["ue_metrics"]) {
    uint32_t off   = ue.value("srs_rb_snr_offset", 0u);   // byte offset into buffer
    uint16_t n_prg = ue.value("srs_n_valid_prg",   0u);
    const float* ue_snr = reinterpret_cast<const float*>(base + off);  // zero-copy
    process(ue_snr, n_prg);
}
```

---

## Key Differences Summary

| Feature | IQ Samples | H Estimates | SRS IQ Samples | SRS H Estimates | SRS Per-RB SNR |
|---------|------------|-------------|----------------|-----------------|----------------|
| **Indication path** | PUSCH | PUSCH | SRS | SRS | SRS |
| **Storage Type** | `int16` (FP16 encoded) | `std::complex<float>` | `int16` (FP16 encoded) | `int16` (`short2` complex) | `float32` |
| **Typical Size (4 ant, max BW)** | ~717 KB per slot | Varies by allocation and number of UE groups | ~307 KB per cell per SRS slot | ~17 KB per UE (worst case) | ~1 KB per UE |
| **Dimensions** | `[N_ANTS, 14, 273, 12, 2]` | Per group: `[N_DMRS, N_SC, N_BS_ANTS, N_LAYERS_GROUP]` | `[N_ANTS, 6, 273, 12, 2]` | Per UE: `[N_ANT_PORTS, N_RX_ANT_SRS, N_PRG]` (column-major) | Per UE: `[N_VALID_PRG]` |
| **Layout** | Single contiguous block | Concatenated per-group blobs; per-UE slicing via `ue_metrics[]` | One block per cell | Contiguous per-UE blobs via `srs_hest_offset`/`srs_hest_size` | Contiguous per-UE blobs via `srs_rb_snr_offset`/`srs_rb_snr_size` |
| **Update Rate** | Every PUSCH slot | Per PUSCH allocation | Every SRS slot (per cell) | Per SRS allocation (per UE) | Per SRS allocation (per UE) |

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

### SRS Decode Notebook

Reference implementation for decoding SRS streams from ClickHouse rows:
- **Location**: [`pyaerial/notebooks/datalake_srs_per_ue.ipynb`](https://github.com/NVIDIA/aerial-cuda-accelerated-ran/blob/main/pyaerial/notebooks/datalake_srs_per_ue.ipynb) in the [NVIDIA Aerial CUDA Accelerated RAN (ACAR)](https://github.com/NVIDIA/aerial-cuda-accelerated-ran) repository
- **Coverage**: per-UE wideband SNR / TOA distribution timelines, per-PRG wideband SNR heatmap, SRS H-estimate magnitude across PRG, SRS IQ constellation per antenna

---

## Message Format References

### E3 Indication protocolData Fields (PUSCH path)

IQ samples (shared memory reference):
```json
"iq_samples": {
  "shm_name": "/e3_ran_buffers",
  "fh_buffer_index": 0,
  "fh_write_index": 42
}
```

H estimates (shared memory reference + per-UE metadata):
```json
"h_estimates": {
  "shm_name": "/e3_ran_buffers",
  "hest_buffer_index": 1,
  "hest_write_index": 42,
  "hest_row_byte_offset": 156672
},
"ue_metrics": [
  { "rnti": 2831,  "h_offset": 0,   "h_size": 720, "layer_offset": 0,
    "n_layers": 1,  "n_subcarriers": 60, "n_dmrs_estimates": 3, "ue_grp_idx": 0 },
  { "rnti": 47668, "h_offset": 720, "h_size": 720, "layer_offset": 0,
    "n_layers": 1,  "n_subcarriers": 60, "n_dmrs_estimates": 3, "ue_grp_idx": 1 }
]
```

### E3 Indication protocolData Fields (SRS path)

SRS IQ samples (cell-level, shared memory reference):
```json
"srs_iq_samples": {
  "shm_name": "/e3_ran_buffers",
  "srs_iq_buffer_index": 0,
  "srs_iq_write_index": 7,
  "srs_iq_row_byte_offset": 943488
},
"n_rx_ant_srs": 4,
"srs_cell_start_sym": 13,
"srs_cell_n_srs_sym": 1
```

SRS channel estimates + per-RB SNR (shared memory references + per-UE metadata):
```json
"srs_hest": {
  "shm_name": "/e3_ran_buffers",
  "srs_hest_buffer_index": 1,
  "srs_hest_write_index": 7
},
"srs_rb_snr": {
  "shm_name": "/e3_ran_buffers",
  "srs_rb_snr_buffer_index": 1,
  "srs_rb_snr_write_index": 7
},
"ue_metrics": [
  { "rnti": 2831,  "srs_hest_offset": 0,     "srs_hest_size": 8736, "srs_rb_snr_offset": 0,    "srs_rb_snr_size": 1092,
    "srs_hest_n_prb_grps": 273, "srs_ant_ports": 2, "srs_wideband_snr": 19.2 },
  { "rnti": 47668, "srs_hest_offset": 8736, "srs_hest_size": 4368, "srs_rb_snr_offset": 1092, "srs_rb_snr_size": 1092,
    "srs_hest_n_prb_grps": 273, "srs_ant_ports": 1, "srs_wideband_snr": 21.7 }
]
```

**Full schemas**: See `docs/e3_message_schemas.json`
**Examples**: See `docs/e3_message_examples.json`

---

## Integration Notes

### Zero-Copy Design

Large data streams (IQ samples, H estimates, SRS IQ, SRS H estimates, SRS per-RB SNR) are delivered via shared memory references. The E3 Indication message carries small per-stream metadata (buffer index, write index, and where applicable a slot-level row byte offset and per-UE element offsets into the row) needed to locate each slot's data within the mapped shared memory region. This enables:
- Minimal latency
- Efficient memory usage
- Direct memory access when the inference engine supports system shared memory (e.g., Triton)

Small scalar metadata (sfn, slot, timestamp, mcs_index, rb_size, wideband_snr_db, etc.) is delivered inline as JSON fields within the `protocolData` object of the indication message.

### Ping-Pong Buffering

All data types use dual buffers (`buffer_index: 0 or 1`). Buffer sizes are configurable based on the amount of data to keep in memory before entries are overwritten (or pushed to a database by the NVIDIA Aerial Data Lake tool, if enabled). PUSCH and SRS buffers advance independently and each notify path manages its own `buffer_index`/`write_index` pair.

