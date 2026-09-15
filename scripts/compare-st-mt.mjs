#!/usr/bin/env node
/**
 * ST vs MT G-code equivalence check.
 *
 * Slices a fixed set of project manifests through both engine variants — slicer.js
 * (single-threaded, wasm/shims/ header stubs) and slicer-mt.js
 * (multithreaded, real oneTBB + Emscripten pthreads) — with identical
 * configs, and compares the resulting G-code. Real parallelism can reorder
 * floating-point reductions, so this deliberately does NOT
 * require byte-equal output: it requires an identical toolpath *structure*
 * (same layer count, same number of G0/G1 moves, same move types in the
 * same order) with G0/G1 coordinates matching within a small numeric
 * tolerance.
 *
 * Requires both engines already built/downloaded into --wasm-dir (default
 * artifacts/) — this does not build or fetch them itself. In CI, run
 * after both build-wasm.yml matrix legs have published, with both artifact
 * sets downloaded into the same directory.
 *
 * Usage:
 *   node scripts/compare-st-mt.mjs [--wasm-dir artifacts] [--tolerance 0.01]
 */

import {
  trianglesToStl, sphereStl, loadModule, checkedMalloc, free, decodeError,
  initSession, projectSetObjectsOnce, projectSliceOnce, projectGetAssetOnce,
} from './lib/engine-harness.mjs'

// ── CLI args ──────────────────────────────────────────────────────────────────

function parseArgs(argv) {
  const args = { wasmDir: 'artifacts', tolerance: 0.01 }
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === '--wasm-dir') args.wasmDir = argv[++i]
    else if (argv[i] === '--tolerance') args.tolerance = Number(argv[++i])
  }
  return args
}

// ── test meshes ──────────────────────────────────────────────────────────────
// A fixed set of meshes (small cube, a >100k-triangle organic mesh, and a
// multi-object project plate through the 0.3 project surface).
// sphereStl()/trianglesToStl() live in ./lib/engine-harness.mjs.

function cubeStl(sizeMm) {
  const s = sizeMm
  const v = [
    [0, 0, 0], [s, 0, 0], [s, s, 0], [0, s, 0],
    [0, 0, s], [s, 0, s], [s, s, s], [0, s, s],
  ]
  const faces = [
    [0, 2, 1], [0, 3, 2], // bottom
    [4, 5, 6], [4, 6, 7], // top
    [0, 1, 5], [0, 5, 4], // front
    [1, 2, 6], [1, 6, 5], // right
    [2, 3, 7], [2, 7, 6], // back
    [3, 0, 4], [3, 4, 7], // left
  ]
  return trianglesToStl(v, faces)
}

const MESHES = {
  'small cube (20mm)': () => cubeStl(20),
  // subdivisions=7 -> 20 * 4^7 = 327,680 triangles, comfortably over the
  // >100k-triangle bar Phase 4 asks for.
  'organic mesh (~328k tris)': () => sphereStl(7, 15),
}

// ── engine harness ──────────────────────────────────────────────────────────
// loadModule() + onewasm_* heap marshaling live in ./lib/engine-harness.mjs.

function assertCapabilities(module, label) {
  const outPtrPtr = checkedMalloc(module, 4, `${label} capability output pointer`)
  const outLenPtr = checkedMalloc(module, 4, `${label} capability output length`)
  try {
    const rc = module._onewasm_get_capabilities(outPtrPtr, outLenPtr)
    if (rc !== 0) throw new Error(`${label}: get_capabilities failed (${rc}): ${decodeError(module, 0)}`)
    const ptr = module.getValue(outPtrPtr, 'i32')
    const len = module.getValue(outLenPtr, 'i32')
    try {
      const capabilities = JSON.parse(new TextDecoder().decode(module.HEAPU8.slice(ptr, ptr + len)))
      if (capabilities.api?.name !== 'one-wasm-slicer-api' || capabilities.api?.version !== '0.3.0') {
        throw new Error(`${label}: artifact does not identify one-wasm-slicer-api 0.3.0`)
      }
      for (const feature of ['project.manifest', 'project.slice', 'project.slice.multiPlate', 'project.slice.assets']) {
        if (capabilities.features?.[feature] !== 'supported') {
          throw new Error(`${label}: capability ${feature} is not supported`)
        }
      }
      return capabilities
    } finally {
      module._onewasm_free(ptr)
    }
  } finally {
    free(module, outLenPtr)
    free(module, outPtrPtr)
  }
}

