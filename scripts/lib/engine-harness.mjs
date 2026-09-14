// Shared Node test harness for the OrcaSlicer WASM engine — used by
// smoke-test.mjs and compare-st-mt.mjs. Centralizes the ABI-coupled pieces
// (module loading + heap marshaling for onewasm_init/onewasm_slice_stl/
// onewasm_slice_stl_multi/onewasm_prepare_plate)
// so the C bridge's calling convention lives in ONE place instead of being
// copy-pasted and drifting between scripts.
//
// Plain Node ESM (no TS build step); host runtimes use their own loader for
// the same job. This harness runs standalone from the repository.

import { readFileSync, existsSync } from 'node:fs'
import { resolve } from 'node:path'
import { createRequire } from 'node:module'

// ── synthetic meshes ─────────────────────────────────────────────────────────

export function icosphere(subdivisions) {
  const t = (1 + Math.sqrt(5)) / 2
  let verts = [
    [-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0],
    [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t],
    [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1],
  ]
  let faces = [
    [0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11],
    [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
    [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9],
    [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1],
  ]

  const midpointCache = new Map()
  const midpoint = (a, b) => {
    const key = a < b ? `${a}_${b}` : `${b}_${a}`
    if (midpointCache.has(key)) return midpointCache.get(key)
    const [ax, ay, az] = verts[a], [bx, by, bz] = verts[b]
    verts.push([(ax + bx) / 2, (ay + by) / 2, (az + bz) / 2])
    const idx = verts.length - 1
    midpointCache.set(key, idx)
    return idx
  }

  for (let s = 0; s < subdivisions; s++) {
    const nextFaces = []
    for (const [a, b, c] of faces) {
      const ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a)
      nextFaces.push([a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca])
    }
    faces = nextFaces
  }

  // Returns the RAW subdivided icosahedron (verts on a ~unit-ish icosahedron,
  // NOT normalized to a sphere). Callers that want a printable sphere use
  // sphereStl(), which projects onto a sphere of a given radius and lifts it
  // onto the bed — do NOT re-normalize the output of this function.
  return { verts, faces }
}

// A printable sphere STL: projects the raw icosphere onto a sphere of the
// given radius and lifts it so min z = 0 (matches how the bridge centers a
// model — see onewasm_slice_stl's center_object_xy_only(), which handles X/Y but
// leaves Z as-is). Both smoke-test.mjs (radius 10) and compare-st-mt.mjs
// (radius 15) build their sphere meshes through here.
export function sphereStl(subdivisions, radiusMm) {
  const { verts, faces } = icosphere(subdivisions)
  const scaled = verts.map(([x, y, z]) => {
    const len = Math.sqrt(x * x + y * y + z * z) || 1
    return [(x / len) * radiusMm, (y / len) * radiusMm, (z / len) * radiusMm + radiusMm]
  })
  return trianglesToStl(scaled, faces)
}

export function trianglesToStl(verts, faces) {
  const buf = new ArrayBuffer(84 + faces.length * 50)
  const dv = new DataView(buf)
  let off = 80
  dv.setUint32(off, faces.length, true)
  off += 4
  for (const [i1, i2, i3] of faces) {
    const p1 = verts[i1], p2 = verts[i2], p3 = verts[i3]
    const ax = p2[0] - p1[0], ay = p2[1] - p1[1], az = p2[2] - p1[2]
    const bx = p3[0] - p1[0], by = p3[1] - p1[1], bz = p3[2] - p1[2]
    const nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx
    const nl = Math.sqrt(nx * nx + ny * ny + nz * nz) || 1
    dv.setFloat32(off, nx / nl, true); off += 4
    dv.setFloat32(off, ny / nl, true); off += 4
    dv.setFloat32(off, nz / nl, true); off += 4
    for (const p of [p1, p2, p3]) {
      dv.setFloat32(off, p[0], true); off += 4
      dv.setFloat32(off, p[1], true); off += 4
      dv.setFloat32(off, p[2], true); off += 4
    }
    dv.setUint16(off, 0, true); off += 2
  }
  return new Uint8Array(buf)
}

