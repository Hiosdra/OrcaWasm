// Unit tests for the gen-wsl-build-script parsing helpers. Run with
// `node --test scripts/` (also run by the wsl-build-script CI job). Uses Node's
// built-in test runner so a dev-only script needs no extra dev dependency.
import assert from 'node:assert/strict'
import { describe, it } from 'node:test'
import { envExportLine, stripYamlComment, substituteExpr } from './gen-wsl-build-lib.mjs'

describe('substituteExpr', () => {
  it('rewrites env.FOO to shell references', () => {
    assert.equal(substituteExpr('v=${{ env.ORCA_VERSION }}', 'ctx'), 'v=${ORCA_VERSION}')
  })

  it('throws on an expression it has no local equivalent for', () => {
    assert.throws(() => substituteExpr('${{ github.sha }}', 'step body'), /unhandled \$\{\{ }} expression in step body/)
  })
})

describe('stripYamlComment', () => {
  it('strips a trailing comment introduced by whitespace', () => {
    assert.equal(stripYamlComment('value  # note'), 'value  ')
  })

  it('keeps a # that is part of the token (no leading whitespace)', () => {
    assert.equal(stripYamlComment('a#b'), 'a#b')
  })

  it('keeps a # inside a quoted string', () => {
    assert.equal(stripYamlComment('"a # b"'), '"a # b"')
    assert.equal(stripYamlComment("'c # d'  # real"), "'c # d'  ")
  })

  it('is a no-op when there is no comment', () => {
    assert.equal(stripYamlComment('plain'), 'plain')
  })
})

describe('envExportLine', () => {
  it('exports a plain literal as an overridable default', () => {
    assert.equal(envExportLine('EMSDK', '/opt/emsdk'), 'export EMSDK="${EMSDK:-/opt/emsdk}"')
  })

  it('unwraps a double-quoted literal', () => {
    assert.equal(envExportLine('FLAGS', '"-O2 -s"'), 'export FLAGS="${FLAGS:--O2 -s}"')
  })

  it('does not fold a trailing comment into the literal', () => {
    // The exact fragility the review flagged: `KEY: "value"  # note`.
    assert.equal(
      envExportLine('ONETBB_VERSION', 'v2021.13.0  # keep in sync'),
      'export ONETBB_VERSION="${ONETBB_VERSION:-v2021.13.0}"',
    )
  })

  it("uses a github-context expression's trailing literal fallback as the local default", () => {
    assert.equal(
      envExportLine('ORCA_VERSION', "${{ github.event.inputs.orca_version || 'v2.4.2' }}"),
      'export ORCA_VERSION="${ORCA_VERSION:-v2.4.2}"',
    )
  })

  it('throws on a github-context expression with no literal fallback', () => {
    assert.throws(() => envExportLine('X', '${{ github.ref }}'), /no .*trailing.*fallback/s)
  })
})
