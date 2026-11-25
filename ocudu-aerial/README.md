# OCUDU–Aerial

Translation libraries between the [OCUDU](https://gitlab.com/ocudu/ocudu) FAPI adaptor interfaces and the [NVIDIA Aerial CUDA-Accelerated RAN](https://github.com/NVIDIA/aerial-cuda-accelerated-ran) FAPI interfaces for split-6 (O-DU high / low) integration. It also carries the Docker deployment for the whole split-6 stack: OCUDU gNB, Aerial L1, and an Open5GS 5GC.

This repository contains only the bridge code and build glue. It does not modify or copy source from either upstream project.

New features, changes, fixes, and limitations of each release are listed in [CHANGELOG.md](CHANGELOG.md).

## Dependencies (submodules)

- **ocudu** (BSD-3-Clause-Open-MPI) – [feat/aerial-plugin-rel-26-2](https://gitlab.com/ocudu/ocudu/-/tree/feat/aerial-plugin-rel-26-2), containing OCUDU MRs 1247, 1471, and 1631. See `ocudu/LICENSE`.
- **OCUDU Aerial L1 plugin** (BSD-3-Clause-Open-MPI) – nested at `ocudu/plugins/aerial` from [aerial_l1_ocudu_plugin](https://gitlab.com/ocudu/work_groups/wg1_hw_accel/aerial_l1_ocudu_plugin) `main`. See `ocudu/plugins/aerial/LICENSE`.
- **NVIDIA Aerial CUDA-Accelerated RAN** (Apache License 2.0) – [aerial-cuda-accelerated-ran](https://github.com/NVIDIA/aerial-cuda-accelerated-ran) tag [`26.2.0`](https://github.com/NVIDIA/aerial-cuda-accelerated-ran/tree/26.2.0). See `aerial-cuda-accelerated-ran/LICENSE`.

See [NOTICE](NOTICE) for attribution details.

## Design (split-6 plugin and nvIPC)

Split-6 PHY integration uses a **compile-time plugin hook**. With `-DENABLE_PLUGINS=ON -DENABLE_AERIAL_PLUGIN=ON`, the nested `plugins/aerial` submodule injects OBJECT files into `ocudu_split6_plugin` and supplies `ocudu::create_split6_plugin()`, so **Aerial is linked into `gnb_split_6`** (no runtime plugin loading or `du_phy_transport` in YAML). A build **without** the Aerial plugin (or with plugins disabled) uses the built-in dummy split-6 plugin.

**nvIPC** is still resolved at runtime for the Aerial adaptor (`libnvipc.so` on `LD_LIBRARY_PATH` / `NVIPC_LIB_PATH` as documented for your environment).

**PHY capabilities** (PUCCH formats, MCS tables, SRS symbols/locations, limits): see [phy_capabilities.md](phy_capabilities.md).

## Layout

```
├── install.sh           # Optional wrapper around the docker compose commands below
├── configs/
│   ├── gnb_split6*.yml             # gNB configs (single cell and two cell)
│   ├── subscriber_db.csv           # default UE DB mounted into the 5GC
│   └── subscriber_db.local.csv     # optional UE DB override (gitignored)
├── scripts/
│   ├── ho_ue.sh              # force-handover a UE between the two cells
│   └── ...                   # dev-container build/run helpers (mounted at /workspace/split6_scripts)
└── docker/
    ├── Dockerfile.split6           # gNB image (build context = repo root)
    ├── Dockerfile.split6.dev       # gNB dev image (ROHC in image; source/build mounted)
    ├── Dockerfile.aerial           # Aerial L1 image (FAPI 10.04 cuphycontroller)
    ├── docker-compose.yml          # production: 5gc/gnb/aerial; dev: 5gc/gnb_dev/aerial_dev
    └── split6.env                  # OPEN5GS_IP/GNB_IP and OPEN5GS_SUBSCRIBER_DB_PATH
ocudu/docker/open5gs/               # 5GC Dockerfile (built as ocudu/open5gs-5gc)
```

The gNB image is built from the **ocudu** submodule. OCUDU provides the split-6 integration hooks and pins the standalone Aerial implementation as its nested `plugins/aerial` submodule. No OCUDU patch apply step is required for that image.

## Prerequisites

- Docker and Docker Compose
- Submodules initialized recursively: `git submodule update --init --recursive`
- An NGC login for the Aerial base image: `docker login nvcr.io`

## Building

Every compose command below runs from the **ocudu-aerial/** directory and needs the same two flags, `--env-file docker/split6.env -f docker/docker-compose.yml`.

```bash
git clone --recurse-submodules https://github.com/NVIDIA/aerial-sample-apps.git
cd aerial-sample-apps/ocudu-aerial
docker compose --env-file docker/split6.env -f docker/docker-compose.yml build 5gc gnb aerial
```

Name `5gc` explicitly: all three images are compiled locally and none of them exist in a public registry.

- **5GC** `ocudu/open5gs-5gc` from `ocudu/docker/open5gs`
- **gNB** `ocudu/gnb` from `docker/Dockerfile.split6`
- **Aerial L1** `aerial-cuda-accelerated-ran:26-2_10.04` from `docker/Dockerfile.aerial`

The only things pulled are the Aerial devel base (`nvcr.io/nvidia/aerial/aerial-cuda-accelerated-ran:26-2-cubb`) and the Ubuntu bases used by the 5GC and gNB builds from Docker Hub.

## Running

```bash
docker compose --env-file docker/split6.env -f docker/docker-compose.yml up -d
docker compose --env-file docker/split6.env -f docker/docker-compose.yml logs -f
docker compose --env-file docker/split6.env -f docker/docker-compose.yml down
```

`./install.sh` is an optional wrapper for those commands (`build` / `up` / `logs` / `down`, prefixed with `dev` for the dev stack).

To run against the OAI CN5G core instead of Open5GS, start that core first, then run:

```bash
docker compose --env-file docker/split6.env -f docker/docker-compose.yml -f docker/docker-compose.oai-cn.yml up -d aerial gnb
```

The gNB then uses `configs/gnb_split6_oai_cn.yaml`.

On a two-cell stack, force a UE onto the other cell from the host with `./scripts/ho_ue.sh <RNTI>` (for example `0x4603`). The script reads the serving PCI from recent `ocudu_gnb` scheduler stdout and writes an `ho` console command into the gNB's stdin (`stdin_open` in compose). Override `GNB_CONTAINER_NAME`, `HO_PCI_A` / `HO_PCI_B`, `HO_PLMN`, and `HO_TAC` if the deployment is not the default PCIs 1 and 2 on PLMN 00101 TAC 7.

### Configuration

IP addresses, config selection, and the subscriber DB are all defined in one place, `docker/split6.env`. `GNB_CONFIG_FILE` (and the optional `GNB_CONFIG_OVERLAY`) are resolved under the mounted `GNB_CONFIG_DIR`. Any of it can be overridden per invocation:

```bash
GNB_CONFIG_DIR=/path/to/config-dir \
OPEN_5GS_ENV_FILE=ocudu/docker/open5gs/open5gs.env \
docker compose --env-file docker/split6.env -f docker/docker-compose.yml up -d
```

`OPEN5GS_SUBSCRIBER_DB_PATH` picks the subscriber DB mounted into the 5GC and defaults to `configs/subscriber_db.csv`, which subscribes UEs to DNN `internet`, SST 1, and SDs `19cde1` and `ffffff`. The CSV accepts optional DNN, access-restriction, SST, and SD columns after the original eight fields. For a local-only UE DB (not needed with ATB SIM cards), copy the default to `configs/subscriber_db.local.csv` and edit it: `install.sh` picks that file up automatically, while plain `docker compose` needs `OPEN5GS_SUBSCRIBER_DB_PATH=../configs/subscriber_db.local.csv` set in `split6.env` or exported.

## Development (host-mounted source/build)

The **dev** services `gnb_dev` and `aerial_dev` recompile without rebuilding images; the gNB runs the dev image with `ocudu` and `ocudu/build` bind-mounted. Both carry `profiles: [dev]`, so name them explicitly to keep the production `gnb` / `aerial` containers from starting as well:

```bash
docker compose --env-file docker/split6.env -f docker/docker-compose.yml build 5gc gnb_dev
docker compose --env-file docker/split6.env -f docker/docker-compose.yml up -d 5gc gnb_dev aerial_dev
```

The dev gNB container runs `sleep infinity`, so the binary is built and started by hand as described below.

### Building nvIPC and the gNB

The gNB dev container needs an nvIPC source tree (headers and `libnvipc.so`) that is ABI-compatible with the Aerial PHY. That tree is produced **inside the Aerial container** (Aerial SDK and toolchain), then the same directory is mounted into the gNB dev container as `/workspace/gt_common_libs` for building nvIPC and the OCUDU binary.

**Step 1 — build the nvIPC package in the Aerial container.** `/opt/nvidia/cuBB` is the mounted Aerial SDK repository.

```bash
docker exec -it ocudu_aerial_dev bash
/nvipc_scripts/copy_nvipc_to_out.sh
exit
```

`copy_nvipc_to_out.sh` fetches the nvIPC source, then copies and extracts it to `/nvipc_out`. On the host that is `nvipc_src/`, mounted in the gNB dev container as `/workspace/gt_common_libs`.

**Step 2 — build nvIPC and the gNB in the gNB dev container.**

```bash
docker exec -it ocudu_gnb_dev bash
DO_INSTALL=1 ./split6_scripts/build_nvipc.sh   # nvIPC under /workspace/gt_common_libs/build
./split6_scripts/build_gnb_split6.sh           # gnb_split_6 (OCUDU source at /workspace)
```

Optional environment variables: for `build_nvipc.sh` — `GT_COMMON_LIBS_DIR`, `NUM_JOBS`, `NVIPC_CUDA_ENABLE` (default `OFF`), `CMAKE_BUILD_TYPE`; for `build_gnb_split6.sh` — `INSTALL_PREFIX`, `DEBUG_BUILD=1`, `NUM_JOBS`, `MARCH`, `COMPILER`.

### SCF 5G FAPI header

`scf_5g_fapi.h` needs to be copied from Aerial cuPHY-CP into ocudu so that the gNB can build. `docker/Dockerfile.split6` does this for the production image.

- **Copy location in ocudu:** `ocudu/lib/fapi_adaptor/phy/p7/scf_5g_fapi/scf_5g_fapi.h`
- **Source (Aerial):** `aerial-cuda-accelerated-ran/cuPHY-CP/scfl2adapter/lib/scf_5g_fapi/scf_5g_fapi.h`

When Aerial updates this header, copy the file again into ocudu to keep the FAPI wire format in sync.

### Building and running Aerial (PHY side)

To build the Aerial PHY (e.g. cuphycontroller) inside the Aerial container:

```bash
./cuPHY-CP/container/run_aerial.sh
./testBenches/phase4_test_scripts/build_aerial_sdk.sh --preset 10_04 -- -DENABLE_CONFORMANCE_TM_PDSCH_PDCCH=OFF
```

Then run the PHY with either of:

```bash
./build.aarch64/cuPHY-CP/cuphycontroller/examples/cuphycontroller_scf F08_CG1
./run_l1.sh ${AERIAL_RUN_L1_ARGS}
```

### Running the gNB with nvIPC

The Aerial plugin loads `libnvipc.so` via `dlopen`. `DO_INSTALL=1` above puts it on `LD_LIBRARY_PATH`; otherwise point `NVIPC_LIB_PATH` at the directory containing it before starting the gNB:

```bash
export NVIPC_LIB_PATH=/workspace/gt_common_libs/build/nvIPC
./split6_scripts/run_gnb_split6.sh
```

`run_gnb_split6.sh` renders `/configs/${GNB_CONFIG_FILE}` to `/tmp/gnb_config.yml` and starts `gnb_split_6 -c /tmp/gnb_config.yml`. You should now see the L1 report the config request received and start sending slot indications.

## Aerial plugin notes

### DU-high PHY cell start timer and stub SLOT.indication

The DU-high uses a **5 second timeout** for the PHY cell start procedure (P5 PARAM / CONFIG / START). The procedure completes when the first SLOT.indication is received from the PHY (Aerial). The split-6 Aerial plugin sends a **stub SLOT.indication** if no SLOT.indication has been received from Aerial within **4 seconds** after sending START.request, so the DU-high can complete the cell start even when Aerial is started after the gNB or is slow to come up. You may see a log line such as "sending stub SLOT.indication (SFN 0.0) after … ms to avoid DU-high timeout" in that case.

## License

NVIDIA-authored files in this repository are Apache License 2.0. See [LICENSE](LICENSE). Submodules and downloaded materials are under their respective licenses. See [NOTICE](NOTICE).

## Contributing

This project is not accepting code contributions at this time. See [CONTRIBUTING.md](CONTRIBUTING.md).