// ── WASM module loading (Node) ────────────────────────────────────────────────
// Mirrors the blob-URL trick in src/workers/slicer.worker.ts (Emscripten's
// MODULARIZE output is a CommonJS IIFE, not an ES module) but uses a data:
// URL instead of Blob+createObjectURL, since artifacts/ sits under this
// project's root package.json ("type": "module") — a plain require()/import
// of the file would make Node parse its CommonJS syntax as ES module syntax
// and fail. A data: URL sidesteps that entirely.
//
// Built with `-sENVIRONMENT=web,worker,node`, slicer.js's own Node-detection
// branch unconditionally does `var fs = require("fs"); ... scriptDirectory =
// __dirname + "/"` whenever it detects Node — but loading it as a real ES
// module (which the data: URL trick does) means `require`/`__dirname` don't
// exist as module-scope bindings, so a bare reference to either throws
// ReferenceError before the factory ever runs. Polyfilling them as globals
// works because an unresolved bare identifier in any script/module falls
// back to a same-named own property on globalThis. We supply wasmBinary
// directly below, so the actual fs/path calls this unlocks are never
// exercised — only the unconditional `__dirname + "/"` assignment needs to
// not throw.
globalThis.require ??= createRequire(import.meta.url)
globalThis.__dirname ??= '.'
globalThis.__filename ??= ''

export async function loadModule(wasmDir, engine) {
  const jsPath = resolve(wasmDir, `${engine}.js`)
  const wasmPath = resolve(wasmDir, `${engine}.wasm`)
  if (!existsSync(jsPath) || !existsSync(wasmPath)) {
    throw new Error(`${engine}.js/${engine}.wasm not found in ${wasmDir} — run the build workflow or build-local-wsl.sh first`)
  }
  const jsText = readFileSync(jsPath, 'utf8')
  const wasmBinary = readFileSync(wasmPath)
  const dataUrl = 'data:text/javascript;charset=utf-8,' +
    encodeURIComponent(`${jsText}\nexport default OrcaModule;`)
  const { default: factory } = await import(dataUrl)
  return factory({
    wasmBinary,
    // mt builds spawn pthread workers via `new Worker(pthreadMainJs, ...)`,
    // where pthreadMainJs defaults to `_scriptName`, which Node's shell.js
    // resolves from `__filename` — but that's polyfilled to '' above (only
    // so the unconditional `__dirname + "/"` assignment doesn't throw), so
    // without this override Node's Worker constructor rejects the empty
    // path with ERR_WORKER_PATH. Hand Emscripten the real script path
    // directly via the officially-supported override instead of relying on
    // the __filename fallback. Harmless no-op for st (non-pthread) builds,
    // whose output has no code path that reads this option.
    mainScriptUrlOrBlob: jsPath,
    printErr: (m) => console.warn('[OrcaWASM]', m),
    onAbort: (m) => { throw new Error(`WASM module aborted: ${m}`) },
  })
}

// ── minimal heap marshaling ────────────────────────────────────────────────────
// Deliberately duplicated (in miniature) from host loader rather
// than imported — this runs as plain Node ESM with no TS build step, and
// wasm-loader.ts is TypeScript.

const OBJECT_TRANSFORM_STRIDE = 11

export function encodeObjectTransforms(transforms) {
  const table = new Float32Array(transforms.length * OBJECT_TRANSFORM_STRIDE)
  transforms.forEach((transform, index) => {
    const offset = index * OBJECT_TRANSFORM_STRIDE
    table.set(transform.scale, offset)
    table.set(transform.rotation, offset + 3)
    table.set(transform.mirror, offset + 6)
    table[offset + 9] = transform.offset?.[0] ?? Number.NaN
    table[offset + 10] = transform.offset?.[1] ?? Number.NaN
  })
  return table
}

export function writeBytes(module, bytes) {
  const ptr = checkedMalloc(module, bytes.length, 'input data')
  module.HEAPU8.set(bytes, ptr)
  return ptr
}

export function checkedMalloc(module, size, label) {
  const ptr = module._malloc(size)
  if (ptr === 0 && size !== 0) throw new Error(`Out of memory allocating ${label} (${size} bytes)`)
  return ptr
}

export function free(module, ptr) {
  if (ptr !== 0) module._free(ptr)
}

export function decodeError(module, session) {
  try {
    const ptr = module._onewasm_last_error(session)
    return ptr ? module.UTF8ToString(ptr) : '(no message)'
  } catch {
    return '(failed to decode error)'
  }
}

export function initSession(module, session, configJson) {
  const configBytes = new TextEncoder().encode(configJson)
  const configPtr = writeBytes(module, configBytes)
  const rc = module._onewasm_init(session, configPtr, configBytes.length)
  free(module, configPtr)
  if (rc !== 0) throw new Error(`onewasm_init failed (${rc}): ${decodeError(module, session)}`)
}

function writeFloat32Table(module, values, label) {
  if (!values?.length) return 0
  const ptr = checkedMalloc(module, values.length * 4, label)
  for (let i = 0; i < values.length; i++) module.setValue(ptr + i * 4, values[i], 'float')
  return ptr
}

