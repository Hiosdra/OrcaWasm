#!/usr/bin/env node
// One-off generator: extracts the dependency-build steps from
// .github/workflows/build-wasm.yml and assembles them into a single bash
// script for local WSL builds. Not wired into any app code — a dev-only tool.
import { readFileSync, writeFileSync } from 'fs'
import { dirname, join } from 'path'
import { fileURLToPath } from 'url'
import { envExportLine, ifCondToBash, substituteExpr } from './gen-wsl-build-lib.mjs'

const __dirname = dirname(fileURLToPath(import.meta.url))
const yml = join(__dirname, '../.github/workflows/build-wasm.yml')
const allLines = readFileSync(yml, 'utf8').replace(/\r/g, '').split('\n')

// ── Scope to the `build` job only ─────────────────────────────────────────
// build-wasm.yml has a second job (`compare-outputs`) that downloads BOTH
// matrix legs' artifacts from a completed CI run and diffs them — it has its
// own `- name:`/`run:` steps (e.g. "Run st vs. mt G-code comparison") that
// match the same extraction pattern as the build job's steps but make no
// sense standalone (there's nothing to download locally). Scanning the whole
// file used to pull that step into the local script too; restrict to the
// `build:` job's line range so only its steps are ever considered.
//
// Job keys are the 2-space-indented `name:` lines that are direct children of
// the top-level `jobs:` key. Anchor the scan to *after* `jobs:` — otherwise
// the same 2-space indent under `on:` (`workflow_dispatch:`, `push:`,
// `pull_request:`) would be misread as jobs.
const jobsKeyLine = allLines.findIndex((l) => /^jobs:\s*$/.test(l))
if (jobsKeyLine === -1) throw new Error('gen-wsl-build-script: could not find top-level `jobs:` key in build-wasm.yml')
const jobLineRe = /^  ([a-zA-Z_][a-zA-Z0-9_-]*):\s*$/
const jobStarts = []
for (let idx = jobsKeyLine + 1; idx < allLines.length; idx++) {
  const m = allLines[idx].match(jobLineRe)
  if (m) jobStarts.push({ name: m[1], line: idx })
}
const buildJob = jobStarts.find((j) => j.name === 'build')
if (!buildJob) throw new Error('gen-wsl-build-script: could not find `build:` job in build-wasm.yml')
const nextJob = jobStarts.find((j) => j.line > buildJob.line)
const lines = allLines.slice(buildJob.line, nextJob ? nextJob.line : allLines.length)

// ── Variant substitution ──────────────────────────────────────────────────
// The workflow builds two variants (st/mt) from one job via a build matrix
// (`strategy.matrix.variant`), with per-variant settings resolved by GitHub
// Actions itself — `${{ matrix.variant }}`, `${{ env.FOO }}` ternaries, and
// step-level `if: matrix.variant == 'mt'` guards — before the shell ever
// sees the script. Copying step bodies verbatim leaves those `${{ }}`
// expressions as literal, unresolved text (always-false comparisons, empty
// interpolations) and silently drops the `if:` guards entirely (steps that
// should only run for mt — patching Emscripten's zlib/libjpeg ports,
// building oneTBB — would either run unconditionally or, prior to this,
// weren't even being extracted because the generator was never rerun after
// the matrix was added). The local script builds one variant per invocation,
// selected via $VARIANT (override with WASM_VARIANT=mt), and every
// `${{ matrix.variant }}` / `${{ env.FOO }}` occurrence is rewritten to the
// equivalent shell reference so the SAME script works for both variants.
// ifCondToBash() and substituteExpr() do that rewriting; they live in
// ./gen-wsl-build-lib.mjs so they can be unit-tested without running this
// generator.

// ── Job-level env: block ──────────────────────────────────────────────────
// GitHub Actions exposes every `jobs.build.env` entry as a real shell env
// var to `run:` steps (in addition to `${{ env.X }}` substitution) — the
// generated header must export the same set with the same values, or shell
// references like `${ONETBB_VERSION}` / `${EXTRA_CXX_FLAGS}` inside step
// bodies silently resolve to empty strings instead of erroring.
const envBlockStart = lines.findIndex((l) => /^\s{4}env:\s*$/.test(l))
const envBlockStepsLine = lines.findIndex((l) => /^\s{4}steps:\s*$/.test(l))
if (envBlockStart === -1 || envBlockStepsLine === -1) {
  throw new Error('gen-wsl-build-script: could not find `env:`/`steps:` blocks under `build:` job')
}
// ENV_KEY_RE's `(.+)` capture is deliberately greedy and keeps any trailing
// ` # comment`; envExportLine() strips that (via stripYamlComment) rather than
// trimming here, so the comment can't be folded into the exported literal.
const ENV_KEY_RE = /^\s{6}([A-Z_][A-Z0-9_]*):\s*(.+)$/
const envEntries = []
for (let idx = envBlockStart + 1; idx < envBlockStepsLine; idx++) {
  const m = lines[idx].match(ENV_KEY_RE)
  if (m) envEntries.push({ key: m[1], value: m[2] })
}

let envExports = ''
for (const { key, value } of envEntries) {
  envExports += `${envExportLine(key, value)}\n`
}

