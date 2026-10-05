// Second --extern-post-js file: expose this engine through the
// one-wasm-slicer-api TypeScript binding (see onewasm-emscripten-glue.js).
var OneWasmEngine = onewasmDefineEmscriptenEngine(OrcaModule);
if (typeof module === 'object' && module && module.exports) module.exports.OneWasmEngine = OneWasmEngine;