function projectMatrix(tx, ty) {
  return [
    1, 0, 0, tx,
    0, 1, 0, ty,
    0, 0, 1, 0,
    0, 0, 0, 1,
  ]
}

function projectForMeshes(meshes) {
  const blobLength = meshes.reduce((sum, mesh) => sum + mesh.length, 0)
  const blob = new Uint8Array(blobLength)
  const manifestMeshes = []
  const objects = []
  const instances = []
  let offset = 0
  meshes.forEach((mesh, index) => {
    blob.set(mesh, offset)
    manifestMeshes.push({
      id: `mesh-${index}`,
      format: 'stl',
      dataRange: { offset, length: mesh.length },
    })
    objects.push({ id: `object-${index}`, meshId: `mesh-${index}`, extruderId: 0 })
    instances.push({
      id: `instance-${index}`,
      objectId: `object-${index}`,
      plateId: 'plate-0',
      transform: { matrix: projectMatrix(100 + index * 45, 100) },
    })
    offset += mesh.length
  })
  return {
    blob,
    manifest: {
      schemaVersion: '0.3',
      plates: [{ id: 'plate-0', label: 'Comparison plate', index: 0 }],
      meshes: manifestMeshes,
      objects,
      instances,
    },
  }
}

function projectSliceGcode(module, session, meshes) {
  const project = projectForMeshes(meshes)
  projectSetObjectsOnce(module, session, project.blob, project.manifest)
  const result = projectSliceOnce(module, session, {
    schemaVersion: '0.3',
    plateSelection: 'all',
    includeGcode: true,
    includeStatistics: true,
  })
  if (result?.schemaVersion !== '0.3' || result.plateResults?.length !== 1) {
    throw new Error('project_slice returned an unexpected 0.3 result')
  }
  const plate = result.plateResults[0]
  const asset = plate.assets?.find((entry) => entry.kind === 'gcode')
  if (!asset || asset.id !== 'gcode:plate-0' || plate.statistics?.schemaVersion !== '0.3') {
    throw new Error('project_slice did not return the expected G-code/statistics assets')
  }
  return new TextDecoder().decode(projectGetAssetOnce(module, session, asset.id))
}

function layerCount(gcode) {
  const m = gcode.match(/;\s*total layers count\s*=\s*(\d+)/i)
  if (m) return Number(m[1])
  return (gcode.match(/;LAYER_CHANGE/g) ?? []).length
}

// Every G0/G1 move, in order, as {cmd, x, y, z, e} (fields absent from a
// given line are carried forward from the previous move, matching G-code's
// modal-coordinate semantics — comparing raw per-line values without this
// would flag every line that only changes one axis as a "structural"
// mismatch against a variant that happened to also restate the others).
function extractMoves(gcode) {
  const moves = []
  let x, y, z, e
  for (const line of gcode.split('\n')) {
    const cleanLine = line.split(';', 1)[0].trim()
    const m = cleanLine.match(/^(G[01])\s+(.*)/)
    if (!m) continue
    const [, cmd, rest] = m
    const xm = rest.match(/X(-?[0-9.]+)/)
    const ym = rest.match(/Y(-?[0-9.]+)/)
    const zm = rest.match(/Z(-?[0-9.]+)/)
    const em = rest.match(/E(-?[0-9.]+)/)
    if (!xm && !ym && !zm && !em) continue
    if (xm) x = parseFloat(xm[1])
    if (ym) y = parseFloat(ym[1])
    if (zm) z = parseFloat(zm[1])
    if (em) e = parseFloat(em[1])
    moves.push({ cmd, x, y, z, e })
  }
  return moves
}

// Returns null on structural equivalence, or a description of the first
// divergence found.
function compareGcode(stGcode, mtGcode, tolerance) {
  const stLayers = layerCount(stGcode)
  const mtLayers = layerCount(mtGcode)
  if (stLayers !== mtLayers) {
    return `layer count differs: st=${stLayers} mt=${mtLayers}`
  }
  if (stLayers === 0) {
    return 'layer count is 0 on both — G-code parsing likely broken, not a real pass'
  }

  const stMoves = extractMoves(stGcode)
  const mtMoves = extractMoves(mtGcode)
  if (stMoves.length !== mtMoves.length) {
    return `move count differs: st=${stMoves.length} mt=${mtMoves.length}`
  }

  let maxDelta = 0
  for (let i = 0; i < stMoves.length; i++) {
    const a = stMoves[i], b = mtMoves[i]
    if (a.cmd !== b.cmd) return `move ${i}: command differs (st=${a.cmd} mt=${b.cmd})`
    for (const axis of ['x', 'y', 'z', 'e']) {
      const av = a[axis], bv = b[axis]
      if ((av === undefined) !== (bv === undefined)) {
        return `move ${i} (${a.cmd}): ${axis.toUpperCase()} presence differs`
      }
      if (av === undefined) continue
      const delta = Math.abs(av - bv)
      if (delta > tolerance) {
        return `move ${i} (${a.cmd}): ${axis.toUpperCase()} differs beyond tolerance ` +
          `(st=${av} mt=${bv}, delta=${delta.toFixed(4)} > ${tolerance})`
      }
      maxDelta = Math.max(maxDelta, delta)
    }
  }
  return { ok: true, layers: stLayers, moves: stMoves.length, maxDelta }
}

