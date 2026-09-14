#!/usr/bin/env node
/**
 * WASM engine smoke test.
 *
 * Loads the built slicer.js/slicer.wasm and runs several real onewasm_init +
 * onewasm_slice_stl(_multi)/onewasm_write_3mf/onewasm_read_3mf calls end-to-end, so a broken
 * engine build is caught before it's ever published as a GitHub Release
 * (build-wasm.yml) or trusted by a host after the artifacts are prepared.
 *
 * This formalizes the ad-hoc reproduction script referenced (but never
 * committed) in wasm/CMakeLists.txt's --profiling-funcs comment,
 * which was used to root-cause the Voron Cube wall-generator crash — a
 * build that compiles fine can still trap on a real slice, and nobody
 * noticed until a live user's host session failed.
 *
 * Usage:
 *   node scripts/smoke-test.mjs [--wasm-dir artifacts] [--engine slicer] [--fixture path/to.stl]
 *
 * --engine selects the output-name stem (see wasm/CMakeLists.txt's
 * ORCA_WEB_WASM_OUTPUT_NAME) — "slicer" (default, single-threaded) or
 * "slicer-mt" (build-wasm.yml's mt matrix leg, real oneTBB).
 * The mt build never produces a plain
 * slicer.js/.wasm alias, so this must be passed explicitly for that variant.
 *
 * Without --fixture, the scenario runs against a synthetic torture-test mesh
 * generated in memory.
 * Pass --fixture to run against a specific STL.
 */

import { readFileSync } from 'node:fs'
import {
  sphereStl, loadModule, writeBytes, decodeError,
  initSession, sliceOnce, sliceMultiOnce, preparePlateOnce, encodeObjectTransforms,
  projectSetObjectsOnce, projectGetManifestOnce, projectPrepareOnce,
  projectSliceOnce, projectGetAssetOnce, projectExportOnce, checkedMalloc, free,
} from './lib/engine-harness.mjs'

// ── CLI args ──────────────────────────────────────────────────────────────────

function parseArgs(argv) {
  const args = { wasmDir: 'artifacts', engine: 'slicer', fixture: null }
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--wasm-dir') args.wasmDir = argv[++i]
    else if (argv[i] === '--engine') args.engine = argv[++i]
    else if (argv[i] === '--fixture') args.fixture = argv[++i]
  }
  return args
}

// ── synthetic torture-test mesh (subdivided icosphere) ───────────────────────
// sphereStl() (and its icosphere/trianglesToStl building blocks) live in
// ./lib/engine-harness.mjs.

function generateTortureStl() {
  return sphereStl(4, 10) // 20 * 4^4 = 5120 triangles, 10mm-radius sphere
}

function getCapabilitiesOnce(module) {
  const outPtrPtr = checkedMalloc(module, 4, 'capability output pointer')
  const outLenPtr = checkedMalloc(module, 4, 'capability output length')
  try {
    const rc = module._onewasm_get_capabilities(outPtrPtr, outLenPtr)
    if (rc !== 0) throw new Error(`onewasm_get_capabilities failed (${rc}): ${decodeError(module, 0)}`)
    const ptr = module.getValue(outPtrPtr, 'i32')
    const len = module.getValue(outLenPtr, 'i32')
    try {
      const capabilities = JSON.parse(new TextDecoder().decode(module.HEAPU8.slice(ptr, ptr + len)))
      if (capabilities.api?.name !== 'one-wasm-slicer-api' || capabilities.api?.version !== '0.2.0') {
        throw new Error('capabilities document does not identify one-wasm-slicer-api 0.2.0')
      }
      return capabilities
    } finally {
      module._onewasm_free(ptr)
    }
  } finally {
    module._free(outLenPtr)
    module._free(outPtrPtr)
  }
}

function getStatisticsOnce(module, session) {
  const outPtrPtr = checkedMalloc(module, 4, 'statistics output pointer')
  const outLenPtr = checkedMalloc(module, 4, 'statistics output length')
  try {
    const rc = module._onewasm_get_last_statistics(session, outPtrPtr, outLenPtr)
    if (rc !== 0) throw new Error(`onewasm_get_last_statistics failed (${rc}): ${decodeError(module, session)}`)
    const ptr = module.getValue(outPtrPtr, 'i32')
    const len = module.getValue(outLenPtr, 'i32')
    try { return JSON.parse(new TextDecoder().decode(module.HEAPU8.slice(ptr, ptr + len))) } finally { module._onewasm_free(ptr) }
  } finally {
    module._free(outLenPtr)
    module._free(outPtrPtr)
  }
}

function assertNoStatistics(module, session, label) {
  const outPtrPtr = checkedMalloc(module, 4, 'empty statistics output pointer')
  const outLenPtr = checkedMalloc(module, 4, 'empty statistics output length')
  try {
    const rc = module._onewasm_get_last_statistics(session, outPtrPtr, outLenPtr)
    if (rc !== -12) throw new Error(`${label}: expected ONEWASM_ERR_NO_DATA (-12), got ${rc}`)
    if (module.getValue(outPtrPtr, 'i32') !== 0 || module.getValue(outLenPtr, 'i32') !== 0) {
      throw new Error(`${label}: no-data getter returned a non-empty output`)
    }
  } finally {
    module._free(outLenPtr)
    module._free(outPtrPtr)
  }
}

function assertStatistics(statistics, label) {
  if (statistics?.schemaVersion !== '0.2') throw new Error(`${label}: invalid statistics schema version`)
  if (!statistics.timeSeconds || !statistics.filament) throw new Error(`${label}: incomplete statistics document`)
  if (!Array.isArray(statistics.filament.lengthMmByExtruder)) throw new Error(`${label}: missing per-extruder filament lengths`)
  if (!Array.isArray(statistics.printingExtruders)) throw new Error(`${label}: missing printing extruder list`)
}

function initProfileOnce(module, session, mfBytes) {
  const formatBytes = new TextEncoder().encode('project.3mf')
  const formatPtr = writeBytes(module, formatBytes)
  const profilePtr = writeBytes(module, mfBytes)
  try {
    const rc = module._onewasm_init_profile(session, formatPtr, formatBytes.length, profilePtr, mfBytes.length)
    if (rc !== 0) throw new Error(`onewasm_init_profile failed (${rc}): ${decodeError(module, session)}`)
  } finally {
    free(module, profilePtr)
    free(module, formatPtr)
  }
}

