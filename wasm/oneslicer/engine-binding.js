// Second --extern-post-js file: expose this engine through the
// one-slicer-api TypeScript binding (see oneslicer-emscripten-glue.js).
//
// The reference glue calls the synchronous C ABI on the runtime's JS thread,
// which blocks that thread until a slice returns, so an abort signal could
// only cancel an operation before it started. With runtime.cancellation an
// abort must also stop a running operation and leave the session reusable.
// Project operations therefore run on a pthread (orcawasm_async_* in
// bridge/slicer.cpp) while this thread polls, delivers progress and calls
// oneslicer_cancel on abort.
var orcawasmWithAsyncOperations = (function () {
  'use strict';
  var KIND = { slice: 0, prepare: 1, export: 2 };
  var POLL_MS = 10;
  var encoder = new TextEncoder();

  function delay() {
    return new Promise(function (resolve) { setTimeout(resolve, POLL_MS); });
  }

  // The glue does not export OneSlicerError; take its constructor from a call
  // that fails, so rejections stay real OneSlicerError instances.
  function errorClass(binding) {
    try {
      binding.call(0, function () { return [-11, function () { return undefined; }]; });
    } catch (error) {
      return error.constructor;
    }
    return null;
  }

  function runAsync(session, OneSlicerError, kind, first, second, options) {
    var binding = session.binding;
    var module = binding.module;
    var handle = session.live();
    if (options && options.signal && options.signal.aborted) throw new OneSlicerError('CANCELLED');

    var onProgress = options && options.onProgress;
    var callback = 0;
    var settled = false;
    if (onProgress && module.addFunction && typeof module._oneslicer_set_progress_callback === 'function') {
      // The bridge proxies progress to this thread, which runs it while idle.
      callback = module.addFunction(function (percent, stagePtr) {
        if (settled) return;
        try {
          onProgress({
            percent: percent >= 0 && percent <= 100 ? percent : null,
            stage: stagePtr ? module.UTF8ToString(stagePtr >>> 0) : '',
          });
        } catch (error) {
          // A host callback must not unwind through the engine.
        }
      }, 'viii');
      module._oneslicer_set_progress_callback(handle, callback, 0);
    }
    var release = function () {
      settled = true;
      if (callback) {
        module._oneslicer_set_progress_callback(handle, 0, 0);
        if (module.removeFunction) module.removeFunction(callback);
        callback = 0;
      }
    };

    var operation;
    try {
      operation = binding.call(handle, function (heap) {
        return [0, function () {
          return module._orcawasm_async_start(
            kind, handle, heap.bytes(first), first.byteLength, heap.bytes(second), second.byteLength) >>> 0;
        }];
      });
    } catch (error) {
      release();
      throw error;
    }
    if (!operation) {
      release();
      return null;
    }

    var signal = options && options.signal;
    var cancel = function () { module._oneslicer_cancel(handle); };
    if (signal) signal.addEventListener('abort', cancel);
    return (async function () {
      var state = module._malloc(12);
      try {
        if (!state) throw new OneSlicerError('OUTPUT', 'out of engine memory polling an operation');
        while (!module._orcawasm_async_poll(operation, state, state + 4, state + 8)) {
          // The operation clears earlier cancellation when it starts, so
          // repeat the request until it finishes.
          if (signal && signal.aborted) cancel();
          await delay();
        }
        var status = module.getValue(state, 'i32');
        var out = module.getValue(state + 4, 'i32') >>> 0;
        var outLen = module.getValue(state + 8, 'i32') >>> 0;
        return binding.call(handle, function (heap) {
          var outPtr = heap.outPointer();
          var outLenPtr = heap.outPointer();
          module.setValue(outPtr, out, 'i32');
          module.setValue(outLenPtr, outLen, 'i32');
          return [status, function () { return binding.takeJson(outPtr, outLenPtr); }];
        });
      } finally {
        if (state) module._free(state);
        if (signal) signal.removeEventListener('abort', cancel);
        release();
      }
    })();
  }

  function wrapSession(session, OneSlicerError) {
    if (typeof session.binding.module._orcawasm_async_start !== 'function') return session;
    var slice = session.slice.bind(session);
    var prepare = session.prepare.bind(session);
    var exportProject = session.export.bind(session);
    var empty = new Uint8Array(0);
    session.slice = async function (request, options) {
      var run = runAsync(session, OneSlicerError, KIND.slice, encoder.encode(JSON.stringify(request)), empty, options);
      return run === null ? slice(request, options) : run;
    };
    session.prepare = async function (request, options) {
      var run = runAsync(session, OneSlicerError, KIND.prepare, encoder.encode(JSON.stringify(request)), empty, options);
      return run === null ? prepare(request, options) : run;
    };
    session.export = async function (format, options, operation) {
      var run = runAsync(session, OneSlicerError, KIND.export,
        encoder.encode(format), encoder.encode(JSON.stringify(options)), operation);
      return run === null ? exportProject(format, options, operation) : run;
    };
    return session;
  }

  return function (artifact) {
    return {
      apiVersion: artifact.apiVersion,
      createEngine: async function (options) {
        var engine = await artifact.createEngine(options);
        var OneSlicerError = errorClass(engine.binding);
        if (!OneSlicerError) return engine;
        var createSession = engine.createSession.bind(engine);
        engine.createSession = async function () {
          return wrapSession(await createSession(), OneSlicerError);
        };
        return engine;
      },
    };
  };
})();
var OneSlicerEngine = orcawasmWithAsyncOperations(oneslicerDefineEmscriptenEngine(OrcaModule));
if (typeof module === 'object' && module && module.exports) module.exports.OneSlicerEngine = OneSlicerEngine;
