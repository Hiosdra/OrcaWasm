# orca-wasm — OrcaSlicer WebAssembly engine

Standalone Emscripten build of [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer)
v2.4.2. This repository contains the C++ bridge, compatibility patches,
toolchain configuration, and release workflow for the host-ready engine.

## Repository layout

```text
orca/                 OrcaSlicer source checkout (submodule or shallow clone)
bridge/               exported C API and host boundary
cmake/                dependency discovery and WASM configuration
overrides/            source overrides for unavailable desktop libraries
patches/              idempotent OrcaSlicer compatibility patcher
wasm/                 Emscripten target and compatibility shims
scripts/              local build, smoke-test, and comparison tools
.github/workflows/    reproducible CI build and release workflow
```

## Artifacts

| File | Description |
|------|-------------|
| `slicer.js` / `slicer.wasm` | Single-threaded compatibility engine |
| `slicer-mt.js` / `slicer-mt.wasm` | Multithreaded engine for COOP/COEP hosts |

The build does not produce `slicer.data`: the headless engine uses its virtual
filesystem only for input and output files.

## Local build

Install Emscripten 3.1.74 and the system tools used by CI (CMake, Ninja,
Python 3, `m4`, `texinfo`, OpenSSL, `ccache`, and a C/C++ toolchain). Then run:

```bash
WASM_VARIANT=st ./scripts/build-local-wsl.sh
WASM_VARIANT=mt ./scripts/build-local-wsl.sh
```

The script builds the pinned OrcaSlicer dependencies, applies `patches/apply.py`,
and writes the selected pair to `artifacts/`. Use `EMSDK=/path/to/emsdk` when
the toolchain is not installed at `/opt/emsdk`.

The generated local script is derived from
`.github/workflows/build-wasm.yml`. After changing that workflow, regenerate it
with:

```bash
node scripts/gen-wsl-build-script.mjs
```

## C API

The module exports the `one-wasm-slicer-api` 0.4 ABI. The old `orc_*` and
0.2 STL-specific symbols are not part of the canonical surface; hosts use the
project-oriented API below:

```text
onewasm_session_create / onewasm_session_destroy
onewasm_init / onewasm_init_profile / onewasm_apply_profile / onewasm_set_progress_callback
onewasm_cancel
onewasm_project_set_objects / onewasm_project_get_manifest
onewasm_project_prepare / onewasm_project_slice
onewasm_project_get_asset / onewasm_project_export
onewasm_obj_to_stl / onewasm_cad_to_stl
onewasm_get_capabilities
onewasm_free / onewasm_last_error
```

The Emscripten exports therefore use `_onewasm_*` names. The canonical header
is vendored at [`bridge/onewasm_slicer_api.h`](bridge/onewasm_slicer_api.h) and
is synchronized with the private
[`one-wasm-slicer-api`](https://github.com/Hiosdra/one-wasm-slicer-api)
repository at `v0.4.0`. The extra `onewasm_read_3mf` export is a geometry-only
PoC import helper, outside the canonical project ABI. An Orca project `.3mf`
is loaded as a native profile with
`onewasm_init_profile(session, "project.3mf", ...)`.

## one-wasm-slicer-api compatibility

| Target capability | OrcaWasm 0.4 status | Evidence |
|---|---|---|
| Session lifecycle | supported | `onewasm_session_create/destroy` |
| Native config initialization | supported | `onewasm_init`, format `orca.native-json` |
| Full native profile | supported | `onewasm_init_profile`, format `project.3mf` |
| Native profile fragments | supported | `onewasm_apply_profile`, formats `orca.profile-json` and `orca.native-json` |
| Progress callback | supported | `onewasm_set_progress_callback` plus worker progress messages |
| Neutral project manifest | supported | `onewasm_project_set_objects/get_manifest` |
| Project transforms | supported | row-major 4x4 affine matrices |
| Auto-orient / arrange | supported | `onewasm_project_prepare` |
| Single/multi/all-plate slice | supported | `onewasm_project_slice` with selected/all plate selection |
| Per-plate G-code/statistics | supported | result manifest plus `onewasm_project_get_asset` |
| OBJ / STEP to STL | supported | `onewasm_obj_to_stl` / `onewasm_cad_to_stl` |
| Native project export | supported with limits | `onewasm_project_export`, explicit preservation policy |
| G-code in native 3MF | unsupported | `includeSliceArtifacts=true` returns `ONEWASM_ERR_UNSUPPORTED` |
| Capability metadata | supported | `onewasm_get_capabilities` |
| Stable status and ownership | supported | 0.3 status values and `onewasm_free` |
| Cooperative cancellation | supported | `onewasm_cancel`, native `PrintBase::cancel()`, `-11` completion status |

### 0.3 project adapter scope

The bridge implements the promoted 0.3 project adapter:

| 0.3 surface | Status | Scope |
|---|---|---|
| `onewasm_project_set_objects` | implemented | neutral 0.3 manifest plus owned concatenated STL blob |
| `onewasm_project_get_manifest` | implemented | returns the neutral project state |
| `onewasm_project_prepare` | implemented | native Orca arrange and auto-orient per plate |
| `onewasm_project_slice` | implemented | selected/all plates, sequential adapter orchestration, exact 0.3 affine transforms, per-plate result assets/statistics; serializable native meshes are materialized into the neutral blob |
| `onewasm_project_get_asset` | implemented | reads G-code and exported-project assets from the last successful project operation |
| `onewasm_project_export` | implemented with limits | clean native projects can be passed through losslessly; host/dirty projects are regenerated with `require`/`best-effort`/`portable` reporting; `includeSliceArtifacts=true` is still unsupported |
| `init_profile("project.3mf", ...)` as a full project load | implemented with limits | OrcaSlicer loads native configuration/project data and the bridge exposes a neutral manifest; serializable native meshes are copied for later prepare/slice/export |

The canonical contract and schemas live in the private
[`one-wasm-slicer-api`](https://github.com/Hiosdra/one-wasm-slicer-api)
repository.

## CI and releases

The `Build WASM` workflow validates pull requests and builds both `st` and
`mt` variants on the default branch. Each successful build runs the real
engine smoke test before publishing immutable GitHub Release assets:

```text
wasm-v2.4.2
wasm-v2.4.2-patchN
wasm-v2.4.2-patchN-multithreaded
```

A rebuild never overwrites an existing release. Consumers should resolve the
highest patch number in the desired release family and use the JavaScript and
WASM files from the same tag. This repository publishes engine releases only;
the frontend deployment is handled separately by the JustSlice-PoC Cloudflare
Workers project.

## Licence and notices

OrcaSlicer and the linked libraries retain their upstream licences. See
[`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md) for source and attribution
details. The bridge and build infrastructure are original project code under the
licence stated in `LICENSE`.