// ── engine harness ──────────────────────────────────────────────────────────
// loadModule() + the onewasm_* heap marshaling (writeBytes/decodeError/
// initSession/sliceOnce/sliceMultiOnce) live in ./lib/engine-harness.mjs.

function write3mfOnce(module, session, stlBytes) {
  const stlPtr = writeBytes(module, stlBytes)
  try {
    const outPtrPtr = checkedMalloc(module, 4, '3MF output pointer')
    try {
      const outLenPtr = checkedMalloc(module, 4, '3MF output length')
      try {
        const rc = module._onewasm_write_3mf(session, stlPtr, stlBytes.length, outPtrPtr, outLenPtr)
        if (rc !== 0) throw new Error(`onewasm_write_3mf failed (${rc}): ${decodeError(module, session)}`)
        const dataPtr = module.getValue(outPtrPtr, 'i32')
        const dataLen = module.getValue(outLenPtr, 'i32')
        try { return module.HEAPU8.slice(dataPtr, dataPtr + dataLen) } finally { module._onewasm_free(dataPtr) }
      } finally { module._free(outLenPtr) }
    } finally { module._free(outPtrPtr) }
  } finally { free(module, stlPtr) }
}

function read3mfOnce(module, mfBytes) {
  const mfPtr = writeBytes(module, mfBytes)
  try {
    const outStlPtrPtr = checkedMalloc(module, 4, 'STL output pointer')
    try {
      const outStlLenPtr = checkedMalloc(module, 4, 'STL output length')
      try {
        const rc = module._onewasm_read_3mf(mfPtr, mfBytes.length, outStlPtrPtr, outStlLenPtr)
        if (rc !== 0) throw new Error(`onewasm_read_3mf failed (${rc}): ${decodeError(module, 0)}`)
        const stlPtr = module.getValue(outStlPtrPtr, 'i32'), stlLen = module.getValue(outStlLenPtr, 'i32')
        try { return module.HEAPU8.slice(stlPtr, stlPtr + stlLen) } finally { module._onewasm_free(stlPtr) }
      } finally { module._free(outStlLenPtr) }
    } finally { module._free(outStlPtrPtr) }
  } finally { free(module, mfPtr) }
}

// Binary STL: 80-byte header + uint32 triangle count + N * 50 bytes.
function stlTriangleCount(bytes) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(80, true)
}

function stlBounds(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const triangles = view.getUint32(80, true)
  const bounds = {
    min: [Infinity, Infinity, Infinity],
    max: [-Infinity, -Infinity, -Infinity],
  }
  for (let triangle = 0; triangle < triangles; triangle++) {
    const triangleOffset = 84 + triangle * 50
    for (let vertex = 0; vertex < 3; vertex++) {
      const vertexOffset = triangleOffset + 12 + vertex * 12
      for (let axis = 0; axis < 3; axis++) {
        const value = view.getFloat32(vertexOffset + axis * 4, true)
        bounds.min[axis] = Math.min(bounds.min[axis], value)
        bounds.max[axis] = Math.max(bounds.max[axis], value)
      }
    }
  }
  return bounds
}

// Small stored ZIP writer used only for a deterministic parser regression. It
// keeps the smoke test dependency-free (the CI job intentionally does not run
// npm install) while exercising the real 3MF component graph in both engines.
function crc32(bytes) {
  let crc = 0xffffffff
  for (const byte of bytes) {
    crc ^= byte
    for (let bit = 0; bit < 8; bit++) {
      crc = (crc & 1) ? ((crc >>> 1) ^ 0xedb88320) >>> 0 : crc >>> 1
    }
  }
  return (crc ^ 0xffffffff) >>> 0
}

function makeStoredZip(entries) {
  const encoder = new TextEncoder()
  const records = entries.map(({ name, data }) => ({
    name: encoder.encode(name),
    data: typeof data === 'string' ? encoder.encode(data) : new Uint8Array(data),
  }))
  const localSize = records.reduce((sum, record) => sum + 30 + record.name.length + record.data.length, 0)
  const centralSize = records.reduce((sum, record) => sum + 46 + record.name.length, 0)
  const centralOffset = localSize
  const bytes = new Uint8Array(localSize + centralSize + 22)
  const view = new DataView(bytes.buffer)
  let position = 0
  const localOffsets = []
  for (const record of records) {
    localOffsets.push(position)
    view.setUint32(position, 0x04034b50, true)
    view.setUint16(position + 4, 20, true)
    view.setUint16(position + 6, 0x800, true) // UTF-8 names
    view.setUint16(position + 8, 0, true) // stored, not deflated
    view.setUint32(position + 10, 0, true)
    view.setUint32(position + 14, crc32(record.data), true)
    view.setUint32(position + 18, record.data.length, true)
    view.setUint32(position + 22, record.data.length, true)
    view.setUint16(position + 26, record.name.length, true)
    view.setUint16(position + 28, 0, true)
    bytes.set(record.name, position + 30)
    bytes.set(record.data, position + 30 + record.name.length)
    position += 30 + record.name.length + record.data.length
  }
  const centralStart = position
  records.forEach((record, index) => {
    view.setUint32(position, 0x02014b50, true)
    view.setUint16(position + 4, 20, true)
    view.setUint16(position + 6, 20, true)
    view.setUint16(position + 8, 0x800, true)
    view.setUint16(position + 10, 0, true)
    view.setUint32(position + 12, 0, true)
    view.setUint32(position + 16, crc32(record.data), true)
    view.setUint32(position + 20, record.data.length, true)
    view.setUint32(position + 24, record.data.length, true)
    view.setUint16(position + 28, record.name.length, true)
    view.setUint16(position + 30, 0, true)
    view.setUint16(position + 32, 0, true)
    view.setUint16(position + 34, 0, true)
    view.setUint16(position + 36, 0, true)
    view.setUint32(position + 38, 0, true)
    view.setUint32(position + 42, localOffsets[index], true)
    bytes.set(record.name, position + 46)
    position += 46 + record.name.length
  })
  if (position !== centralStart + centralSize) throw new Error('component ZIP central directory size mismatch')
  view.setUint32(position, 0x06054b50, true)
  view.setUint16(position + 4, 0, true)
  view.setUint16(position + 6, 0, true)
  view.setUint16(position + 8, records.length, true)
  view.setUint16(position + 10, records.length, true)
  view.setUint32(position + 12, centralSize, true)
  view.setUint32(position + 16, centralOffset, true)
  view.setUint16(position + 20, 0, true)
  return bytes
}

