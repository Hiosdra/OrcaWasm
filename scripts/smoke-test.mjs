#!/usr/bin/env node
/**
 * WASM engine smoke test.
 *
 * Loads the built slicer.js/slicer.wasm and runs the stable project contract
 * end-to-end under API 0.5: onewasm_init, profile application, project
 * manifest/prepare/slice/assets, project export, and the geometry-only 3MF
 * import helper. The test catches broken builds before they are published as a GitHub Release
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
  sphereStl, trianglesToStl, loadModule, writeBytes, decodeError,
  initSession, applyProfileOnce,
  projectSetObjectsOnce, projectGetManifestOnce, projectPrepareOnce,
  projectSliceOnce, projectGetAssetOnce, projectExportOnce, checkedMalloc, free,
} from './lib/engine-harness.mjs'

const VORON_PROFILE_FIXTURE = JSON.parse(
  readFileSync(new URL('./fixtures/voron-0.4-profile-smoke.json', import.meta.url), 'utf8'),
)

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
      if (capabilities.api?.name !== 'one-wasm-slicer-api' || capabilities.api?.version !== '0.5.0') {
        throw new Error('capabilities document does not identify one-wasm-slicer-api 0.5.0')
      }
      if (!capabilities.project?.nativeProjectFormats?.includes('project.3mf')) {
        throw new Error('capabilities document does not advertise native project.3mf support')
      }
      const requiredFeatures = [
        'core.session',
        'core.configuration',
        'config.fullProfile',
        'config.profileApply',
        'project.manifest',
        'project.modifierVolumes',
        'project.prepare',
        'project.slice',
        'project.slice.multiPlate',
        'project.slice.assets',
        'project.export',
        'project.export.preservation',
        'format.objToStl',
        'format.stepToStl',
        'runtime.capabilities',
        'runtime.progress',
        'runtime.cancellation',
        'runtime.errors',
        'runtime.memory',
      ]
      for (const feature of requiredFeatures) {
        if (capabilities.features?.[feature] !== 'supported') {
          throw new Error(`capabilities document does not mark ${feature} as supported`)
        }
      }
      if (capabilities.features?.['project.modifierVolumes.nativeProject'] !== 'supported') {
        throw new Error('capabilities document must mark native-project modifier volumes as supported')
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
// loadModule() + the onewasm_* heap marshaling live in ./lib/engine-harness.mjs.

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
        <!-- ST_Matrix3D: row-major affine matrix; translation is the final three values. -->
        <component objectid="1" transform="1 0 0 0 1 0 0 0 1 5 0 0"/>
      </components>
    </object>
  </resources>
  <build><item objectid="2" transform="1 0 0 0 1 0 0 0 1 10 0 0"/></build>
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

// A .3mf is a ZIP; verify that project_export returns a native package with
// the mesh (3D/3dmodel.model) and embedded OrcaSlicer settings (a
// Metadata/*.config file — see
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

function makeBoxStl(size) {
  const [hx, hy, hz] = size.map((value) => value / 2)
  const vertices = [
    [-hx, -hy, -hz], [hx, -hy, -hz], [hx, hy, -hz], [-hx, hy, -hz],
    [-hx, -hy, hz], [hx, -hy, hz], [hx, hy, hz], [-hx, hy, hz],
  ]
  const faces = [
    [1, 2, 6], [1, 6, 5],
    [0, 4, 7], [0, 7, 3],
    [3, 7, 6], [3, 6, 2],
    [0, 1, 5], [0, 5, 4],
    [4, 5, 6], [4, 6, 7],
    [0, 3, 2], [0, 2, 1],
  ]
  return trianglesToStl(vertices, faces)
}

function makeSupportModifierProject(meshBytes, modifier) {
  const modifierBytes = modifier ? makeBoxStl(modifier.size) : null
  const blob = new Uint8Array(meshBytes.length + (modifierBytes?.length ?? 0))
  blob.set(meshBytes)
  if (modifierBytes) blob.set(modifierBytes, meshBytes.length)
  const meshes = [{
    id: 'mesh-model',
    format: 'stl',
    dataRange: { offset: 0, length: meshBytes.length },
  }]
  if (modifierBytes) {
    meshes.push({
      id: 'mesh-modifier',
      format: 'stl',
      dataRange: { offset: meshBytes.length, length: modifierBytes.length },
    })
  }
  return {
    blob,
    manifest: {
      schemaVersion: '0.5',
      plates: [{ id: 'plate-0', index: 0 }],
      meshes,
      objects: [{ id: 'object-model', meshId: 'mesh-model' }],
      instances: [{
        id: 'instance-model',
        objectId: 'object-model',
        plateId: 'plate-0',
        transform: { matrix: projectMatrix(128, 128) },
      }],
      modifierVolumes: modifier ? [{
        id: 'modifier-' + modifier.role,
        meshId: 'mesh-modifier',
        role: modifier.role,
        objectId: 'object-model',
        plateId: 'plate-0',
        transform: { matrix: projectMatrix(...modifier.position) },
      }] : [],
    },
  }
}

function expectProjectValidationFailure(module, session, project, message, label) {
  let failure = null
  try {
    projectSetObjectsOnce(module, session, project.blob, project.manifest)
  } catch (error) {
    failure = error
  }
  if (!failure || !String(failure.message).includes(message)) {
    throw new Error(label + ': expected validation error containing "' + message + '", got '
      + (failure?.message ?? 'success'))
  }
}

function sliceSupportProject(module, session, label) {
  const result = projectSliceOnce(module, session, {
    schemaVersion: '0.3',
    plateSelection: 'selected',
    plateIds: ['plate-0'],
    includeGcode: true,
    includeStatistics: false,
  })
  const plate = result.plateResults?.[0]
  const asset = plate?.assets?.find((entry) => entry.kind === 'gcode')
  if (result.schemaVersion !== '0.3' || plate?.plateId !== 'plate-0' || !asset) {
    throw new Error(label + ': modifier test did not return one plate G-code asset')
  }
  const gcode = new TextDecoder().decode(projectGetAssetOnce(module, session, asset.id))
  assertSaneGcode(gcode, label)
  return gcode
}

function expectModifierNativeProjectRoundTrip(module, session, role) {
  const result = projectExportOnce(module, session, 'project.3mf', {
    schemaVersion: '0.3',
    preservation: 'portable',
    includeSliceArtifacts: false,
  })
  if (result.schemaVersion !== '0.3' || result.asset?.kind !== 'project') {
    throw new Error('modifier project export returned an invalid asset descriptor')
  }
  const bytes = projectGetAssetOnce(module, session, result.asset.id)
  if (bytes.length !== result.asset.byteLength) {
    throw new Error('modifier project export returned an incorrect byte length')
  }
  initProfileOnce(module, session, bytes)
  const imported = projectGetManifestOnce(module, session)
  const volume = imported.modifierVolumes?.find((entry) => entry.role === role)
  if (imported.schemaVersion !== '0.5'
    || imported.modifierVolumes?.length !== 1
    || !volume
    || volume.objectId !== 'native-object-0'
    || volume.plateId !== 'plate-0') {
    throw new Error(role + ': native 3MF export/import did not preserve the modifier role and association')
  }
  const expectedZ = role === 'support-enforcer' ? 16 : 6
  if (Math.abs(volume.transform.matrix[11] - expectedZ) > 0.01) {
    throw new Error(role + ': native 3MF export/import changed the modifier transform')
  }
}

function runSupportModifierSmoke(module, session, configureSession = () => {
  initSession(module, session, JSON.stringify({
    ...BASE_CONFIG,
    support_type: 'normal(auto)',
    support_threshold_angle: 45,
  }))
}, assertSupportOutputChange = true) {
  // The low-resolution sphere has a printable underside and upper surface:
  // a blocker removes automatically generated supports near the bed, while an
  // enforcer requests supports on an upper area that automatic support omits.
  const meshBytes = sphereStl(2, 10)
  configureSession()

  const baseline = makeSupportModifierProject(meshBytes, null)
  projectSetObjectsOnce(module, session, baseline.blob, baseline.manifest)
  const baselineRoundTrip = projectGetManifestOnce(module, session)
  if (baselineRoundTrip.schemaVersion !== '0.5' || baselineRoundTrip.modifierVolumes?.length !== 0) {
    throw new Error('API 0.5 empty modifierVolumes manifest did not round-trip')
  }
  const automaticSupports = sliceSupportProject(module, session, 'automatic support baseline')

  const invalidTransform = makeSupportModifierProject(meshBytes, {
    role: 'support-enforcer',
    position: [0, 0, 16],
    size: [8, 8, 8],
  })
  invalidTransform.manifest.modifierVolumes[0].transform.matrix[3] = null
  expectProjectValidationFailure(
    module,
    session,
    invalidTransform,
    'modifierVolume.transform.matrix must be a number',
    'modifier transform validation',
  )

  const cases = [
    {
      role: 'support-enforcer',
      position: [9, 0, 16],
      size: [8, 8, 8],
    },
    {
      role: 'support-blocker',
      position: [0, 0, 6],
      size: [16, 16, 12],
    },
  ]
  for (const modifier of cases) {
    const project = makeSupportModifierProject(meshBytes, modifier)
    projectSetObjectsOnce(module, session, project.blob, project.manifest)
    const roundTrip = projectGetManifestOnce(module, session)
    const returned = roundTrip.modifierVolumes?.[0]
    if (
      roundTrip.schemaVersion !== '0.5' ||
      roundTrip.modifierVolumes?.length !== 1 ||
      !returned ||
      returned.id !== 'modifier-' + modifier.role ||
      returned.role !== modifier.role ||
      returned.objectId !== 'object-model' ||
      returned.plateId !== 'plate-0' ||
      JSON.stringify(returned.transform.matrix) !== JSON.stringify(project.manifest.modifierVolumes[0].transform.matrix)
    ) {
      throw new Error(modifier.role + ' geometry, association, or transform did not round-trip')
    }

    const modifiedSupports = sliceSupportProject(module, session, modifier.role)
    const supportOutputChanged = modifiedSupports !== automaticSupports
    if (assertSupportOutputChange && !supportOutputChanged) {
      throw new Error(modifier.role + ' did not change support output from the automatic-support baseline')
    }
    expectModifierNativeProjectRoundTrip(module, session, modifier.role)
    const sliceAssertion = assertSupportOutputChange
      ? 'support output changed'
      : 'profile-configured slice succeeded'
    console.log('PASS (' + modifier.role + ' round-trip and ' + sliceAssertion + ')')
  }
}

function makeSelectedFilamentFragment(filaments) {
  const metadata = new Set([
    'compatible_printers',
    'compatible_printers_condition',
    'filament_settings_id',
    'id',
    'inherits',
    'name',
    'type',
    'version',
  ])
  const keys = new Set(filaments.flatMap((profile) => Object.keys(profile)))
  const fragment = {}
  for (const key of keys) {
    if (metadata.has(key)) continue
    const values = filaments.map((profile) => {
      const value = profile[key]
      if (Array.isArray(value)) {
        if (value.length !== 1) {
          throw new Error(`Voron smoke fixture expected one selected-slot value for ${key}`)
        }
        return value[0]
      }
      return value
    })
    if (values.some((value) => value === undefined || value === null || value === '')) continue
    fragment[key] = values
  }
  return fragment
}

function applyVoronProfile(module, session) {
  applyProfileOnce(module, session, VORON_PROFILE_FIXTURE.machine)
  applyProfileOnce(module, session, VORON_PROFILE_FIXTURE.process)
  applyProfileOnce(module, session, makeSelectedFilamentFragment(VORON_PROFILE_FIXTURE.filaments))

  // The profile-set format does not include the user's currently selected
  // physical plate. Pick a test-only surface that has explicit temperatures
  // in both selected material presets; this is test setup, not a hardware
  // recommendation or an application-side auto-selection. Enable supports
  // because the selected process omits support_enable, but retain its tree
  // strategy and all other support settings.
  applyProfileOnce(module, session, {
    curr_bed_type: 'Textured PEI Plate',
    support_enable: 1,
  }, 'orca.native-json')
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
  return exported
}

// A deliberately asymmetric triangular prism keeps transform regressions
// cheap while making each axis observable in the real sliced toolpaths. Its
// base is offset from the origin and its unequal dimensions expose axis swaps.
const TRANSFORM_VERTICES = [
  [20, 24, 0],
  [44, 24, 0],
  [44, 36, 0],
  [20, 24, 8],
  [44, 24, 8],
  [44, 32, 8],
]
const TRANSFORM_FACES = [
  [0, 2, 1],
  [3, 4, 5],
  [0, 1, 4],
  [0, 4, 3],
  [1, 2, 5],
  [1, 5, 4],
  [2, 0, 3],
  [2, 3, 5],
]
const TRANSFORM_MESH = trianglesToStl(TRANSFORM_VERTICES, TRANSFORM_FACES)
const TRANSFORM_CONFIG = {
  ...BASE_CONFIG,
  sparse_infill_density: 0,
  skirt_loops: 0,
  brim_type: 'no_brim',
  support_enable: 0,
}

function projectTransformMatrix({
  scale = [1, 1, 1],
  rotation = [0, 0, 0],
  mirror = [1, 1, 1],
  translation = [0, 0, 0],
} = {}) {
  const [sx, sy, sz] = scale
  const [rx, ry, rz] = rotation
  const [mx, my, mz] = mirror
  const [tx, ty, tz] = translation
  const cx = Math.cos(rx), sxr = Math.sin(rx)
  const cy = Math.cos(ry), syr = Math.sin(ry)
  const cz = Math.cos(rz), szr = Math.sin(rz)

  // Rz * Ry * Rx, with signed non-uniform scale applied to each column.
  const linear = [
    cz * cy, cz * syr * sxr - szr * cx, cz * syr * cx + szr * sxr,
    szr * cy, szr * syr * sxr + cz * cx, szr * syr * cx - cz * sxr,
    -syr, cy * sxr, cy * cx,
  ]
  const signedScale = [sx * mx, sy * my, sz * mz]
  return [
    linear[0] * signedScale[0], linear[1] * signedScale[1], linear[2] * signedScale[2], tx,
    linear[3] * signedScale[0], linear[4] * signedScale[1], linear[5] * signedScale[2], ty,
    linear[6] * signedScale[0], linear[7] * signedScale[1], linear[8] * signedScale[2], tz,
    0, 0, 0, 1,
  ]
}

function transformedBounds(vertices, matrix) {
  const points = vertices.map(([x, y, z]) => [
    matrix[0] * x + matrix[1] * y + matrix[2] * z + matrix[3],
    matrix[4] * x + matrix[5] * y + matrix[6] * z + matrix[7],
    matrix[8] * x + matrix[9] * y + matrix[10] * z + matrix[11],
  ])
  return boundsOfPoints(points)
}

function boundsOfPoints(points) {
  const xs = points.map((point) => point[0])
  const ys = points.map((point) => point[1])
  const zs = points.map((point) => point[2])
  return {
    minX: Math.min(...xs), maxX: Math.max(...xs),
    minY: Math.min(...ys), maxY: Math.max(...ys),
    minZ: Math.min(...zs), maxZ: Math.max(...zs),
  }
}

function boundsUnion(boundsList) {
  return {
    minX: Math.min(...boundsList.map((bounds) => bounds.minX)),
    maxX: Math.max(...boundsList.map((bounds) => bounds.maxX)),
    minY: Math.min(...boundsList.map((bounds) => bounds.minY)),
    maxY: Math.max(...boundsList.map((bounds) => bounds.maxY)),
    minZ: Math.min(...boundsList.map((bounds) => bounds.minZ)),
    maxZ: Math.max(...boundsList.map((bounds) => bounds.maxZ)),
  }
}

function assertBoundsNear(actual, expected, tolerance, label) {
  for (const key of ['minX', 'maxX', 'minY', 'maxY', 'minZ', 'maxZ']) {
    if (Math.abs(actual[key] - expected[key]) > tolerance) {
      throw new Error(label + ': ' + key + ' ' + actual[key].toFixed(2)
        + ' is not near transformed mesh bound ' + expected[key].toFixed(2))
    }
  }
}

function assertXYBoundsChanged(actual, reference, label) {
  const changed = ['minX', 'maxX', 'minY', 'maxY']
    .some((key) => Math.abs(actual[key] - reference[key]) > 0.5)
  if (!changed) throw new Error(label + ': XY sliced footprint did not change')
}

function extractExtrusionGeometry(gcode, label) {
  const position = { x: 0, y: 0, z: 0, e: 0 }
  let absoluteXYZ = true
  let absoluteE = true
  const points = []

  for (const sourceLine of gcode.split('\n')) {
    const line = sourceLine.split(';', 1)[0].trim()
    if (!line) continue
    const command = line.match(/^(G\d+|M\d+)/)?.[1]
    if (command === 'G90') { absoluteXYZ = true; continue }
    if (command === 'G91') { absoluteXYZ = false; continue }
    if (command === 'M82') { absoluteE = true; continue }
    if (command === 'M83') { absoluteE = false; continue }

    const params = {}
    for (const match of line.matchAll(/(?:^|\s)([XYZE])([-+]?(?:\d+(?:\.\d*)?|\.\d+))/g)) {
      params[match[1].toLowerCase()] = Number(match[2])
    }
    if (command === 'G92') {
      for (const axis of ['x', 'y', 'z', 'e']) {
        if (axis in params) position[axis] = params[axis]
      }
      continue
    }
    if (command !== 'G0' && command !== 'G1') continue

    const before = { ...position }
    for (const axis of ['x', 'y', 'z']) {
      if (axis in params) {
        position[axis] = absoluteXYZ ? params[axis] : position[axis] + params[axis]
      }
    }
    let extrusionDelta = 0
    if ('e' in params) {
      extrusionDelta = absoluteE ? params.e - position.e : params.e
      position.e = absoluteE ? params.e : position.e + params.e
    }
    if (command === 'G1' && extrusionDelta > 1e-5 && ('x' in params || 'y' in params)) {
      points.push([before.x, before.y, before.z], [position.x, position.y, position.z])
    }
  }

  if (points.length < 10) throw new Error(label + ': no useful extruded XY toolpaths found')
  const layers = new Map()
  for (const point of points) {
    const z = Math.round(point[2] * 100) / 100
    if (!layers.has(z)) layers.set(z, [])
    layers.get(z).push(point)
  }
  const layerFootprints = [...layers.entries()]
    .sort(([a], [b]) => a - b)
    .map(([z, layerPoints]) => [z, boundsOfPoints(layerPoints)])
  return {
    points,
    bounds: boundsOfPoints(points),
    trace: JSON.stringify(points.map((point) => point.map((value) => Math.round(value * 100) / 100))),
    layerFootprints,
  }
}

function makeTransformProjectManifest(meshBytes, matrices) {
  return {
    schemaVersion: '0.3',
    plates: [{ id: 'transform-plate', label: 'Transform plate', index: 0 }],
    meshes: [{
      id: 'transform-mesh',
      format: 'stl',
      dataRange: { offset: 0, length: meshBytes.length },
    }],
    objects: [{ id: 'asymmetric-object', meshId: 'transform-mesh', extruderId: 0 }],
    instances: matrices.map((matrix, index) => ({
      id: 'transform-instance-' + index,
      objectId: 'asymmetric-object',
      plateId: 'transform-plate',
      transform: { matrix },
    })),
  }
}

function sliceTransformManifest(module, session, meshBytes, manifest, label) {
  try {
    projectSetObjectsOnce(module, session, new Uint8Array(meshBytes), manifest)
  } catch (error) {
    throw new Error(label + ': ' + error.message)
  }
  let result
  try {
    result = projectSliceOnce(module, session, {
      schemaVersion: '0.3',
      plateSelection: 'selected',
      plateIds: ['transform-plate'],
      includeGcode: true,
      includeStatistics: false,
    })
  } catch (error) {
    throw new Error(label + ': ' + error.message)
  }
  const plate = result?.plateResults?.[0]
  if (plate?.plateId !== 'transform-plate' || plate.assets?.length !== 1
    || plate.assets[0].id !== 'gcode:transform-plate') {
    throw new Error(label + ': API 0.3 did not return the transform plate G-code asset')
  }
  const gcode = new TextDecoder().decode(projectGetAssetOnce(module, session, 'gcode:transform-plate'))
  assertSaneGcode(gcode, label)
  return { manifest, gcode, geometry: extractExtrusionGeometry(gcode, label) }
}

function sliceTransformMatrices(module, session, meshBytes, matrices, label) {
  return sliceTransformManifest(
    module, session, meshBytes, makeTransformProjectManifest(meshBytes, matrices), label,
  )
}

function assertExpectedFailure(label, operation, expectedMessage) {
  let failure = null
  try {
    operation()
  } catch (error) {
    failure = error
  }
  if (!failure) throw new Error(label + ': invalid input unexpectedly succeeded')
  if (!expectedMessage.test(failure.message)) {
    throw new Error(label + ': rejected for an unexpected reason: ' + failure.message)
  }
}

function assertInsideBed(bounds, label) {
  const epsilon = 0.05
  if (bounds.minX < -epsilon || bounds.minY < -epsilon
    || bounds.maxX > BASE_CONFIG.bed_size_x + epsilon
    || bounds.maxY > BASE_CONFIG.bed_size_y + epsilon) {
    throw new Error(label + ': sliced footprint ' + JSON.stringify(bounds)
      + ' is outside the ' + BASE_CONFIG.bed_size_x + 'x' + BASE_CONFIG.bed_size_y + ' bed')
  }
}

function runTransformRegressionSmoke(module, session) {
  initSession(module, session, JSON.stringify(TRANSFORM_CONFIG))

  const identityMatrix = projectTransformMatrix()
  const identity = sliceTransformMatrices(
    module, session, TRANSFORM_MESH, [identityMatrix], 'identity transform',
  )
  assertBoundsNear(
    identity.geometry.bounds,
    transformedBounds(TRANSFORM_VERTICES, identityMatrix),
    2.5,
    'identity transform',
  )
  assertInsideBed(identity.geometry.bounds, 'identity transform')

  const placement = [80, 80, 0]
  const referenceMatrix = projectTransformMatrix({ translation: placement })
  const reference = sliceTransformMatrices(
    module, session, TRANSFORM_MESH, [referenceMatrix], 'identity linear transform',
  )
  assertBoundsNear(
    reference.geometry.bounds,
    transformedBounds(TRANSFORM_VERTICES, referenceMatrix),
    2.5,
    'identity linear transform',
  )

  const scenarioDefinitions = [
    ['non-uniform scale', { scale: [1.45, 0.7, 1.25] }, true],
    ['X rotation', { rotation: [Math.PI / 2, 0, 0] }, true],
    ['Y rotation', { rotation: [0, Math.PI / 2, 0] }, true],
    ['Z rotation', { rotation: [0, 0, 0.47] }, true],
    ['X mirror', { mirror: [-1, 1, 1] }, true],
    ['Y mirror', { mirror: [1, -1, 1] }, true],
    ['Z mirror', { mirror: [1, 1, -1] }, false],
  ]
  for (const [label, components, expectXYBoundsChange] of scenarioDefinitions) {
    const options = {
      ...components,
      translation: placement,
    }
    let matrix = projectTransformMatrix(options)
    const beforeBed = transformedBounds(TRANSFORM_VERTICES, matrix)
    if (Math.abs(beforeBed.minZ) > 1e-6) {
      options.translation = [placement[0], placement[1], -beforeBed.minZ]
      matrix = projectTransformMatrix(options)
    }
    const sliced = sliceTransformMatrices(module, session, TRANSFORM_MESH, [matrix], label)
    if (sliced.geometry.trace === reference.geometry.trace) {
      throw new Error(label + ': sliced extrusion paths are identical to the identity transform')
    }
    if (expectXYBoundsChange) {
      assertXYBoundsChanged(sliced.geometry.bounds, reference.geometry.bounds, label)
    } else if (JSON.stringify(sliced.geometry.layerFootprints) === JSON.stringify(reference.geometry.layerFootprints)) {
      throw new Error(label + ': per-layer XY footprint did not change')
    }
    assertBoundsNear(
      sliced.geometry.bounds,
      transformedBounds(TRANSFORM_VERTICES, matrix),
      2.5,
      label,
    )
    assertInsideBed(sliced.geometry.bounds, label)
  }

  const placedMatrix = projectTransformMatrix({ translation: [64, 72, 0] })
  const placed = sliceTransformMatrices(
    module, session, TRANSFORM_MESH, [placedMatrix], 'finite bed-relative placement',
  )
  assertBoundsNear(
    placed.geometry.bounds,
    transformedBounds(TRANSFORM_VERTICES, placedMatrix),
    2.5,
    'finite bed-relative placement',
  )
  assertInsideBed(placed.geometry.bounds, 'finite bed-relative placement')

  const secondInstanceMatrix = projectTransformMatrix({
    scale: [1.35, 0.8, 1.2],
    rotation: [0, 0, 0.34],
    translation: [150, 130, 0],
  })
  const multipleManifest = makeTransformProjectManifest(
    TRANSFORM_MESH, [projectTransformMatrix({ translation: [40, 45, 0] }), secondInstanceMatrix],
  )
  if (multipleManifest.objects.length !== 1 || multipleManifest.instances.length !== 2
    || JSON.stringify(multipleManifest.instances[0].transform.matrix)
      === JSON.stringify(multipleManifest.instances[1].transform.matrix)) {
    throw new Error('same-object multi-instance fixture does not carry two distinct transforms')
  }
  const multiple = sliceTransformManifest(
    module, session, TRANSFORM_MESH, multipleManifest, 'same-object instances with separate transforms',
  )
  const expectedInstanceBounds = multipleManifest.instances.map((instance) =>
    transformedBounds(TRANSFORM_VERTICES, instance.transform.matrix))
  for (const [index, expected] of expectedInstanceBounds.entries()) {
    const nearbyPoints = multiple.geometry.points.filter((point) =>
      point[0] >= expected.minX - 3 && point[0] <= expected.maxX + 3
      && point[1] >= expected.minY - 3 && point[1] <= expected.maxY + 3)
    if (nearbyPoints.length < 10) {
      throw new Error('same-object instances: transform ' + index + ' has no corresponding sliced footprint')
    }
    assertBoundsNear(
      boundsOfPoints(nearbyPoints),
      expected,
      2.5,
      'same-object instance ' + index,
    )
  }

  // Keep the source mesh offset from the origin so the prepared matrices are
  // checked against the original project coordinates, including the shift the
  // native arrangement helper uses while centring its temporary model.
  const arrangeMatrices = [
    projectTransformMatrix(),
    projectTransformMatrix({ scale: [1.8, 0.72, 1.3] }),
  ]
  const arrangeManifest = makeTransformProjectManifest(TRANSFORM_MESH, arrangeMatrices)
  projectSetObjectsOnce(module, session, new Uint8Array(TRANSFORM_MESH), arrangeManifest)
  const arrangedManifest = projectPrepareOnce(module, session, {
    schemaVersion: '0.3',
    operation: 'arrange',
  })
  if (!Array.isArray(arrangedManifest?.instances) || arrangedManifest.instances.length !== 2) {
    throw new Error('arrange: expected two prepared instances of one object')
  }
  const arrangedBounds = arrangedManifest.instances.map((instance, index) => {
    const matrix = instance.transform?.matrix
    if (!Array.isArray(matrix) || matrix.length !== 16 || !matrix.every(Number.isFinite)) {
      throw new Error('arrange: instance ' + index + ' has no finite project transform')
    }
    return transformedBounds(TRANSFORM_VERTICES, matrix)
  })
  const [firstArranged, secondArranged] = arrangedBounds
  const spacingTolerance = 0.05
  const separated = firstArranged.maxX <= secondArranged.minX + spacingTolerance
    || secondArranged.maxX <= firstArranged.minX + spacingTolerance
    || firstArranged.maxY <= secondArranged.minY + spacingTolerance
    || secondArranged.maxY <= firstArranged.minY + spacingTolerance
  if (!separated) throw new Error('arrange: transformed instance footprints overlap')
  if (secondArranged.maxX - secondArranged.minX
      <= firstArranged.maxX - firstArranged.minX + 5) {
    throw new Error('arrange: the larger transformed instance did not retain its wider footprint')
  }
  for (const [index, bounds] of arrangedBounds.entries()) {
    assertInsideBed(bounds, 'arrange instance ' + index)
  }
  const arranged = sliceTransformManifest(
    module, session, TRANSFORM_MESH, arrangedManifest, 'arranged transformed footprints',
  )
  assertBoundsNear(
    arranged.geometry.bounds,
    boundsUnion(arrangedBounds),
    2.5,
    'arranged transformed footprints',
  )

  const invalidMatrix = projectTransformMatrix({ translation: [Number.NaN, 72, 0] })
  assertExpectedFailure(
    'non-finite XY placement',
    () => projectSetObjectsOnce(
      module,
      session,
      new Uint8Array(TRANSFORM_MESH),
      makeTransformProjectManifest(TRANSFORM_MESH, [invalidMatrix]),
    ),
    /number|finite|matrix/i,
  )
  const offBedMatrix = projectTransformMatrix({ translation: [300, 300, 0] })
  projectSetObjectsOnce(
    module,
    session,
    new Uint8Array(TRANSFORM_MESH),
    makeTransformProjectManifest(TRANSFORM_MESH, [offBedMatrix]),
  )
  assertExpectedFailure(
    'off-bed XY placement',
    () => projectSliceOnce(module, session, {
      schemaVersion: '0.3',
      plateSelection: 'selected',
      plateIds: ['transform-plate'],
      includeGcode: true,
      includeStatistics: false,
    }),
    /bed|outside|printable/i,
  )

  initSession(module, session, JSON.stringify({ ...TRANSFORM_CONFIG, bed_shape: 'circle' }))
  const outsideCircleMatrix = projectTransformMatrix({ translation: [184, 184, 0] })
  projectSetObjectsOnce(
    module,
    session,
    new Uint8Array(TRANSFORM_MESH),
    makeTransformProjectManifest(TRANSFORM_MESH, [outsideCircleMatrix]),
  )
  assertExpectedFailure(
    'XY placement outside circular bed printable area',
    () => projectSliceOnce(module, session, {
      schemaVersion: '0.3',
      plateSelection: 'selected',
      plateIds: ['transform-plate'],
      includeGcode: true,
      includeStatistics: false,
    }),
    /bed|outside|printable/i,
  )
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

  // The active smoke path is intentionally strict: a published artifact must
  // expose the complete promoted 0.5 surface. The old helper scenarios below
  // are unreachable legacy text kept temporarily while their historical
  // assertions are retired; they must never be used to validate a release.
  const requiredApiExports = [
    '_onewasm_session_create',
    '_onewasm_session_destroy',
    '_onewasm_init',
    '_onewasm_init_profile',
    '_onewasm_apply_profile',
    '_onewasm_set_progress_callback',
    '_onewasm_cancel',
    '_onewasm_project_set_objects',
    '_onewasm_project_get_manifest',
    '_onewasm_project_prepare',
    '_onewasm_project_slice',
    '_onewasm_project_get_asset',
    '_onewasm_project_export',
    '_onewasm_obj_to_stl',
    '_onewasm_cad_to_stl',
    '_onewasm_read_3mf',
    '_onewasm_get_capabilities',
    '_onewasm_last_error',
    '_onewasm_free',
  ]
  for (const name of requiredApiExports) {
    if (typeof module[name] !== 'function') {
      throw new Error(`loaded engine is missing required 0.5 export ${name}`)
    }
  }

  const stableSession = module._onewasm_session_create()
  if (!stableSession) throw new Error('onewasm_session_create failed (allocation failure)')
  let stableFailures = 0
  try {
    if (module._onewasm_cancel(stableSession) !== 0) {
      throw new Error('idle onewasm_cancel was not a no-op')
    }

    const stableComponentLabel = '[3MF] component graph with composed transforms'
    process.stdout.write(`[smoke-test] ${stableComponentLabel} ... `)
    try {
      assertComponent3mfRead(module)
      console.log('PASS (one flattened triangle at x=15..16)')
    } catch (err) {
      stableFailures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }

    for (const mesh of meshes) {
      const stableProjectLabel = `[${mesh.label}] API 0.3 project manifest, prepare, all/selected plate slicing`
      let stableExportedProject = null
      process.stdout.write(`[smoke-test] ${stableProjectLabel} ... `)
      try {
        initSession(module, stableSession, JSON.stringify(BASE_CONFIG))
        stableExportedProject = runProjectSmoke(module, stableSession, mesh.bytes)
        console.log('PASS')
      } catch (err) {
        stableFailures++
        console.log('FAIL')
        console.error(`  ${err.message}`)
      }

      const stableProfileLabel = `[${mesh.label}] API 0.3 native project profile load/export`
      process.stdout.write(`[smoke-test] ${stableProfileLabel} ... `)
      try {
        if (!stableExportedProject) throw new Error('project export did not produce a profile package')
        initProfileOnce(module, stableSession, stableExportedProject)
        const loadedManifest = projectGetManifestOnce(module, stableSession)
        if (loadedManifest?.schemaVersion !== '0.3'
          || !Array.isArray(loadedManifest.plates)
          || !Array.isArray(loadedManifest.meshes)
          || !Array.isArray(loadedManifest.objects)
          || !Array.isArray(loadedManifest.instances)
          || loadedManifest.instances.length === 0) {
          throw new Error('native project profile did not expose a complete 0.3 manifest')
        }
        const passthrough = projectExportOnce(module, stableSession, 'project.3mf', {
          schemaVersion: '0.3',
          preservation: 'require',
          includeSliceArtifacts: false,
        })
        if (passthrough?.schemaVersion !== '0.3'
          || passthrough.asset?.kind !== 'project'
          || passthrough.asset?.byteLength !== stableExportedProject.length) {
          throw new Error('native project profile did not support preservation=require export')
        }
        const roundTripped = projectGetAssetOnce(module, stableSession, 'project:export')
        if (roundTripped.length !== stableExportedProject.length
          || !roundTripped.every((value, index) => value === stableExportedProject[index])) {
          throw new Error('preservation=require export changed the untouched native project package')
        }
        console.log(`PASS (${loadedManifest.instances.length} native instance(s))`)
      } catch (err) {
        stableFailures++
        console.log('FAIL')
        console.error(`  ${err.message}`)
      }

      const stableReadLabel = `[${mesh.label}] read .3mf (geometry-only import helper)`
      process.stdout.write(`[smoke-test] ${stableReadLabel} ... `)
      try {
        if (!stableExportedProject) throw new Error('project export did not produce a 3MF package')
        const stl = read3mfOnce(module, stableExportedProject)
        // The project fixture deliberately contains one instance on each of
        // two logical plates. project_export flattens those plates into one
        // native build, and read_3mf returns the merged geometry of every
        // exported build item.
        const expectedTris = stlTriangleCount(mesh.bytes) * 2
        const actualTris = stlTriangleCount(stl)
        if (actualTris !== expectedTris) {
          throw new Error(`triangle count mismatch: expected ${expectedTris}, got ${actualTris}`)
        }
        console.log(`PASS (${actualTris} tris)`)
      } catch (err) {
        stableFailures++
        console.log('FAIL')
        console.error(`  ${err.message}`)
      }
    }

    const transformLabel = 'API 0.3 asymmetric object-transform and placement regression coverage'
    process.stdout.write('[smoke-test] ' + transformLabel + ' ... ')
    try {
      runTransformRegressionSmoke(module, stableSession)
      console.log('PASS (geometry, placement, mirroring, arrangement, multi-instance)')
    } catch (err) {
      stableFailures++
      console.log('FAIL')
      console.error('  ' + err.message)
    }

    process.stdout.write('[smoke-test] API 0.5 support modifier round-trip, validation, slicing, and export boundary ... ')
    const modifierSession = module._onewasm_session_create()
    if (!modifierSession) throw new Error('onewasm_session_create failed for modifier smoke test')
    try {
      runSupportModifierSmoke(module, modifierSession)
      console.log('PASS')
    } catch (err) {
      stableFailures++
      console.log('FAIL')
      console.error('  ' + err.message)
    } finally {
      module._onewasm_session_destroy(modifierSession)
    }

    process.stdout.write('[smoke-test] API 0.5 support modifiers with the user Voron 0.4 profile ... ')
    const voronModifierSession = module._onewasm_session_create()
    if (!voronModifierSession) throw new Error('onewasm_session_create failed for Voron profile modifier smoke test')
    try {
      runSupportModifierSmoke(
        module,
        voronModifierSession,
        () => applyVoronProfile(module, voronModifierSession),
        false,
      )
      console.log(`PASS (${VORON_PROFILE_FIXTURE.selection.machine}, ${VORON_PROFILE_FIXTURE.selection.process}, ${VORON_PROFILE_FIXTURE.selection.filaments.join(' + ')})`)
    } catch (err) {
      stableFailures++
      console.log('FAIL')
      console.error('  ' + err.message)
    } finally {
      module._onewasm_session_destroy(voronModifierSession)
    }

    if (stableFailures > 0) {
      console.error(`\n[smoke-test] ${stableFailures} scenario(s) failed`)
      process.exitCode = 1
    } else {
      console.log('\n[smoke-test] all engine smoke scenarios passed')
    }
  } finally {
    module._onewasm_session_destroy(stableSession)
  }
  return
}

main().catch((err) => {
  console.error('[smoke-test] fatal:', err.stack ?? err)
  process.exit(1)
})
