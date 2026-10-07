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
scripts/              local build, smoke-test, and build-probe tools
.github/workflows/    reproducible CI build and release workflow
```

## Artifacts

| File | Description |
|------|-------------|
| `slicer-mt.js` / `slicer-mt.wasm` | Multithreaded engine |

The engine requires `SharedArrayBuffer` and a cross-origin isolated page.
Web hosts must provide the appropriate COOP/COEP response headers.

The build does not produce `slicer.data`: the headless engine uses its virtual
filesystem only for input and output files.

## Local build

Install Emscripten 3.1.74 and the system tools used by CI (CMake, Ninja,
Python 3, `m4`, `texinfo`, OpenSSL, `ccache`, and a C/C++ toolchain). Then run:

```bash
./scripts/build-local-wsl.sh
```

The script builds the pinned OrcaSlicer dependencies, applies `patches/apply.py`,
and writes the threaded pair to `artifacts/`. Use `EMSDK=/path/to/emsdk` when
the toolchain is not installed at `/opt/emsdk`. Its Emscripten cache is stored
under `build-wasm/emscripten-cache-mt`; set `WASM_EM_CACHE` to choose a
different writable cache directory.

The generated local script is derived from
`.github/workflows/build-wasm.yml`. After changing that workflow, regenerate it
with:

```bash
node scripts/gen-wsl-build-script.mjs
```

## Engine API

The module implements exactly `one-slicer-api` 0.7.0-pre.1 per engine release.
It does not negotiate or claim compatibility with older API versions.

Hosts use the **TypeScript binding**. `slicer-mt.js` embeds the reference
Emscripten glue from the API release ([`wasm/oneslicer/`](wasm/oneslicer)) and
exposes one `OneSlicerEngine` object:

```js
// slicer-mt.js loaded as a classic script, or wrapped as a module with
// `export default OneSlicerEngine;`
const engine = await OneSlicerEngine.createEngine({ runtime: emscriptenModuleOverrides })
const session = await engine.createSession()
await session.init(nativeConfigBytes)
```

The engine itself implements the **C binding** declared by the vendored header
[`bridge/oneslicer_api.h`](bridge/oneslicer_api.h), which tracks the
private [`one-slicer-api`](https://github.com/Hiosdra/one-wasm-slicer-api)
specification. The glue is the only code that touches the heap:

```text
oneslicer_session_create / oneslicer_session_destroy
oneslicer_init / oneslicer_init_profile / oneslicer_apply_profile / oneslicer_set_progress_callback
oneslicer_cancel
oneslicer_project_set_objects / oneslicer_project_get_manifest
oneslicer_project_get_preview
oneslicer_project_prepare / oneslicer_project_slice
oneslicer_project_get_asset / oneslicer_project_export
oneslicer_obj_to_stl / oneslicer_cad_to_stl / oneslicer_three_mf_to_stl
oneslicer_get_capabilities
oneslicer_free / oneslicer_last_error
```

`oneslicer_three_mf_to_stl` is optional `format.threeMfToStl` (geometry only). An Orca
project `.3mf` is loaded as a native profile with `initProfile("project.3mf", ...)`.

To update the API, copy `js/glue/oneslicer-emscripten-glue.js` and
`include/oneslicer_api.h` unmodified from the API release into
`wasm/oneslicer/` and `bridge/`. The vendored conformance suite is based on
`js/glue/oneslicer-conformance.mjs`; its `sameJson` helper canonicalizes object
keys because JSON member order is not part of the API contract.

## one-slicer-api compatibility

The eight `requiredIn: "core"` features are `core.session`,
`core.configuration`, `project.manifest`, `project.slice`,
`project.slice.assets`, `runtime.progress`, `runtime.capabilities` and
`runtime.errors`; this source reports them as supported.
The host must reject this engine if a required feature is absent or not
`supported`.

All other features are optional and are selected from `oneslicer_get_capabilities`
at runtime. This build reports profile initialization/application, modifier
volumes and native modifier preservation, prepare/arrange, multi-plate slicing,
project export with slice artifacts and preservation, OBJ/STEP/3MF conversion,
and cancellation as supported. STEP project meshes are imported and tessellated
through OrcaSlicer's native reader, then sliced through the project path;
`project.meshFormat.step` remains `partial` while unit, sheet-body, and warning
conformance is completed. Mapped STEP previews and project face attributes are
reported as `unsupported`; `oneslicer_project_get_preview` remains exported by
the C ABI and returns `UNSUPPORTED`. Hosts must read every optional status from
the capability document and must not infer support from an exported C symbol.

Cancellation also covers running operations in the TypeScript binding. The
reference glue calls the synchronous C ABI on the runtime's JS thread, where a
host abort could only take effect before an operation starts. The artifact's
`engine-binding.js` therefore runs `slice`, `prepare` and `export` on a pthread
through the private `orcawasm_async_start`/`orcawasm_async_poll` exports: the
JS thread stays responsive, delivers progress, and calls `oneslicer_cancel` when
the signal aborts. The operation then rejects with `CANCELLED` and the session
remains usable. C callers keep the synchronous ABI and call `oneslicer_cancel`
from another thread. `scripts/binding-test.mjs` checks this in CI.

Mapped previews and face attributes remain optional parts of the normative
0.7.0-pre.1 contract. This engine does not advertise or implement them.

Every payload (project manifest, prepare/slice/export requests and results,
slice statistics) uses `schemaVersion: "0.7.0-pre.1"`, the API version, including
ordinary projects with an empty `modifierVolumes` array. The bridge rejects
any other `schemaVersion`.

The bridge implements native Orca arrange and auto-orient, sequential
selected/all-plate slicing, per-plate result assets/statistics, native project
loading, and project export subject to its advertised preservation policies.
Native project modifier round-tripping covers support enforcers and blockers;
unrepresentable native volume types fail explicitly.

`includeSliceArtifacts: true` embeds the current G-code of each sliced plate
the way desktop Orca saves a sliced project (`Metadata/plate_N.gcode`, its
`.md5`, the plate's `gcode_file` reference and its `slice_info.config` entry).
The package keeps every manifest plate as a native plate, placed in Orca's
plate grid, and writes the complete configuration. Without a current G-code
result the export fails with `NO_DATA`; a plate with objects but no current
G-code (after a selected-plate slice) is reported as `slice-artifact-missing`.
An imported project is regenerated rather than passed through, so its opaque
entries follow the requested preservation policy.

The canonical contract and schemas live in the private
[`one-slicer-api`](https://github.com/Hiosdra/one-wasm-slicer-api)
repository.

## Exception model and Memory64 decision

Both wasm32 artifacts use native WebAssembly exception handling. C++ builds
compile and link with `-fwasm-exceptions`; C code that uses `setjmp` or
`longjmp` explicitly uses `-sSUPPORT_LONGJMP=wasm`. The bundled zlib, libpng,
and libjpeg ports are built with matching settings. OCCT signal conversion is
disabled because it relies on `setjmp`/`longjmp`, and the oneTBB Emscripten
profile no longer overrides native EH with `-fexceptions`. The workflow runs
[`scripts/check-wasm-eh.sh`](scripts/check-wasm-eh.sh) before each engine build;
it exercises mixed C/C++ exceptions and longjmp, pthread execution, and
separate wasm32/wasm64 toolchain probes.

Memory64 is not shipped. The pthread-enabled wasm32/wasm64 probes pass with
Emscripten 3.1.74 and Node.js 26.9.0. CI pins Node.js 26 for this check because
the runner's Node 22 cannot instantiate the final wasm64 table encoding. The
wasm32 probe measured 46,880 bytes and the wasm64 probe 48,834 bytes. These are
probe sizes, not engine benchmarks. The 64-bit probe reports
8-byte pointers, so an array of 1,048,576 pointer slots uses 8 MiB instead
of 4 MiB.

The versioned API payloads use `uint32_t` byte lengths. The extension worker also
writes and reads pointer outputs through `HEAPU32` and `i32`, which assumes
wasm32 pointers. `wasm/CMakeLists.txt` caps memory at 4 GiB as well. A
Memory64 engine would require a coordinated API and worker ABI change and a
larger-memory configuration; there is no measured workload here that needs
those changes. Keep the current wasm32 artifact contract until a real model
demonstrates the need and the API/worker path is updated.

Implementation references: [Emscripten C++ exceptions](https://emscripten.org/docs/porting/exceptions.html),
[Emscripten setjmp/longjmp](https://emscripten.org/docs/porting/setjmp-longjmp.html),
and the [WebAssembly Memory64 proposal](https://github.com/WebAssembly/memory64/blob/main/proposals/memory64/Overview.md).

## CI and releases

The `Build WASM` workflow validates pull requests and builds the pthread-enabled
engine on the default branch. Each successful build runs the real engine smoke
test and the one-slicer-api conformance suite (`scripts/conformance.mjs`,
through the embedded TypeScript binding) before publishing immutable GitHub
Release assets:

```text
wasm-v2.4.2-patchN-multithreaded
```

The first build for an OrcaSlicer version uses the corresponding
`wasm-vX.Y.Z-multithreaded` tag; later engine changes use immutable patch tags.

The smoke test exercises API 0.7.0-pre.1 support enforcers and blockers both with a
minimal config and with the reduced selected-profile fixture at
`scripts/fixtures/voron-0.4-profile-smoke.json`. The profile export does not
include the active bed surface, so this test explicitly uses `Textured PEI
Plate` as a test-only setting; it does not select a surface for the printer.

A rebuild never overwrites an existing release. Consumers should resolve the
highest patch number in the desired release family and use the JavaScript and
WASM files from the same tag. Each release advertises one exact API version;
these source changes do not publish a release. This repository publishes engine releases only;
the frontend deployment is handled separately by the JustSlice-PoC Cloudflare
Workers project.

Pull requests from branches of this repository that pass the smoke and
conformance tests also publish a **prerelease**
`wasm-v2.4.2-pr<number>.<run>-multithreaded`, so JustSlice-PoC can select the
build before merge. Prereleases never match the `-patchN` numbering and are
skipped by consumers that resolve the newest release. Closing the PR deletes
its prereleases and tags (`cleanup-pr-prereleases.yml`).

## Licence and notices

OrcaSlicer and the linked libraries retain their upstream licences. See
[`LICENSE`](LICENSE) and [`NOTICE.md`](NOTICE.md) for source and attribution
details. The bridge and build infrastructure are original project code under the
licence stated in `LICENSE`.