function makeComponent3mf() {
  const model = `<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
  <resources>
    <object id="1" type="model">
      <mesh>
        <vertices>
          <vertex x="0" y="0" z="0"/>
          <vertex x="1" y="0" z="0"/>
          <vertex x="0" y="1" z="0"/>
        </vertices>
        <triangles><triangle v1="0" v2="1" v3="2"/></triangles>
      </mesh>
    </object>
    <object id="2" type="model">
      <components>
        <component objectid="1" transform="1 0 0 5 0 1 0 0 0 0 1 0"/>
      </components>
    </object>
  </resources>
  <build><item objectid="2" transform="1 0 0 10 0 1 0 0 0 0 1 0"/></build>
</model>`
  return makeStoredZip([
    {
      name: '[Content_Types].xml',
      data: `<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Override PartName="/3D/3dmodel.model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
</Types>`,
    },
    {
      name: '_rels/.rels',
      data: `<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel" Target="3D/3dmodel.model"/>
</Relationships>`,
    },
    { name: '3D/3dmodel.model', data: model },
  ])
}

function assertComponent3mfRead(module) {
  const label = 'component 3MF read (nested transform graph)'
  const stl = read3mfOnce(module, makeComponent3mf())
  if (stlTriangleCount(stl) !== 1) {
    throw new Error(`${label}: expected one flattened triangle, got ${stlTriangleCount(stl)}`)
  }
  const bounds = stlBounds(stl)
  if (Math.abs(bounds.min[0] - 15) > 0.01 || Math.abs(bounds.max[0] - 16) > 0.01
    || Math.abs(bounds.min[1]) > 0.01 || Math.abs(bounds.max[1] - 1) > 0.01) {
    throw new Error(`${label}: composed component/build transform was not applied: ${JSON.stringify(bounds)}`)
  }
}

// Minimal ZIP central-directory reader — deliberately hand-rolled rather than
// pulling in a zip library (e.g. fflate, used for this in src/lib/*.ts): this
// script also runs inside build-wasm.yml's "Smoke test WASM module" step,
// which only sets up Node (actions/setup-node) and never runs `npm install`
// — so no package outside Node's stdlib is resolvable there. Only reads
// filenames from the central directory (no decompression needed for this
// check), and assumes the classic (non-Zip64) EOCD/CD record layout, which is
// what miniz (OrcaSlicer's zip writer) emits for archives this small — Zip64
// records only get written once entry count or size actually exceeds the
// 32-bit fields' range.
function findEndOfCentralDirectory(bytes) {
  const minPos = Math.max(0, bytes.length - 22 - 65535) // EOCD (22B) + max comment (64KB)
  for (let i = bytes.length - 22; i >= minPos; i--) {
    if (bytes[i] === 0x50 && bytes[i + 1] === 0x4b && bytes[i + 2] === 0x05 && bytes[i + 3] === 0x06) {
      return i
    }
  }
  return -1
}

function listZipEntryNames(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
  const eocd = findEndOfCentralDirectory(bytes)
  if (eocd < 0) throw new Error('no End Of Central Directory record found')
  const totalEntries = dv.getUint16(eocd + 10, true)
  const cdOffset = dv.getUint32(eocd + 16, true)

  const names = []
  let pos = cdOffset
  for (let i = 0; i < totalEntries; i++) {
    const sig = dv.getUint32(pos, true)
    if (sig !== 0x02014b50) throw new Error(`bad central directory entry signature at offset ${pos}`)
    const nameLen = dv.getUint16(pos + 28, true)
    const extraLen = dv.getUint16(pos + 30, true)
    const commentLen = dv.getUint16(pos + 32, true)
    const nameStart = pos + 46
    names.push(new TextDecoder('utf-8').decode(bytes.subarray(nameStart, nameStart + nameLen)))
    pos = nameStart + nameLen + extraLen + commentLen
  }
  return names
}

// A .3mf is a ZIP; verify it round-trips as one and carries the two pieces
// onewasm_write_3mf's contract promises: the mesh (3D/3dmodel.model) and the
// embedded OrcaSlicer settings (a Metadata/*.config file — see
// EMBEDDED_PRINT_FILE_FORMAT et al. in bbs_3mf.hpp for why the filename
// isn't a fixed constant).
function assertValid3mf(bytes, label) {
  if (bytes.length < 4 || bytes[0] !== 0x50 || bytes[1] !== 0x4b) {
    throw new Error(`${label}: output does not start with a ZIP (PK) signature`)
  }
  let names
  try {
    names = listZipEntryNames(bytes)
  } catch (err) {
    throw new Error(`${label}: failed to read ZIP central directory: ${err.message}`)
  }
  if (!names.includes('3D/3dmodel.model')) {
    throw new Error(`${label}: missing 3D/3dmodel.model (entries: ${names.join(', ')})`)
  }
  if (!names.some((n) => /^Metadata\/.*\.config$/.test(n))) {
    throw new Error(`${label}: missing a Metadata/*.config file (entries: ${names.join(', ')})`)
  }
}

// ── sanity assertions on the resulting G-code ─────────────────────────────────

function assertSaneGcode(gcode, label) {
  if (!gcode || gcode.length < 200) throw new Error(`${label}: G-code suspiciously short/empty (${gcode?.length ?? 0} bytes)`)
  if (!/^G1 |\nG1 /.test(gcode)) throw new Error(`${label}: no G1 extrusion moves found`)
  const lines = gcode.split('\n').length
  if (lines < 50) throw new Error(`${label}: only ${lines} lines — expected a real multi-layer slice`)
}

// Regression guard for the bug fixed by center_object_xy_only() in
// slicer.cpp: center_around_origin() used to center the mesh on Z too,
// leaving the object floating with its vertical midpoint (not its base) at
// the bed plane. The torture mesh's base sits at z=0 (see icosphere()
// above), so a correctly-placed slice's lowest G0/G1 Z move should land at
// the first layer height, never deep below zero (sunk into the bed) or
// far above it (floating).
function assertRestsOnBed(gcode, label, firstLayerHeight) {
  const zValues = []
  for (const line of gcode.split('\n')) {
    if (!/^G[01] /.test(line)) continue
    const m = line.match(/Z(-?[0-9.]+)/)
    if (m) zValues.push(parseFloat(m[1]))
  }
  if (zValues.length === 0) throw new Error(`${label}: no Z-bearing G0/G1 moves found`)
  const minZ = Math.min(...zValues)
  if (minZ < -0.01) throw new Error(`${label}: minimum Z is ${minZ} — model appears to be sunk below the bed`)
  if (minZ > firstLayerHeight + 0.05) throw new Error(`${label}: minimum Z is ${minZ}, expected ~${firstLayerHeight} — model appears to be floating above the bed`)
}

