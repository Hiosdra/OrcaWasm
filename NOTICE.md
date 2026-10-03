# Third-party notices

## one-wasm-slicer-api glue

`wasm/onewasm/onewasm-emscripten-glue.js` and
`wasm/onewasm/onewasm-conformance.mjs` are copied unmodified from the
one-wasm-slicer-api 0.6.0 release. The glue is linked into `slicer-mt.js`.

- License: Apache License 2.0
- Copyright: one-wasm-slicer-api contributors

## OrcaSlicer

This repository embeds a WebAssembly build of
[OrcaSlicer](https://github.com/SoftFever/OrcaSlicer).

- License: GNU Affero General Public License v3.0 (AGPL-3.0)
- Copyright: SoftFever and OrcaSlicer contributors
- Source version: https://github.com/SoftFever/OrcaSlicer/tree/v2.4.2
- Compatibility changes: [`patches/apply.py`](patches/apply.py)

Per AGPL-3.0 §13, the complete corresponding source for this modified build,
including the patches, is available in this repository.

## PrusaSlicer / libslic3r

OrcaSlicer is a fork of
[PrusaSlicer](https://github.com/prusa3d/PrusaSlicer), also licensed under
AGPL-3.0.

## OCCT (Open CASCADE Technology)

OCCT 7.8.1 is compiled into the engine for STEP import:
https://github.com/Open-Cascade-SAS/OCCT

- License: Open CASCADE Technology Public License (modified LGPL-2.1 with a
  static-linking exception)
- Copyright: Open Cascade SAS and OCCT contributors

## oneTBB

The engine links [oneTBB](https://github.com/uxlfoundation/oneTBB) v2021.13.2,
built from source for `wasm32-emscripten` with pthread support.

- License: Apache License 2.0
- Copyright: Intel Corporation and oneTBB contributors

## libnoise

libnoise is compiled into the engine for the FuzzySkin feature:
https://github.com/amitsharma/libnoise

- License: GNU Lesser General Public License v2.1 (LGPL-2.1)
- Copyright: Jason Bevins and libnoise contributors
