#!/usr/bin/env node
/**
 * one-wasm-slicer-api 0.6.0 conformance run for the built artifact.
 *
 * Loads slicer-mt.js only through its embedded TypeScript binding
 * (`OneWasmEngine`) — the path every host uses — and runs the vendored,
 * binding-neutral conformance suite (wasm/onewasm/onewasm-conformance.mjs,
 * copied unmodified from the API release). Engine-specific behaviour stays in
 * smoke-test.mjs; this script proves the shared contract.
 *
 * Usage:
 *   node scripts/conformance.mjs [--wasm-dir artifacts]
 */

import { readFileSync } from 'node:fs'
import { loadEngineArtifact } from './lib/engine-harness.mjs'
import { runConformance } from '../wasm/onewasm/onewasm-conformance.mjs'

const wasmDirIndex = process.argv.indexOf('--wasm-dir')
const wasmDir = wasmDirIndex >= 0 ? process.argv[wasmDirIndex + 1] : 'artifacts'
const encode = (value) => new TextEncoder().encode(JSON.stringify(value))

// The same representative native configuration smoke-test.mjs uses.
const config = {
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
const voron = JSON.parse(readFileSync(new URL('./fixtures/voron-0.4-profile-smoke.json', import.meta.url), 'utf8'))

const { artifact, runtime } = await loadEngineArtifact(wasmDir, 'slicer-mt')
const report = await runConformance(artifact, {
  config: encode(config),
  engineOptions: { runtime },
  profileFragments: {
    'orca.native-json': encode({ sparse_infill_density: 20 }),
    'orca.profile-json': encode(voron.process),
  },
  samples: {
    obj: new TextEncoder().encode('v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\nf 1 3 2\nf 1 2 4\nf 2 3 4\nf 1 4 3\n'),
  },
})

for (const check of report.checks) {
  const status = !check.ok ? 'FAIL' : check.skipped ? 'SKIP' : 'PASS'
  console.log(`[conformance] ${status} ${check.name}${check.detail ? ` — ${check.detail}` : ''}`)
}
console.log(`[conformance] ${report.engine?.family ?? 'engine'} ${report.ok ? 'conforms to' : 'does NOT conform to'} one-wasm-slicer-api ${report.apiVersion}`)
process.exit(report.ok ? 0 : 1)