// Adaptive (variable) layer height (#138) must actually vary the layer
// thickness — a fixed-height slice emits one repeated ;Z: step (plus at most a
// thin top-cap remainder), an adaptive one emits many. Parse the engine's own
// ;Z:<height> layer-change markers (the authoritative per-layer Z, unlike a raw
// G1 Z scan which also picks up the start-gcode nozzle lift and travel Z-hops)
// and require several distinct steps. The >=3 threshold cleanly separates a
// fixed slice (<=2 distinct: the nominal height + maybe the remainder) from an
// adaptive one (both smoke meshes yield well over a dozen).
function assertVariableLayerHeights(gcode, label) {
  const zs = []
  for (const line of gcode.split('\n')) {
    const m = /^;Z:(-?[0-9.]+)/.exec(line)
    if (m) zs.push(parseFloat(m[1]))
  }
  if (zs.length < 5) throw new Error(`${label}: only ${zs.length} ;Z: layer markers — expected a real multi-layer slice`)
  const steps = new Set()
  for (let i = 1; i < zs.length; i++) steps.add((zs[i] - zs[i - 1]).toFixed(3))
  if (steps.size < 3) {
    throw new Error(`${label}: ${steps.size} distinct layer height(s) — adaptive layer height did not vary the thickness`)
  }
}

// ── real base config (Generic-ish printer + PLA + Standard) ───────────────────
// Deliberately not importing host profile adapter (TS + depends on the bundled
// orca-profiles.json shape); a minimal but real, representative config is
// enough to exercise the engine the same way the app's default preset does.

const BASE_CONFIG = {
  bed_size_x: 256,
  bed_size_y: 256,
  printable_height: 250,
  nozzle_diameter: 0.4,
  layer_height: 0.2,
  initial_layer_print_height: 0.2,
  wall_loops: 2,
  top_shell_layers: 3,
  bottom_shell_layers: 3,
  sparse_infill_density: 15,
  sparse_infill_pattern: 'grid',
  filament_type: 'PLA',
  nozzle_temperature: 220,
  bed_temperature: 55,
}

// A real dual-nozzle machine (Bambu Lab H2D shape) with two filament slots,
// one per nozzle — the exact config shape withFilamentSlots() in
// host profile adapter builds, written out here rather than imported for the
// same reason BASE_CONFIG is (see above).
//
// This is the scenario issue #140 was about, and it is worth a committed
// regression test for two independent reasons:
//
//  - Every multi-value option below has to survive serialization with the
//    separator its *type* requires. The bridge used to join all of them with
//    ',', which fused each per-nozzle printable area and colour into one
//    entry — leaving nozzle_diameter length 2 with length-1 companions,
//    indexed by extruder id, and dying in Brim.cpp. A plain "did it slice?"
//    check catches that, since it crashed rather than misbehaved quietly.
//  - The per-filament vectors must all agree in length, or the engine reads
//    past the end of one. flush_volumes_matrix in particular is one N×N
//    sub-matrix per nozzle laid end to end, which append_full_config()
//    validates against flush_multiplier's length.
const DUAL_NOZZLE_CONFIG = {
  ...BASE_CONFIG,
  nozzle_diameter: ['0.4', '0.4'],
  extruder_printable_area: ['0x0,325x0,325x320,0x320', '25x0,350x0,350x320,25x320'],
  filament_type: ['PLA', 'PETG'],
  filament_colour: ['#F5A623', '#4A90D9'],
  filament_diameter: ['1.75', '1.75'],
  nozzle_temperature: ['220', '255'],
  nozzle_temperature_initial_layer: ['220', '255'],
  hot_plate_temp: ['55', '70'],
  hot_plate_temp_initial_layer: ['55', '70'],
  filament_start_gcode: ['', ''],
  filament_end_gcode: ['', ''],
  filament_extruder_variant: ['Direct Drive Standard', 'Direct Drive Standard'],
  filament_self_index: ['1', '2'],
  // slot 1 -> nozzle 1, slot 2 -> nozzle 2: this is what produces T0/T1
  filament_map: ['1', '2'],
  filament_map_mode: 'Manual',
  // 2 nozzles x 2 filaments squared; 0 to purge a filament into itself
  flush_volumes_matrix: ['0', '280', '280', '0', '0', '280', '280', '0'],
  flush_multiplier: ['1', '1'],
  // The prime tower the flush volumes above flush into. The engine default is
  // off, so a multi-material plate has to turn it on explicitly (#163) — this
  // mirrors what withFilamentSlots() now emits, including its two companions:
  // the wipe tower validates only under relative extruder addressing, and that
  // addressing needs a per-layer "G92 E0" reset on a Marlin, non-Bambu printer
  // (this config is one — no "Bambu Lab" printer_model) or Print::validate()
  // hard-fails the slice. See withPrimeTowerAddressing() in host profile adapter.
  enable_prime_tower: '1',
  use_relative_e_distances: '1',
  before_layer_change_gcode: 'G92 E0',
}

// Single-nozzle AMS with two slots sharing the one nozzle, on a shallow 210 mm
// bed (a Prusa MK4). Both slots are the same material so the engine's
// mixed-nozzle-temperature guard (which fires only when filaments share a
// nozzle) doesn't reject the slice — this scenario is about placement, not
// temperature. The prime tower's engine-default position (15, 220) is off the
// back of a 210 mm bed; clamp_wipe_tower_to_bed in bridge/slicer.cpp is what
// keeps it on. See #163.
const SMALL_BED_AMS_CONFIG = {
  ...BASE_CONFIG,
  bed_size_y: 210,
  filament_type: ['PLA', 'PLA'],
  filament_colour: ['#F5A623', '#4A90D9'],
  filament_diameter: ['1.75', '1.75'],
  nozzle_temperature: ['220', '220'],
  nozzle_temperature_initial_layer: ['220', '220'],
  hot_plate_temp: ['55', '55'],
  hot_plate_temp_initial_layer: ['55', '55'],
  filament_start_gcode: ['', ''],
  filament_end_gcode: ['', ''],
  filament_map: ['1', '1'],
  filament_map_mode: 'Manual',
  flush_volumes_matrix: ['0', '280', '280', '0'],
  flush_multiplier: ['1'],
  enable_prime_tower: '1',
  use_relative_e_distances: '1',
  before_layer_change_gcode: 'G92 E0',
}