const steps = []
let i = 0
while (i < lines.length) {
  const nameMatch = lines[i].match(/^(\s*)- name:\s*(.+)$/)
  if (nameMatch) {
    const stepIndent = nameMatch[1].length
    const name = nameMatch[2]
    let j = i + 1
    let runLine = -1
    let inlineRun = null
    let ifCond = null
    while (j < lines.length) {
      const m2 = lines[j].match(/^(\s*)- name:/)
      if (m2 && m2[1].length <= stepIndent) break
      const ifMatch = lines[j].match(/^\s*if:\s*(.+)$/)
      if (ifMatch && ifCond === null) ifCond = ifMatch[1].trim()
      if (/^\s*run:\s*\|/.test(lines[j])) {
        runLine = j
        break
      }
      // Single-line `run: <command>` (no `|` block) — the whole step is one
      // line, e.g. "run: python3 patches/apply.py". Without this
      // branch the step is silently dropped: runLine stays -1, the `if`
      // below never fires, and nothing gets pushed to `steps`.
      const inline = lines[j].match(/^\s*run:\s*(?!\|)(\S.*)$/)
      if (inline) {
        inlineRun = inline[1]
        break
      }
      j++
    }
    if (inlineRun !== null) {
      steps.push({ name, script: inlineRun, ifCond })
      i = j + 1
      continue
    }
    if (runLine !== -1) {
      const runIndent = lines[runLine].match(/^(\s*)run:/)[1].length
      let k = runLine + 1
      const body = []
      while (k < lines.length) {
        const line = lines[k]
        if (line.trim() === '') {
          body.push('')
          k++
          continue
        }
        const lineIndent = line.match(/^(\s*)/)[1].length
        if (lineIndent <= runIndent) break
        body.push(line)
        k++
      }
      const minIndent = Math.min(...body.filter((l) => l.trim()).map((l) => l.match(/^(\s*)/)[1].length))
      const script = body.map((l) => l.slice(minIndent)).join('\n')
      steps.push({ name, script, ifCond })
      i = k
      continue
    }
  }
  i++
}

const skip = new Set(['Install Emscripten', 'Install build tools', 'Configure ccache', 'Upload to GitHub Release'])

const header = `#!/usr/bin/env bash
# Generated by scripts/gen-wsl-build-script.mjs from .github/workflows/build-wasm.yml —
# re-run that generator after editing the workflow rather than hand-editing this file.
set -e
# Which build-matrix leg to build locally — the workflow builds st and mt as
# two separate CI jobs (strategy.matrix.variant); this script builds one at a
# time. Override with WASM_VARIANT=mt.
export VARIANT="\${WASM_VARIANT:-st}"
if [[ "$VARIANT" != "st" && "$VARIANT" != "mt" ]]; then
  echo "[build-local-wsl] WASM_VARIANT must be 'st' or 'mt', got: $VARIANT" >&2
  exit 1
fi
${envExports}source "$EMSDK/emsdk_env.sh"
export CCACHE_DIR="$HOME/.cache/ccache"
mkdir -p "$CCACHE_DIR"
ccache --max-size=2G >/dev/null 2>&1 || true
# Local-only: rolling-release distros (Arch, etc.) ship a much newer CMake
# than CI's ubuntu-latest. Several deps (EXPAT 2.5.0 confirmed) declare an
# old cmake_minimum_required() that newer CMake rejects outright ("Compatibility
# with CMake < 3.5 has been removed"). This env var is CMake's own documented
# escape hatch (3.31+) and applies to every invocation without patching each
# dep's CMakeLists.txt. Not needed in CI, so not added to build-wasm.yml.
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cd "$(dirname "$0")/.."   # repo root (this script lives in scripts/)
# The SDK's default cache may be read-only. Keep a variant-specific local
# cache beside the build tree; override it with WASM_EM_CACHE when needed.
export EM_CACHE="\${WASM_EM_CACHE:-$(pwd)/build-wasm/emscripten-cache-\${VARIANT}}"
mkdir -p "$EM_CACHE"
echo "[build-local-wsl] repo root: $(pwd)"
echo "[build-local-wsl] ORCA_VERSION=$ORCA_VERSION VARIANT=$VARIANT"
`

// CI always starts from a fresh checkout, so build-wasm.yml's clone step
// has no guard for "already exists" — a local re-run after any later step
// fails would otherwise abort on `git clone` seeing a non-empty directory.
// Only this one step needs it: every dependency step already guards itself
// with a stamp file, and cmake/ninja are naturally idempotent.
const LOCAL_OVERRIDES = {
  'Checkout OrcaSlicer ${{ env.ORCA_VERSION }}': `if [ -d orca/.git ]; then
  echo "[checkout] orca already present — skip"
else
  rm -rf orca
  git clone --depth 1 --branch "$ORCA_VERSION" \\
    https://github.com/SoftFever/OrcaSlicer.git \\
    orca
fi`,
}

let out = header
for (const s of steps) {
  if (skip.has(s.name)) continue
  const displayName = substituteExpr(s.name, `step name ${JSON.stringify(s.name)}`)
  out += `\n# ══════════════════════════════════════════════════════════\n# ${displayName}\n# ══════════════════════════════════════════════════════════\n`
  const rawBody = LOCAL_OVERRIDES[s.name] ?? s.script
  const body = substituteExpr(rawBody, `step "${s.name}"`)
  // Each step runs as its own shell process in CI, so a stamp-check's
  // `exit 0` ("already built, skip the rest of this step") only ends that
  // one step there. Concatenated into a single script, a bare `exit 0`
  // would terminate the ENTIRE build instead — wrap each step in a subshell
  // so `exit` only escapes that step; `set -e` propagates into subshells,
  // so a real failure still stops the whole script via its own nonzero exit.
  if (s.ifCond) {
    const bashCond = ifCondToBash(s.ifCond)
    out += `if ${bashCond}; then\n(\n${body}\n)\nelse\n  echo "[skip] ${displayName} — requires ${s.ifCond}, VARIANT=$VARIANT"\nfi\n`
  } else {
    out += `(\n${body}\n)\n`
  }
}

const outPath = join(__dirname, 'build-local-wsl.sh')
writeFileSync(outPath, out, { mode: 0o755 })
console.log(`Wrote ${out.length} bytes to ${outPath}`)