export function sliceOnce(module, session, stlBytes, transforms = null) {
  const stlPtr = writeBytes(module, stlBytes)
  try {
    const outPtrPtr = checkedMalloc(module, 4, 'G-code output pointer')
    try {
      const outLenPtr = checkedMalloc(module, 4, 'G-code output length')
      try {
        const transformsPtr = writeFloat32Table(module, transforms, 'object transform table')
        try {
          const offsetsPtr = checkedMalloc(module, 8, 'single-object offset table')
          try {
            module.setValue(offsetsPtr, 0, 'i32')
            module.setValue(offsetsPtr + 4, stlBytes.length, 'i32')
            const rc = transforms
              ? module._onewasm_slice_stl_multi(
                  session, stlPtr, stlBytes.length, offsetsPtr, 1, 0, transformsPtr, outPtrPtr, outLenPtr,
                )
              : module._onewasm_slice_stl(session, stlPtr, stlBytes.length, outPtrPtr, outLenPtr)
            if (rc !== 0) throw new Error(`onewasm slice failed (${rc}): ${decodeError(module, session)}`)
          } finally { module._free(offsetsPtr) }
          const gcodePtr = module.getValue(outPtrPtr, 'i32')
          const gcodeLen = module.getValue(outLenPtr, 'i32')
          try { return module.UTF8ToString(gcodePtr, gcodeLen) } finally { module._onewasm_free(gcodePtr) }
        } finally { free(module, transformsPtr) }
      } finally { module._free(outLenPtr) }
    } finally { module._free(outPtrPtr) }
  } finally { free(module, stlPtr) }
}

export function sliceMultiOnce(module, session, stlBytesArr, extruderIds, transforms = null) {
  const totalLen = stlBytesArr.reduce((sum, b) => sum + b.length, 0)
  const combined = new Uint8Array(totalLen)
  const offsets = new Uint32Array(stlBytesArr.length * 2)
  let pos = 0
  for (let i = 0; i < stlBytesArr.length; i++) {
    combined.set(stlBytesArr[i], pos)
    offsets[i * 2] = pos
    offsets[i * 2 + 1] = pos + stlBytesArr[i].length
    pos += stlBytesArr[i].length
  }
  const dataPtr = writeBytes(module, combined)
  try {
    const offsetsPtr = checkedMalloc(module, offsets.length * 4, 'STL offset table')
    try {
      for (let i = 0; i < offsets.length; i++) module.setValue(offsetsPtr + i * 4, offsets[i], 'i32')
      const extruderIdsPtr = extruderIds?.length ? checkedMalloc(module, extruderIds.length * 4, 'extruder ID table') : 0
      try {
        if (extruderIdsPtr) for (let i = 0; i < extruderIds.length; i++) module.setValue(extruderIdsPtr + i * 4, extruderIds[i], 'i32')
        const transformsPtr = writeFloat32Table(module, transforms, 'object transform table')
        try {
          const outPtrPtr = checkedMalloc(module, 4, 'G-code output pointer')
          try {
            const outLenPtr = checkedMalloc(module, 4, 'G-code output length')
            try {
              const rc = module._onewasm_slice_stl_multi(session, dataPtr, combined.length, offsetsPtr, stlBytesArr.length, extruderIdsPtr, transformsPtr, outPtrPtr, outLenPtr)
              if (rc !== 0) throw new Error(`onewasm_slice_stl_multi failed (${rc}): ${decodeError(module, session)}`)
              const gcodePtr = module.getValue(outPtrPtr, 'i32'), gcodeLen = module.getValue(outLenPtr, 'i32')
              try { return module.UTF8ToString(gcodePtr, gcodeLen) } finally { module._onewasm_free(gcodePtr) }
            } finally { module._free(outLenPtr) }
          } finally { module._free(outPtrPtr) }
        } finally { free(module, transformsPtr) }
      } finally { if (extruderIdsPtr) module._free(extruderIdsPtr) }
    } finally { free(module, offsetsPtr) }
  } finally { free(module, dataPtr) }
}