// Single-nozzle AMS with two filaments whose recommended nozzle-temperature
// ranges don't overlap (PLA ~190–230, PETG ~220–260) sharing one nozzle. The
// engine's mixed-temperature guard (Print::check_multi_filament_valid) rejects
// this with -6 by default; setting remove_mixed_temp_restriction makes the
// bridge call Print::set_check_multi_filaments_compatibility(false) so it
// slices anyway. Both the rejection and the override are asserted below (#164).
const MIXED_TEMP_AMS_CONFIG = {
  ...BASE_CONFIG,
  filament_type: ['PLA', 'PETG'],
  filament_colour: ['#F5A623', '#4A90D9'],
  filament_diameter: ['1.75', '1.75'],
  nozzle_temperature: ['220', '255'],
  nozzle_temperature_initial_layer: ['220', '255'],
  hot_plate_temp: ['55', '70'],
  hot_plate_temp_initial_layer: ['55', '70'],
  filament_start_gcode: ['', ''],
  filament_end_gcode: ['', ''],
  // both slots share nozzle 1 — this is what makes the guard fire (a genuine
  // dual-nozzle machine is exempt: each nozzle holds its own temperature).
  filament_map: ['1', '1'],
  filament_map_mode: 'Manual',
  flush_volumes_matrix: ['0', '280', '280', '0'],
  flush_multiplier: ['1'],
  enable_prime_tower: '1',
  use_relative_e_distances: '1',
  before_layer_change_gcode: 'G92 E0',
}

// Both nozzles actually used. A config that silently collapsed to a single
// filament still slices and still passes assertSaneGcode() — it just prints
// everything with one tool, which is precisely the failure mode #140's
// comma-joining produced once it stopped crashing outright.
function assertToolChanges(gcode, label) {
  const tools = new Set()
  for (const line of gcode.split('\n')) {
    const m = line.match(/^T(\d+)\b/)
    if (m) tools.add(m[1])
  }
  if (!tools.has('0') || !tools.has('1')) {
    throw new Error(`${label}: expected both T0 and T1 tool changes, saw [${[...tools].join(',') || 'none'}]`)
  }
}

// The prime tower has to sit on the bed. OrcaSlicer's fit-and-clamp is GUI-only,
// so the WASM bridge ports it (clamp_wipe_tower_to_bed in bridge/slicer.cpp);
// without that a small bed keeps the engine default (15, 220) and the tower
// hangs off the back edge. Reads the position the bridge wrote from the config
// block the engine appends to the G-code.
function assertWipeTowerOnBed(gcode, bedX, bedY, label) {
  const x = parseFloat(gcode.match(/;\s*wipe_tower_x\s*=\s*([\-0-9.]+)/)?.[1])
  const y = parseFloat(gcode.match(/;\s*wipe_tower_y\s*=\s*([\-0-9.]+)/)?.[1])
  if (!(x >= 0 && x < bedX) || !(y >= 0 && y < bedY)) {
    throw new Error(`${label}: prime tower origin (${x}, ${y}) is off a ${bedX}x${bedY} bed`)
  }
}

function assertValidPlateTransforms(transforms, expectedCount, label, requireFiniteOffsets) {
  if (!Array.isArray(transforms) || transforms.length !== expectedCount) {
    throw new Error(`${label}: expected ${expectedCount} object transforms, got ${JSON.stringify(transforms)}`)
  }
  for (let i = 0; i < transforms.length; i++) {
    const transform = transforms[i]
    if (
      !transform ||
      !Array.isArray(transform.scale) ||
      transform.scale.length !== 3 ||
      !transform.scale.every(Number.isFinite) ||
      transform.scale.some((value) => value <= 0)
    ) {
      throw new Error(`${label}: transform ${i} has an invalid scale`)
    }
    if (!Array.isArray(transform.rotation) || transform.rotation.length !== 3 || !transform.rotation.every(Number.isFinite)) {
      throw new Error(`${label}: transform ${i} has an invalid rotation`)
    }
    if (
      !Array.isArray(transform.mirror) ||
      transform.mirror.length !== 3 ||
      !transform.mirror.every((value) => value === 1 || value === -1)
    ) {
      throw new Error(`${label}: transform ${i} has an invalid mirror`)
    }
    if (requireFiniteOffsets) {
      if (!Array.isArray(transform.offset) || transform.offset.length !== 2 || !transform.offset.every(Number.isFinite)) {
        throw new Error(`${label}: transform ${i} has no finite arranged offset`)
      }
    } else if (
      transform.offset !== null &&
      (!Array.isArray(transform.offset) || transform.offset.length !== 2 || !transform.offset.every(Number.isFinite))
    ) {
      throw new Error(`${label}: transform ${i} has an invalid optional offset`)
    }
  }
}

// ── test meshes ──────────────────────────────────────────────────────────────
function collectMeshes(fixture) {
  if (fixture) return [{ label: fixture, bytes: readFileSync(fixture) }]
  return [{ label: 'synthetic icosphere (~5120 tris)', bytes: generateTortureStl() }]
}

function projectMatrix(tx, ty, tz = 0, shearXByY = 0) {
  // one-wasm-slicer-api 0.3 uses row-major matrices with column vectors.
  return [
    1, shearXByY, 0, tx,
    0, 1, 0, ty,
    0, 0, 1, tz,
    0, 0, 0, 1,
  ]
}

function makeProjectFixture(meshBytes) {
  return {
    blob: new Uint8Array(meshBytes),
    manifest: {
      schemaVersion: '0.3',
      plates: [
        { id: 'plate-0', label: 'Plate 0', index: 0 },
        { id: 'plate-1', label: 'Plate 1', index: 1 },
      ],
      meshes: [{
        id: 'mesh-0',
        format: 'stl',
        dataRange: { offset: 0, length: meshBytes.length },
      }],
      objects: [{ id: 'object-0', meshId: 'mesh-0', extruderId: 0 }],
      instances: [
        { id: 'instance-0', objectId: 'object-0', plateId: 'plate-0', transform: { matrix: projectMatrix(128, 128) } },
        { id: 'instance-1', objectId: 'object-0', plateId: 'plate-1', transform: { matrix: projectMatrix(128, 128) } },
      ],
    },
  }
}

