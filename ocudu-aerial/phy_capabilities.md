# PHY capabilities reference (split-6 / OCUDU + Aerial)

This document consolidates supported PHY capabilities relevant to split-6 deployments (OCUDU DU + Aerial PHY), including PUCCH formats, MCS tables and indices, SRS symbols and locations, and related limits. Values are derived from the OCUDU codebase and CHANGELOG; the Aerial PHY may impose additional or different limits (see [NVIDIA Aerial documentation](https://docs.nvidia.com/aerial/)).

## OCUDU version and release cadence

- **Current version (reference):** Check the **ocudu** submodule for the active build; e.g. `ocudu/cmake/modules/version.cmake` defines `OCUDU_VERSION_MAJOR.MINOR.PATCH` (as of this doc, the in-repo default is **25.10.0**).
- **Release naming:** OCUDU uses **YY.MM** for releases: **25.04** = April 2025, **25.10** = October 2025, **26.04** = April 2026, **26.10** = October 2026, etc. A third digit (e.g. 24.10.1) denotes a patch/hotfix.
- **Where to look:** `ocudu/CHANGELOG` lists features per release; `git -C ocudu describe --tags` or the version in the built binary gives the exact revision in use.

This document describes capabilities for **OCUDU 25.04 and later**; references to earlier releases (23.x, 24.x) are omitted.

---

## OCUDU PHY Features

The following are **OCUDU** supported features or exclusions.
This document assumes a real PHY is connected; integration with a specific PHY (e.g. Aerial) and its limits are covered separately.

| Feature / limit | OCUDU Status | Notes |
|----------------|----------------|--------|
| **PRACH formats** | Supported | Long: 0, 1, 2, 3. Short: A1, A2, A3, B1, B4, C0, C2, A1/B1, A2/B2, A3/B3 (TS 38.211 Tables 6.3.3.1-1, 6.3.3.1-2). |
| **PRACH restricted set** | Unrestricted only | Type A and Type B restricted sets not implemented in PHY generator. |
| **PUCCH resource sets** | Only set index 0 and 1 | No other PUCCH resource set indices. |
| **UCI on PUSCH** | Supported | HARQ-ACK, SR, CSI multiplexed on PUSCH in scheduler. |
| **PUSCH layers** | Supported | Up to 4; default max rank 1. Configurable via BWP / UE capability; codebook path requires SRS. |
| **UL MIMO (rank > 1)** | Requires SRS; max rank ≤ SRS ports | Codebook-based UL MIMO needs SRS; max rank cannot exceed number of SRS ports (1, 2, or 4). |
| **SRS (Sounding Reference Signals)** | Supported since 25.04 | Single-layer closed-loop UL-MIMO using SRS. |
| **PDSCH layers** | Supported since 23.10 | Up to 4 layers (downlink MIMO). |
| **CSI-RS** | Supported since 23.5 | Downlink; multiplexing with PDSCH configurable. |
| **HARQ retransmissions** | Same MCS as initial tx | Retransmissions reuse same MCS (and TBS/PRBs); MCS 31 is not used for retx. |
| **MCS 29, 30, 31** | Reserved | 3GPP tables; max data-carrying MCS is 28 (64-QAM) or 27 (256-QAM / UL with transform precoding). |

---

## 1. PUCCH formats

### 1.1 Format combinations (cell-level)

A cell uses **one** of the following PUCCH format combinations (TS 38.213 §9.2, TS 38.331 PUCCH-Config):

| Combination        | Resource set 0 / SR | Resource set 1 / CSI |
|-------------------|---------------------|------------------------|
| `f0_and_f2`       | Format 0            | Format 2               |
| `f1_and_f2`       | Format 1            | Format 2               |
| `f1_and_f3`       | Format 1            | Format 3               |
| `f1_and_f4`       | Format 1            | Format 4               |

- **Resource set 0**: HARQ-ACK / SR (short formats). The format (F0 or F1) is **cell-wide**: the cell uses one combination (e.g. `f0_and_f2` or `f1_and_f2`), so all UEs in the cell use the same format for set 0.
- **Resource set 1**: Larger UCI payloads (e.g. CSI). The format (F2, F3, or F4) is also **cell-wide**: the combination fixes one of F2, F3, or F4 for the cell, so all UEs use the same set-1 format.
- Formats 3 and 4 since 25.04.

*Source: `ocudu/include/ocudu/ran/pucch/pucch_mapping.h` (`pucch_formats`, `pucch_f0f1_format`, `pucch_f2f3f4_format`).*

### 1.2 Per-format limits (OCUDU constants)

| Format | Symbols (range)     | PRBs (max) | HARQ-ACK bits (F0) | UCI bits (min) | Notes |
|--------|---------------------|------------|---------------------|----------------|-------|
| **0**  | 1–2                 | 1         | 0–2                 | —              | Short; cyclic shift 0–11 |
| **1**  | 1–7 (data symbols)  | 1         | —                   | —              | OCC 0–7, cyclic shift 0–11 |
| **2**  | 1–2                 | 1–16      | —                   | ≥ 3            | 8 data SC per RB (TS 38.213 §9.1.5.2) |
| **3**  | 4–14                | 1–16      | —                   | ≥ 3            | Max code rate 0.80 |
| **4**  | 4–14                | 1         | —                   | ≥ 3            | Max code rate 0.80 |

- **PUCCH resource sets**: Only resource set index **0** and **1** supported.
- **Common PUCCH resources**: Up to 16 per cell (`r_PUCCH` 0–15). When using F0, `pucch_resource_common` ∈ {0, 1, 2}; when using F1, ∈ {3, …, 15}.
- **Dedicated PUCCH resources**: Up to 256 per cell (implementation-defined).
- **Max UCI payload** (formats 2/3/4): 1706 bits.

*Source: `ocudu/include/ocudu/ran/pucch/pucch_constants.h`, `serving_cell_config_validator.cpp`.*

---

## 2. SRS (Sounding Reference Signals)

SRS is supported with single-layer closed-loop UL-MIMO (since 25.04).

### 2.1 Symbol and location

- **Number of symbols per resource** \(N^{SRS}_{symb}\): **1, 2, or 4** (`srs_nof_symbols`: n1, n2, n4).
- **Start position**: Index from **0 to 5** (counted from the **last** symbol of the slot: 0 = last symbol, 1 = second last, etc.), as per TS 38.211 §6.4.1.4.1.
- **Repetition factor** \(R\): 1, 2, or 4; must satisfy \(R \le N^{SRS}_{symb}\) (TS 38.211 §6.4.1.4.3).
- SRS occupies the **last part of the slot** (UL symbols); exact symbol indices are derived from `start_pos` and `nof_symb` in the scheduler (e.g. `ofdm_symbol_range` in `srs_info`).

*Source: `ocudu/include/ocudu/ran/srs/srs_configuration.h`, `ocudu/include/ocudu/scheduler/result/srs_info.h`, `ocudu/include/ocudu/ran/srs/srs_resource_configuration.h`.*

### 2.2 Periodicity and offset

Supported **SRS periodicity** (in slots), TS 38.331 SRS-PeriodicityAndOffset:

1, 2, 4, 5, 8, 10, 16, 20, 32, 40, 64, 80, 160, 320, 640, 1280, 2560.

**Offset** \(t_{offset}\): 0 to period − 1 (slots). For TDD, offset must fall in an UL slot.

*Source: `ocudu/include/ocudu/ran/srs/srs_configuration.h` (`srs_periodicity`).*

### 2.3 Other SRS parameters (OCUDU)

- **Resource type**: aperiodic, semi-persistent, periodic.
- **Transmission comb**: \(K_{TC}\) = 2 or 4 (`tx_comb_size`).
- **Comb offset**: 0–1 (comb 2) or 0–3 (comb 4).
- **Cyclic shift**: 0–7 (comb 2) or 0–11 (comb 4).
- **Configuration index** \(c_{SRS}\): 0–63.
- **Bandwidth index** \(b_{SRS}\): 0–3.
- **Sequence ID**: 0–1023.
- **Freq. domain position**: 0–67; **freq. domain shift**: 0–268.
- **Resource sets**: up to 16; **resources per set**: up to 16; **resources per cell**: up to 64.

*Source: `ocudu/include/ocudu/ran/srs/srs_configuration.h`, `srs_info.h`.*

### 2.4 Aerial / SCF FAPI (SRS)

- **MAX_SRS_SYMBOLS**: 4 (in SCF 5G FAPI header).
- SRS indication types: beam management, codebook, non-codebook, antenna switching.
- For Aerial-specific SRS limits and behaviour, see NVIDIA Aerial documentation and `aerial-cuda-accelerated-ran/cuPHY-CP/scfl2adapter/lib/scf_5g_fapi/scf_5g_fapi.h`.

---

## 3. MCS index and tables

### 3.1 MCS index range

- **MCS index** \(I_{MCS}\): **0–31** (TS 38.214 §5.1.3.1 DL, §6.1.4.1 UL).
- Type: `sch_mcs_index` (bounded 0–31).

*Source: `ocudu/include/ocudu/ran/sch/sch_mcs.h`.*

### 3.2 PDSCH MCS tables

| Table        | Identifier   | 3GPP reference              | Typical use      |
|-------------|--------------|-----------------------------|------------------|
| Table 1     | `qam64`      | TS 38.214 Table 5.1.3.1-1   | 64-QAM default   |
| Table 2     | `qam256`     | TS 38.214 Table 5.1.3.1-2   | 256-QAM          |
| Table 3     | `qam64LowSe` | TS 38.214 Table 5.1.3.1-3   | Low spectral eff.|

*Source: `ocudu/include/ocudu/ran/pdsch/pdsch_mcs.h`.*

### 3.3 PUSCH MCS tables

Same three table identifiers as PDSCH:

| Table        | Identifier   | 3GPP reference              |
|-------------|--------------|-----------------------------|
| Table 1     | `qam64`      | TS 38.214 Table 6.1.4.1-1   |
| Table 2     | `qam256`     | TS 38.214 Table 6.1.4.1-2   |
| Table 3     | `qam64LowSe` | TS 38.214 Table 6.1.4.1-3   |

PUSCH MCS resolution also depends on **transform precoding** and **tp-pi2BPSK** (TS 38.331 PUSCH-Config).

*Source: `ocudu/include/ocudu/ran/pusch/pusch_mcs.h`.*

### 3.4 Max MCS index by table (data-carrying)

Not all indices 0–31 carry data; 29, 30, 31 are **reserved** in 3GPP tables (target code rate 0). Effective max for scheduling:

| Context | Max MCS | Notes |
|--------|---------|--------|
| **PDSCH 64-QAM** (qam64) | **28** | Last data-carrying index in table 5.1.3.1-1. |
| **PDSCH 256-QAM** (qam256) | **27** | Index 28 is reserved (0 code rate). |
| **PUSCH 64-QAM**, no transform precoding | **28** | Same as DL 64-QAM. |
| **PUSCH 256-QAM** or **transform precoding enabled** | **27** | `get_max_mcs_ul()` caps at 27 in these cases. |

Config validation enforces: for PDSCH/PUSCH, `max_ue_mcs` ≤ 28 for qam64 and ≤ 27 for qam256 (and for PUSCH with transform precoding). **MCS 31 is not used for retransmissions** in this codebase; HARQ retransmissions keep the **same MCS** (and same TBS/PRBs) as the original transmission.

*Source: `ocudu/lib/scheduler/support/mcs_calculator.h` (`get_max_mcs_ul`), `ocudu/apps/.../du_high_config_validator.cpp`, `ocudu/lib/scheduler/ue_scheduling/ue_fallback_scheduler.cpp` (retx reuses same MCS), MCS table tests (29/30/31 → 0 code rate).*

### 3.5 Where to configure max MCS

| What | Where to configure |
|------|---------------------|
| **DL UE MCS range** | `cell_cfg.pdsch.max_ue_mcs` / `min_ue_mcs` (YAML) or CLI `cell_cfg pdsch --max_ue_mcs`, `--min_ue_mcs`. Maps to scheduler expert `ue.dl_mcs` (interval). |
| **UL UE MCS range** | `cell_cfg.pusch.max_ue_mcs` / `min_ue_mcs` (YAML) or CLI `cell_cfg pusch --max_ue_mcs`, `--min_ue_mcs`. Maps to scheduler expert `ue.ul_mcs`. |
| **RAR / SIB1 / Msg4** | `cell_cfg.pdsch.fixed_rar_mcs`, `fixed_sib1_mcs`; expert `ra.rar_mcs_index`, `ra.msg3_mcs_index`; `ue.max_msg4_mcs`. |

Defaults (du_high_config): `min_ue_mcs = 0`, `max_ue_mcs = 28` (PDSCH and PUSCH). Expert defaults: `dl_mcs` and `ul_mcs` interval `{0, 28}`.

*Source: `ocudu/apps/units/flexible_o_du/o_du_high/du_high/du_high_config.h`, `du_high_config_cli11_schema.cpp`, `du_high_config_translators.cpp`, `ocudu/include/ocudu/scheduler/config/scheduler_expert_config.h`.*

### 3.6 Configuration defaults

- **PDSCH**: default MCS table in BWP builder is often `qam256`; cell/scheduler may use `qam64` or `qam64LowSe`.
- **PUSCH**: default `qam64`.

---

## 4. FAPI / PARAM TLVs (reference)

Capability reporting between MAC and PHY uses FAPI P5 PARAM.response. The OCUDU side uses a simplified param response (e.g. error code only); the **Aerial SCF 5G FAPI** defines PARAM TLV IDs that describe PHY capabilities, including:

- **PRACH**: CONFIG TLVs for PRACH (e.g. occasion params); Aerial requires PRACH config for `cuphyValidatePrachParams`.
- **PUCCH**: `PARAM_TLVS_PUCCH_FORMATS`, `PARAM_TLVS_MAX_PUCCH_PER_SLOT`
- **PUSCH**: UCI mux, DMRS, mapping type, modulation order, etc.
- **SRS**: usage, symbol bitmaps, comb size, cyclic shifts, etc.
- **PDSCH**: mapping type, allocation types, VRB-to-PRB, DMRS, MCS/modulation order, etc.

For the exact TLV set and values reported by the Aerial PHY, see the Aerial SDK and `aerial-cuda-accelerated-ran/cuPHY-CP/scfl2adapter/lib/scf_5g_fapi/scf_5g_fapi.h` (e.g. `PARAM_TLVS_*`, `CONFIG_TLV_*`).

---

## 5. Code and config references

| Topic           | OCUDU headers / sources |
|----------------|--------------------------|
| PRACH formats / config | `ocudu/include/ocudu/ran/prach/prach_format_type.h`, `prach_configuration.h`, `restricted_set_config.h` |
| PUCCH formats  | `ocudu/include/ocudu/ran/pucch/pucch_mapping.h`, `pucch_constants.h` |
| PUCCH config   | `ocudu/include/ocudu/ran/pucch/pucch_configuration.h` |
| PUCCH validation | `ocudu/lib/scheduler/config/serving_cell_config_validator.cpp` |
| MCS            | `ocudu/include/ocudu/ran/sch/sch_mcs.h`, `ran/pdsch/pdsch_mcs.h`, `ran/pusch/pusch_mcs.h` |
| SRS            | `ocudu/include/ocudu/ran/srs/srs_configuration.h`, `scheduler/result/srs_info.h` |
| FAPI param     | `ocudu/include/ocudu/fapi/p5/param_request_tlvs.h` |
| Aerial SCF FAPI | `aerial-cuda-accelerated-ran/cuPHY-CP/scfl2adapter/lib/scf_5g_fapi/scf_5g_fapi.h` |

- **Config options**: `ocudu/configs/readme.md` and `./gnb cell_cfg -h` (and subcommands e.g. `pucch`, `pdsch`, `pusch`).
- **Changelog**: `ocudu/CHANGELOG`.

---

## 6. External documentation

- **OCUDU config reference**: [docs.srsran.com – config ref](https://docs.srsran.com/projects/project/en/latest/user_manuals/source/config_ref.html)
- **NVIDIA Aerial**: [docs.nvidia.com/aerial](https://docs.nvidia.com/aerial/)
- **3GPP**: TS 38.211 (physical channels), TS 38.213 (physical layer procedures), TS 38.214 (physical layer data), TS 38.331 (RRC).