// ── real base config (same as smoke-test.mjs) ─────────────────────────────────

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

// ── main ──────────────────────────────────────────────────────────────────────

async function main() {
  const { wasmDir, tolerance } = parseArgs(process.argv.slice(2))
  console.log(`[compare-st-mt] loading st + mt engines from ${wasmDir} (tolerance=${tolerance}mm)...`)
  const st = await loadModule(wasmDir, 'slicer')
  const mt = await loadModule(wasmDir, 'slicer-mt')
  console.log('[compare-st-mt] both engines loaded')

  const stCapabilities = assertCapabilities(st, 'st')
  const mtCapabilities = assertCapabilities(mt, 'mt')
  console.log(`[compare-st-mt] API 0.3: ${stCapabilities.engine?.family ?? 'unknown'} / ${mtCapabilities.engine?.family ?? 'unknown'}`)

  const stSession = st._onewasm_session_create()
  const mtSession = mt._onewasm_session_create()
  if (!stSession || !mtSession) throw new Error('onewasm_session_create failed (allocation failure)')

  let failures = 0
  try {
    if (st._onewasm_cancel(stSession) !== 0 || mt._onewasm_cancel(mtSession) !== 0) {
      throw new Error('idle onewasm_cancel was not a no-op')
    }

    for (const [label, makeStl] of Object.entries(MESHES)) {
      const stlBytes = makeStl()
      process.stdout.write(`[compare-st-mt] ${label} via project_slice (${stlBytes.length} bytes) ... `)
      try {
        initSession(st, stSession, JSON.stringify(BASE_CONFIG))
        initSession(mt, mtSession, JSON.stringify(BASE_CONFIG))
        const stGcode = projectSliceGcode(st, stSession, [stlBytes])
        const mtGcode = projectSliceGcode(mt, mtSession, [stlBytes])
        const result = compareGcode(stGcode, mtGcode, tolerance)
        if (typeof result === 'string') {
          failures++
          console.log('FAIL')
          console.error(`  ${result}`)
        } else {
          console.log(`PASS (${result.layers} layers, ${result.moves} moves, max delta ${result.maxDelta.toFixed(5)}mm)`)
        }
      } catch (err) {
        failures++
        console.log('FAIL')
        console.error(`  ${err.message}`)
      }
    }

    const cube = cubeStl(20)
    const plateLabel = 'plate: 2x small cube via project_slice'
    process.stdout.write(`[compare-st-mt] ${plateLabel} ... `)
    try {
      initSession(st, stSession, JSON.stringify(BASE_CONFIG))
      initSession(mt, mtSession, JSON.stringify(BASE_CONFIG))
      const stGcode = projectSliceGcode(st, stSession, [cube, cube])
      const mtGcode = projectSliceGcode(mt, mtSession, [cube, cube])
      const result = compareGcode(stGcode, mtGcode, tolerance)
      if (typeof result === 'string') {
        failures++
        console.log('FAIL')
        console.error(`  ${result}`)
      } else {
        console.log(`PASS (${result.layers} layers, ${result.moves} moves, max delta ${result.maxDelta.toFixed(5)}mm)`)
      }
    } catch (err) {
      failures++
      console.log('FAIL')
      console.error(`  ${err.message}`)
    }
  } finally {
    st._onewasm_session_destroy(stSession)
    mt._onewasm_session_destroy(mtSession)
  }

  if (failures > 0) {
    console.error(`\n[compare-st-mt] ${failures} scenario(s) diverged`)
    process.exit(1)
  }
  console.log('\n[compare-st-mt] st and mt engines produce structurally equivalent G-code')
}

main().catch((err) => {
  console.error('[compare-st-mt] fatal:', err.stack ?? err)
  process.exit(1)
})