function assertProjectManifest(manifest, label) {
  if (manifest?.schemaVersion !== '0.3') throw new Error(`${label}: invalid project manifest schema version`)
  if (!Array.isArray(manifest.plates) || manifest.plates.length !== 2) throw new Error(`${label}: expected two plates`)
  if (!Array.isArray(manifest.instances) || manifest.instances.length !== 2) throw new Error(`${label}: expected two instances`)
  for (const instance of manifest.instances) {
    const matrix = instance.transform?.matrix
    if (!Array.isArray(matrix) || matrix.length !== 16 || !matrix.every(Number.isFinite)) {
      throw new Error(`${label}: instance has an invalid affine matrix`)
    }
  }
}

function assertProjectSlice(module, session, result, expectedPlateIds, label) {
  if (result?.schemaVersion !== '0.3') throw new Error(`${label}: invalid project result schema version`)
  if (!Array.isArray(result.plateResults) || result.plateResults.length !== expectedPlateIds.length) {
    throw new Error(`${label}: unexpected plate result count`)
  }
  for (let index = 0; index < expectedPlateIds.length; index++) {
    const plateResult = result.plateResults[index]
    if (plateResult.plateId !== expectedPlateIds[index]) throw new Error(`${label}: plate result order/id mismatch`)
    if (!Array.isArray(plateResult.assets) || plateResult.assets.length !== 1) {
      throw new Error(`${label}: expected one G-code asset for ${plateResult.plateId}`)
    }
    const asset = plateResult.assets[0]
    if (asset.kind !== 'gcode' || asset.id !== `gcode:${plateResult.plateId}`) {
      throw new Error(`${label}: invalid G-code asset descriptor`)
    }
    const bytes = projectGetAssetOnce(module, session, asset.id)
    if (bytes.length !== asset.byteLength) throw new Error(`${label}: asset byte length mismatch`)
    assertSaneGcode(new TextDecoder().decode(bytes), `${label} ${plateResult.plateId}`)
    if (plateResult.statistics?.schemaVersion !== '0.3') {
      throw new Error(`${label}: statistics are not attached to ${plateResult.plateId}`)
    }
  }
}

function runProjectSmoke(module, session, meshBytes) {
  const project = makeProjectFixture(meshBytes)
  projectSetObjectsOnce(module, session, project.blob, project.manifest)
  assertProjectManifest(projectGetManifestOnce(module, session), 'project_set_objects/get_manifest')

  const prepared = projectPrepareOnce(module, session, {
    schemaVersion: '0.3',
    operation: 'arrange',
  })
  assertProjectManifest(prepared, 'project_prepare')

  // Arrange intentionally consumes the legacy decomposable transform shape.
  // Re-upload a valid full-affine manifest for slicing so this test also pins
  // the 0.3 direct-matrix path (including a shear) independently of arrange.
  const slicedManifest = JSON.parse(JSON.stringify(project.manifest))
  slicedManifest.instances[1].transform.matrix = projectMatrix(128, 128, 0, 0.1)
  projectSetObjectsOnce(module, session, project.blob, slicedManifest)

  const allResult = projectSliceOnce(module, session, {
    schemaVersion: '0.3',
    plateSelection: 'all',
    includeGcode: true,
    includeStatistics: true,
  })
  assertProjectSlice(module, session, allResult, ['plate-0', 'plate-1'], 'project_slice all')

  const selectedResult = projectSliceOnce(module, session, {
    schemaVersion: '0.3',
    plateSelection: 'selected',
    plateIds: ['plate-1'],
    includeGcode: true,
    includeStatistics: true,
  })
  assertProjectSlice(module, session, selectedResult, ['plate-1'], 'project_slice selected')

  const exportResult = projectExportOnce(module, session, 'project.3mf', {
    schemaVersion: '0.3',
    preservation: 'best-effort',
    includeSliceArtifacts: false,
  })
  if (exportResult?.schemaVersion !== '0.3'
    || exportResult.asset?.id !== 'project:export'
    || exportResult.asset.kind !== 'project'
    || exportResult.asset.mimeType !== 'model/3mf') {
    throw new Error('project_export returned an unexpected result descriptor')
  }
  const exported = projectGetAssetOnce(module, session, 'project:export')
  if (exported.length !== exportResult.asset.byteLength) {
    throw new Error('project_export asset byte length mismatch')
  }
  assertValid3mf(exported, 'project_export')
}

// ── main ──────────────────────────────────────────────────────────────────────

