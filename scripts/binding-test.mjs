#!/usr/bin/env node
/**
 * OrcaWasm TypeScript binding behaviour beyond the shared conformance suite:
 * project operations run off the runtime's JS thread, so a host abort stops a
 * running slice with CANCELLED and the same session slices again afterwards.
 *
 * Usage:
 *   node scripts/binding-test.mjs [--wasm-dir artifacts]
 */

import { annotateFailuresOnGitHub, loadEngineArtifact, sphereStl } from './lib/engine-harness.mjs'

annotateFailuresOnGitHub('[binding]')

const wasmDirIndex = process.argv.indexOf('--wasm-dir')
const wasmDir = wasmDirIndex >= 0 ? process.argv[wasmDirIndex + 1] : 'artifacts'
const encode = (value) => new TextEncoder().encode(JSON.stringify(value))

const config = {
  bed_size_x: 256,
  bed_size_y: 256,
  printable_height: 250,
  nozzle_diameter: 0.4,
  layer_height: 0.1,
  initial_layer_print_height: 0.2,
  wall_loops: 3,
  top_shell_layers: 4,
  bottom_shell_layers: 4,
  sparse_infill_density: 20,
  sparse_infill_pattern: 'gyroid',
  filament_type: 'PLA',
  nozzle_temperature: 220,
  bed_temperature: 55,
}

// A finely tessellated sphere with gyroid infill at 0.1 mm layers takes long
// enough to abort while the slice is running.
const mesh = sphereStl(5, 40)
const manifest = {
  schemaVersion: '0.7.0',
  plates: [{ id: 'plate-0', label: 'Plate 1', index: 0 }],
  meshes: [{ id: 'mesh-0', format: 'stl', dataRange: { offset: 0, length: mesh.length } }],
  objects: [{ id: 'object-0', meshId: 'mesh-0' }],
  instances: [{
    id: 'instance-0',
    objectId: 'object-0',
    plateId: 'plate-0',
    transform: { matrix: [1, 0, 0, 128, 0, 1, 0, 128, 0, 0, 1, 0, 0, 0, 0, 1] },
  }],
  modifierVolumes: [],
}
const request = {
  schemaVersion: '0.7.0',
  plateSelection: 'selected',
  plateIds: ['plate-0'],
  includeGcode: true,
  includeStatistics: false,
}

let failures = 0
async function check(name, run) {
  process.stdout.write(`[binding] ${name} ... `)
  try {
    const detail = await run()
    console.log(`PASS${detail ? ` (${detail})` : ''}`)
  } catch (error) {
    failures++
    console.log('FAIL')
    console.error(`  ${error?.stack ?? error}`)
  }
}

async function sliceGcode(session, options) {
  const result = await session.slice(request, options)
  const asset = result.plateResults[0]?.assets.find((entry) => entry.kind === 'gcode')
  if (!asset) throw new Error('slice published no G-code asset')
  const bytes = await session.getAsset(asset.id)
  if (bytes.byteLength !== asset.byteLength || bytes.byteLength < 1000) {
    throw new Error(`unexpected G-code asset of ${bytes.byteLength} bytes`)
  }
  return bytes
}

const { artifact, runtime } = await loadEngineArtifact(wasmDir, 'slicer-mt')
const engine = await artifact.createEngine({ runtime })
const session = await engine.createSession()
try {
  await session.init(encode(config))
  await session.setObjects(mesh, manifest)

  let baselineMs = 0
  await check('a running slice leaves the JS thread free and reports progress', async () => {
    let ticks = 0
    const timer = setInterval(() => { ticks++ }, 5)
    const progress = []
    const started = performance.now()
    try {
      await sliceGcode(session, { onProgress: (event) => progress.push(event) })
    } finally {
      clearInterval(timer)
    }
    baselineMs = performance.now() - started
    if (ticks < 2) throw new Error(`the JS thread ran only ${ticks} timer tick(s) during the slice`)
    if (!progress.some((event) => typeof event.percent === 'number')) throw new Error('no numeric progress event')
    return `${Math.round(baselineMs)} ms, ${ticks} timer ticks, ${progress.length} progress events`
  })

  await check('an abort during a running slice rejects with CANCELLED', async () => {
    const controller = new AbortController()
    let abortedAt = 0
    const started = performance.now()
    let failure = null
    try {
      await session.slice(request, {
        signal: controller.signal,
        onProgress: (event) => {
          if (!abortedAt && typeof event.percent === 'number' && event.percent > 5) {
            abortedAt = performance.now()
            controller.abort()
          }
        },
      })
    } catch (error) {
      failure = error
    }
    if (!abortedAt) throw new Error('the slice finished before it could be aborted')
    if (failure?.name !== 'OneSlicerError' || failure.code !== 'CANCELLED') {
      throw new Error(`expected CANCELLED, got ${failure ? `${failure.name} ${failure.code}: ${failure.message}` : 'success'}`)
    }
    const stopMs = performance.now() - abortedAt
    if (baselineMs && stopMs > baselineMs) throw new Error(`cancellation took ${Math.round(stopMs)} ms, longer than a full slice`)
    return `stopped ${Math.round(stopMs)} ms after abort, ${Math.round(abortedAt - started)} ms into the slice`
  })

  await check('the cancelled session slices the same project again', async () => {
    const bytes = await sliceGcode(session)
    return `${bytes.byteLength} G-code bytes`
  })

  await check('a pre-aborted signal still rejects before running', async () => {
    const controller = new AbortController()
    controller.abort()
    try {
      await session.slice(request, { signal: controller.signal })
    } catch (error) {
      if (error?.code === 'CANCELLED') return null
      throw error
    }
    throw new Error('a pre-aborted slice succeeded')
  })
} finally {
  await session.dispose()
  await engine.dispose()
}

console.log(`[binding] ${failures ? `${failures} check(s) FAILED` : 'all checks passed'}`)
process.exit(failures ? 1 : 0)
