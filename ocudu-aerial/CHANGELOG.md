# Changelog

This file details the notable new features, changes, fixes, and limitations of each release of
the OCUDU Aerial Plugin. Each release lists the platform versions it was validated with. For platform changes, see
the release notes for [Aerial CUDA-Accelerated RAN][acar].

[acar]: https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/release_notes/index.html
[fapi]: https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/cubb/cubb_integration_guide/scf_fapi/standard_support.html

## Release 1.0.0 (November 2026)

Initial release. Validated with:

- Aerial CUDA-Accelerated RAN 26-2
- OCUDU 26.04.0 (`feat/aerial-plugin-rel-26-2`, commit `e99b0d35f4`)

### New Features

- **Split-6 integration.** A compile-time plugin connects OCUDU DU-high to NVIDIA Aerial L1,
  translating between the OCUDU FAPI adaptor interfaces and Aerial SCF FAPI over nvIPC.
  The plugin is linked into the `gnb_split_6` executable. The integration uses Aerial's
  [supported FAPI messages and PDU formats][fapi], within OCUDU's supported feature set.
- **PHY control and data exchange.** FAPI P5 and P7 adaptors handle cell configuration,
  start and stop, downlink and uplink scheduling, transport block exchange, and PHY indications.
- **Deployment examples.** Single-cell and two-cell gNB configurations include n78/n78 and
  n78/n48 examples, with a script to trigger UE handover between the two cells. Docker Compose
  brings up the OCUDU gNB, Aerial L1, and an Open5GS 5G core; an overlay connects the gNB to an
  existing OAI CN5G core instead.
- **Tooling and documentation.** Build and run scripts, development containers with
  host-mounted source and build directories, and guides cover plugin integration, nvIPC setup,
  configuration, and PHY capabilities. The OCUDU dependency uses `feat/aerial-plugin-rel-26-2`,
  which tracks the standalone `aerial_l1_ocudu_plugin` repository's `main` branch.

### Known Issues and Limitations

- **TDD configuration in single-section mode.** The plugin does not translate the OCUDU TDD
  pattern into the `TDD_PERIOD` and `SLOT_CONFIG` TLVs in `CONFIG.request`. With Aerial configured
  for `ru_type: 1` (`SINGLE_SECT_MODE`), the missing TLVs leave Aerial's TDD slot table at its
  initial values, causing fronthaul C-plane sections to fall back to symbol 0 and all 14 symbols.
  Aerial does not automatically select a predefined TDD pattern when these TLVs are absent.
  The fallback matches full 14-symbol DL or UL slots but may exceed the configured DL or UL
  symbol range in special slots.
  This issue does not affect `ru_type: 2` (`MULTI_SECT_MODE`) or `ru_type: 3` (`OTHER_MODE`),
  which use the symbol ranges specified in the FAPI PDUs.
- **Downlink precoding.** The plugin does not register OCUDU precoding matrices with Aerial or
  propagate OCUDU-selected precoding matrix and beam indices. PDSCH PDUs use fixed matrix and
  beam indices of 0.
- **PUSCH CSI Part 2.** CSI Part 2 reporting on PUSCH is not functional. The plugin does not
  send the `UCI_CONFIG` mapping tables in `CONFIG.request` and uses an incorrect CSI Part 2
  enable flag in PUSCH PDUs.