async function main() {
  const { wasmDir, engine, fixture } = parseArgs(process.argv.slice(2))
  console.log(`[smoke-test] loading engine "${engine}" from ${wasmDir}...`)
  const module = await loadModule(wasmDir, engine)
  console.log('[smoke-test] engine loaded')
  const capabilities = getCapabilitiesOnce(module)
  console.log(`[smoke-test] capabilities: ${capabilities.engine?.family ?? 'unknown'} ${capabilities.engine?.version ?? ''}`)

  const meshes = collectMeshes(fixture)
  for (const mesh of meshes) {
    console.log(`[smoke-test] mesh: ${mesh.label} (${mesh.bytes.length} bytes)`)
  }

  const supportsPlateActions = typeof module._onewasm_prepare_plate === 'function'
  if (!supportsPlateActions) {
    console.warn('[smoke-test] WARN: loaded engine has no _onewasm_prepare_plate export — skipping current-plate action scenarios')
  }
  const projectExports = [
    '_onewasm_project_set_objects',
    '_onewasm_project_get_manifest',
    '_onewasm_project_prepare',
    '_onewasm_project_slice',
    '_onewasm_project_get_asset',
    '_onewasm_project_export',
  ]
  const supportsProjectApi = projectExports.every((name) => typeof module[name] === 'function')
  if (!supportsProjectApi) {
    console.warn('[smoke-test] WARN: loaded engine has no complete draft 0.3 project surface — skipping project scenarios')
  }

  const session = module._onewasm_session_create()
  if (!session) throw new Error('onewasm_session_create failed (allocation failure)')
  if (module._onewasm_cancel(session) !== 0) throw new Error('idle onewasm_cancel was not a no-op')
  assertNoStatistics(module, session, 'before first successful slice')

  const scenarios = [
    {
      name: 'default (Arachne walls, no fuzzy skin)',
      config: BASE_CONFIG,
    },
    {
      name: 'fuzzy skin = all',
      config: { ...BASE_CONFIG, fuzzy_skin: 'all', fuzzy_skin_thickness: 0.3, fuzzy_skin_point_dist: 0.8 },
    },
    {
      name: 'classic wall generator (regression control vs. Arachne default)',
      config: { ...BASE_CONFIG, wall_generator: 'classic' },
    },
    {
      // Adaptive layer height is a bridge pseudo-key (#138) — the engine
      // computes each object's layer_height_profile before slicing. The extra
      // assert confirms the layer thickness actually varies, not just that the
      // slice succeeds (the pseudo-key being silently ignored would still emit
      // valid, uniform-height G-code).
      name: 'adaptive (variable) layer height',
      config: { ...BASE_CONFIG, adaptive_layer_height: true, adaptive_layer_height_quality: 0.5 },
      assert: assertVariableLayerHeights,
    },
  ]

  let failures = 0
  const componentLabel = '[3MF] component graph with composed transforms'
  process.stdout.write(`[smoke-test] ${componentLabel} ... `)
  try {
    assertComponent3mfRead(module)
    console.log('PASS (one flattened triangle at x=15..16)')
  } catch (err) {
    failures++
    console.log('FAIL')
    console.error(`  ${err.message}`)
  }

  for (const mesh of meshes) {
    for (const scenario of scenarios) {
      const label = `[${mesh.label}] ${scenario.name}`
      process.stdout.write(`[smoke-test] ${label} ... `)
      try {
        initSession(module, session, JSON.stringify(scenario.config))
        const gcode = sliceOnce(module, session, mesh.bytes)
        assertSaneGcode(gcode, label)
        assertRestsOnBed(gcode, label, scenario.config.initial_layer_print_height)
        assertStatistics(getStatisticsOnce(module, session), label)
        scenario.assert?.(gcode, label)
        console.log(`PASS (${gcode.length} bytes)`)
      } catch (err) {
        failures++
        console.log('FAIL')
        console.error(`  ${err.message}`)
      }
    }

    // Multi-object plate with a per-object "extruder" override (same value on
    // both objects, nozzle_diameter length 1) — the AMS-style path, probing
    // the onewasm_slice_stl_multi extruder_ids plumbing on its own. The genuinely
    // multi-nozzle case is the scenario below.
    const plateLabel = `[${mesh.label}] plate: 2 objects, per-object extruder override (single nozzle)`
    process.stdout.write(`[smoke-test] ${plateLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(BASE_CONFIG))
      const gcode = sliceMultiOnce(module, session, [mesh.bytes, mesh.bytes], Int32Array.from([1, 1]))
      assertSaneGcode(gcode, plateLabel)
      assertRestsOnBed(gcode, plateLabel, BASE_CONFIG.initial_layer_print_height)
      assertStatistics(getStatisticsOnce(module, session), plateLabel)
      console.log(`PASS (${gcode.length} bytes)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    // Real multi-nozzle: two objects on one plate, each assigned to a slot
    // that maps to a different physical nozzle. See DUAL_NOZZLE_CONFIG above
    // for why this scenario is committed rather than run by hand (#140).
    const dualLabel = `[${mesh.label}] plate: 2 objects on a real dual-nozzle machine (T0 + T1)`
    process.stdout.write(`[smoke-test] ${dualLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(DUAL_NOZZLE_CONFIG))
      const gcode = sliceMultiOnce(module, session, [mesh.bytes, mesh.bytes], Int32Array.from([1, 2]))
      assertSaneGcode(gcode, dualLabel)
      assertRestsOnBed(gcode, dualLabel, BASE_CONFIG.initial_layer_print_height)
      assertToolChanges(gcode, dualLabel)
      assertStatistics(getStatisticsOnce(module, session), dualLabel)
      console.log(`PASS (${gcode.length} bytes)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    // The host integration represents assigning one uploaded STL to slot 2 as a one-object
    // multi slice. Keep that exact shape covered: a two-object T0/T1 plate can
    // pass while the single-object path still collapses the selected filament.
    const singlePetgLabel = `[${mesh.label}] single object assigned to filament slot 2 (T1)`
    process.stdout.write(`[smoke-test] ${singlePetgLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(DUAL_NOZZLE_CONFIG))
      const gcode = sliceMultiOnce(module, session, [mesh.bytes], Int32Array.from([2]))
      assertSaneGcode(gcode, singlePetgLabel)
      if (!/; nozzle_temperature = 220,255\n/.test(gcode)) throw new Error('slot 2 assignment collapsed the per-filament nozzle temperatures')
      if (!/; nozzle_temperature_initial_layer = 220,255\n/.test(gcode)) throw new Error('slot 2 assignment collapsed the first-layer temperatures')
      if (!/(?:^|\n)T1(?:\s|$)/m.test(gcode)) throw new Error('slot 2 assignment did not select T1')
      assertStatistics(getStatisticsOnce(module, session), singlePetgLabel)
      console.log(`PASS (${gcode.length} bytes)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    // Prime tower placement: a multi-material plate on a shallow bed whose
    // engine-default tower position would hang off the back edge. Guards the
    // bridge's clamp (#163) — without it the tower origin sits at y=220 on a
    // 210 mm bed.
    const towerLabel = `[${mesh.label}] plate: prime tower clamped onto a shallow bed`
    process.stdout.write(`[smoke-test] ${towerLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(SMALL_BED_AMS_CONFIG))
      const gcode = sliceMultiOnce(module, session, [mesh.bytes, mesh.bytes], Int32Array.from([1, 2]))
      assertSaneGcode(gcode, towerLabel)
      assertToolChanges(gcode, towerLabel)
      assertWipeTowerOnBed(gcode, SMALL_BED_AMS_CONFIG.bed_size_x, SMALL_BED_AMS_CONFIG.bed_size_y, towerLabel)
      assertStatistics(getStatisticsOnce(module, session), towerLabel)
      console.log(`PASS (${gcode.length} bytes)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    // Mixed-temperature single-nozzle guard (#164): without the override the
    // engine must reject the plate with -6; with it the same plate slices. Both
    // halves matter — a bridge that ignored the flag would pass the second
    // check by never enforcing the guard, so the first check pins that the
    // guard is still on by default.
    const guardLabel = `[${mesh.label}] plate: mixed-temp single nozzle is rejected without the override`
    process.stdout.write(`[smoke-test] ${guardLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(MIXED_TEMP_AMS_CONFIG))
      let rejected = false
      try {
        sliceMultiOnce(module, session, [mesh.bytes, mesh.bytes], Int32Array.from([1, 2]))
      } catch (err) {
        rejected = true
        if (!/\(-6\)/.test(err.message) || !/incompatible/i.test(err.message)) {
          throw new Error(`expected an incompatible-temperature -6 rejection, got: ${err.message}`)
        }
        // The message must still carry the desktop menu path that
        // humanizeSliceError() (host loader) rewrites into the
        // in-app toggle. If a future engine reworded this tail, the host integration
        // rewrite would silently no-op — pin the anchor here so that drift is
        // caught at the engine boundary rather than in production (#164).
        if (!/Preferences\s*\/\s*Control\s*\/\s*Slicing\s*\/\s*Remove mixed temperature restriction/.test(err.message)) {
          throw new Error(`rejection message lost the desktop menu-path anchor humanizeSliceError keys on: ${err.message}`)
        }
      }
      if (!rejected) throw new Error('expected the slice to be rejected, but it succeeded')
      assertNoStatistics(module, session, `${guardLabel} after failed slice`)
      console.log('PASS (rejected as expected)')
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    const overrideLabel = `[${mesh.label}] plate: mixed-temp single nozzle slices with the override on`
    process.stdout.write(`[smoke-test] ${overrideLabel} ... `)
    try {
      initSession(module, session, JSON.stringify({ ...MIXED_TEMP_AMS_CONFIG, remove_mixed_temp_restriction: '1' }))
      const gcode = sliceMultiOnce(module, session, [mesh.bytes, mesh.bytes], Int32Array.from([1, 2]))
      assertSaneGcode(gcode, overrideLabel)
      assertToolChanges(gcode, overrideLabel)
      assertStatistics(getStatisticsOnce(module, session), overrideLabel)
      console.log(`PASS (${gcode.length} bytes)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    // onewasm_write_3mf: mesh + embedded config, no plate/gcode data (see
    // bridge/slicer.cpp).
    // Use the real dual-nozzle shape here so the read side also proves it
    // preserves vector boundaries instead of returning one joined scalar per
    // filament/nozzle option.
    const write3mfLabel = `[${mesh.label}] write .3mf (mesh + embedded config)`
    process.stdout.write(`[smoke-test] ${write3mfLabel} ... `)
    let written3mf = null
    try {
      initSession(module, session, JSON.stringify(DUAL_NOZZLE_CONFIG))
      written3mf = write3mfOnce(module, session, mesh.bytes)
      assertValid3mf(written3mf, write3mfLabel)
      console.log(`PASS (${written3mf.length} bytes)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    // onewasm_read_3mf: round-trip the .3mf just written back through the
    // engine's own reader. The common contract deliberately returns geometry
    // only; native settings are loaded through onewasm_init_profile.
    const read3mfLabel = `[${mesh.label}] read .3mf (round-trip geometry)`
    process.stdout.write(`[smoke-test] ${read3mfLabel} ... `)
    try {
      if (!written3mf) throw new Error('no .3mf available (write step failed above)')
      initProfileOnce(module, session, written3mf)
      const stl = read3mfOnce(module, written3mf)

      const expectedTris = stlTriangleCount(mesh.bytes)
      const actualTris = stlTriangleCount(stl)
      if (actualTris !== expectedTris) {
        throw new Error(`triangle count mismatch: expected ${expectedTris}, got ${actualTris}`)
      }

      console.log(`PASS (${actualTris} tris; geometry-only read contract)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }
  }

  if (supportsPlateActions) {
    const autoOrientLabel = '[current plate] auto-orient objects'
    process.stdout.write(`[smoke-test] ${autoOrientLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(BASE_CONFIG))
      const autoOrientMeshes = [meshes[0].bytes, meshes[0].bytes]
      const transforms = preparePlateOnce(module, session, autoOrientMeshes, 1)
      assertValidPlateTransforms(transforms, autoOrientMeshes.length, autoOrientLabel, false)
      const gcode = sliceMultiOnce(module, session, autoOrientMeshes, undefined, encodeObjectTransforms(transforms))
      assertSaneGcode(gcode, autoOrientLabel)
      assertRestsOnBed(gcode, autoOrientLabel, BASE_CONFIG.initial_layer_print_height)
      console.log(`PASS (${transforms.length} transforms)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    const arrangeLabel = '[current plate] arrange objects'
    process.stdout.write(`[smoke-test] ${arrangeLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(BASE_CONFIG))
      const transforms = preparePlateOnce(module, session, [meshes[0].bytes, meshes[0].bytes], 2)
      assertValidPlateTransforms(transforms, 2, arrangeLabel, true)
      const gcode = sliceMultiOnce(
        module,
        session,
        [meshes[0].bytes, meshes[0].bytes],
        undefined,
        encodeObjectTransforms(transforms),
      )
      assertSaneGcode(gcode, arrangeLabel)
      assertRestsOnBed(gcode, arrangeLabel, BASE_CONFIG.initial_layer_print_height)
      console.log(`PASS (${transforms.length} transforms)`)
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }
  }

  if (supportsProjectApi) {
    const projectLabel = '[draft 0.3] project manifest, prepare, all/selected plate slicing'
    process.stdout.write(`[smoke-test] ${projectLabel} ... `)
    try {
      initSession(module, session, JSON.stringify(BASE_CONFIG))
      // Keep this optional draft probe cheap: the regular smoke cases already
      // exercise the larger torture mesh. The project path itself still uses
      // two real native slices, one per logical plate.
      runProjectSmoke(module, session, sphereStl(1, 5))
      console.log('PASS')
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }
  }

  module._onewasm_session_destroy(session)

  if (failures > 0) {
    console.error(`\n[smoke-test] ${failures} scenario(s) failed`)
    process.exit(1)
  }
  console.log('\n[smoke-test] all scenarios passed')
}

main().catch((err) => {
  console.error('[smoke-test] fatal:', err.stack ?? err)
  process.exit(1)
})