export function preparePlateOnce(module, session, stlBytesArr, operation, transforms = null) {
  const totalLen = stlBytesArr.reduce((sum, b) => sum + b.length, 0)
  const combined = new Uint8Array(totalLen)
  const offsets = new Uint32Array(stlBytesArr.length * 2)
  let pos = 0
  for (let i = 0; i < stlBytesArr.length; i++) {
    combined.set(stlBytesArr[i], pos)
    offsets[i * 2] = pos
    offsets[i * 2 + 1] = pos + stlBytesArr[i].length
    pos += stlBytesArr[i].length
  }

  const dataPtr = writeBytes(module, combined)
  try {
    const offsetsPtr = checkedMalloc(module, offsets.length * 4, 'STL offset table')
    try {
      for (let i = 0; i < offsets.length; i++) module.setValue(offsetsPtr + i * 4, offsets[i], 'i32')
      const transformsPtr = writeFloat32Table(module, transforms, 'object transform table')
      try {
        const outPtrPtr = checkedMalloc(module, 4, 'plate transform output pointer')
        try {
          const outLenPtr = checkedMalloc(module, 4, 'plate transform output length')
          try {
            const rc = module._onewasm_prepare_plate(
              session,
              dataPtr,
              combined.length,
              offsetsPtr,
              stlBytesArr.length,
              transformsPtr,
              operation,
              outPtrPtr,
              outLenPtr,
            )
            if (rc !== 0) throw new Error(`onewasm_prepare_plate failed (${rc}): ${decodeError(module, session)}`)
            const jsonPtr = module.getValue(outPtrPtr, 'i32'), jsonLen = module.getValue(outLenPtr, 'i32')
            try { return JSON.parse(module.UTF8ToString(jsonPtr, jsonLen)) } finally { module._onewasm_free(jsonPtr) }
          } finally { module._free(outLenPtr) }
        } finally { module._free(outPtrPtr) }
      } finally { if (transformsPtr) module._free(transformsPtr) }
    } finally { free(module, offsetsPtr) }
  } finally { free(module, dataPtr) }
}

export function projectSetObjectsOnce(module, session, objectBlob, manifest) {
  const manifestBytes = new TextEncoder().encode(JSON.stringify(manifest))
  const blobPtr = writeBytes(module, objectBlob)
  const manifestPtr = writeBytes(module, manifestBytes)
  try {
    const rc = module._onewasm_project_set_objects(
      session, blobPtr, objectBlob.length, manifestPtr, manifestBytes.length,
    )
    if (rc !== 0) throw new Error(`onewasm_project_set_objects failed (${rc}): ${decodeError(module, session)}`)
  } finally {
    free(module, manifestPtr)
    free(module, blobPtr)
  }
}

function readOwnedOutput(module, session, functionName, ...args) {
  const outPtrPtr = checkedMalloc(module, 4, `${functionName} output pointer`)
  try {
    const outLenPtr = checkedMalloc(module, 4, `${functionName} output length`)
    try {
      const rc = module[functionName](session, ...args, outPtrPtr, outLenPtr)
      if (rc !== 0) throw new Error(`${functionName} failed (${rc}): ${decodeError(module, session)}`)
      const dataPtr = module.getValue(outPtrPtr, 'i32')
      const dataLen = module.getValue(outLenPtr, 'i32')
      try {
        return module.HEAPU8.slice(dataPtr, dataPtr + dataLen)
      } finally {
        module._onewasm_free(dataPtr)
      }
    } finally {
      module._free(outLenPtr)
    }
  } finally {
    module._free(outPtrPtr)
  }
}

export function projectGetManifestOnce(module, session) {
  const bytes = readOwnedOutput(module, session, '_onewasm_project_get_manifest')
  return JSON.parse(new TextDecoder().decode(bytes))
}

export function projectPrepareOnce(module, session, request) {
  const requestBytes = new TextEncoder().encode(JSON.stringify(request))
  const requestPtr = writeBytes(module, requestBytes)
  try {
    const bytes = readOwnedOutput(
      module, session, '_onewasm_project_prepare', requestPtr, requestBytes.length,
    )
    return JSON.parse(new TextDecoder().decode(bytes))
  } finally {
    free(module, requestPtr)
  }
}

export function projectSliceOnce(module, session, request) {
  const requestBytes = new TextEncoder().encode(JSON.stringify(request))
  const requestPtr = writeBytes(module, requestBytes)
  try {
    const bytes = readOwnedOutput(
      module, session, '_onewasm_project_slice', requestPtr, requestBytes.length,
    )
    return JSON.parse(new TextDecoder().decode(bytes))
  } finally {
    free(module, requestPtr)
  }
}

export function projectGetAssetOnce(module, session, assetId) {
  const assetBytes = new TextEncoder().encode(assetId)
  const assetPtr = writeBytes(module, assetBytes)
  try {
    return readOwnedOutput(
      module, session, '_onewasm_project_get_asset', assetPtr, assetBytes.length,
    )
  } finally {
    free(module, assetPtr)
  }
}

export function projectExportOnce(module, session, format, options) {
  const formatBytes = new TextEncoder().encode(format)
  const optionsBytes = new TextEncoder().encode(JSON.stringify(options))
  const formatPtr = writeBytes(module, formatBytes)
  const optionsPtr = writeBytes(module, optionsBytes)
  try {
    const bytes = readOwnedOutput(
      module,
      session,
      '_onewasm_project_export',
      formatPtr,
      formatBytes.length,
      optionsPtr,
      optionsBytes.length,
    )
    return JSON.parse(new TextDecoder().decode(bytes))
  } finally {
    free(module, optionsPtr)
    free(module, formatPtr)
  }
}
