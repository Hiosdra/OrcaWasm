# Final Emscripten WASM module target.
# Links libslic3r + our bridge and emits <output name>.js / .wasm / .data.

# Emscripten requires an "executable" target even though the output is a .js module.
add_executable(slicer "${CMAKE_CURRENT_SOURCE_DIR}/empty_main.cpp")

target_link_libraries(slicer PRIVATE orca_web_bridge)

# Output filename stem — "slicer" for the default sequential build,
# "slicer-mt" for the multithreaded variant, so both can be published side by
# side and a host loader can pick one at runtime without either
# overwriting the other in artifacts/.
if(NOT DEFINED ORCA_WEB_WASM_OUTPUT_NAME)
  if(SLIC3R_WASM_MT)
    set(ORCA_WEB_WASM_OUTPUT_NAME "slicer-mt")
  else()
    set(ORCA_WEB_WASM_OUTPUT_NAME "slicer")
  endif()
endif()

# ── Emscripten link flags ────────────────────────────────────────────────────
target_link_options(slicer PRIVATE
  -O3
  # Keeps a wasm name section (function names) for readable stack traces
  # without full DWARF — needed to root-cause the Voron Cube mismatched-
  # signature indirect-call crash (see scripts/repro-real-stl.mjs); release
  # builds are otherwise fully stripped and traces are just opaque
  # wasm-function[N] indices. Negligible size/perf cost, safe to keep.
  --profiling-funcs
  -sALLOW_MEMORY_GROWTH=1
  -sMALLOC=emmalloc
  -sSTACK_SIZE=16777216
  -sINITIAL_MEMORY=268435456
  -sMAXIMUM_MEMORY=4294967296
  -sABORTING_MALLOC=0
  -sENVIRONMENT=web,worker,node
  -sMODULARIZE=1
  -sEXPORT_NAME=OrcaModule
  -sDISABLE_EXCEPTION_CATCHING=0
  $<$<BOOL:${SLIC3R_WASM_MT}>:-pthread>
  $<$<BOOL:${SLIC3R_WASM_MT}>:-sUSE_PTHREADS=1>
  # Pre-spawns the pthread worker pool at module load instead of creating
  # Web Workers lazily on first pthread_create() — required because the
  # slicer's calling thread never yields to the JS event loop while blocked
  # inside print.process(): without this, a
  # lazily-created pthread worker never actually materializes and any
  # blocking join/condvar wait on it hangs forever).
  #
  # Sized to the visitor's own core count (+headroom) so it always covers
  # oneTBB's thread demand, which is hardware_concurrency (≈ core count) and
  # can't be reliably bounded any other way: on the first parallel_for oneTBB
  # eagerly pthread_create()s ~hardware_concurrency workers, and if that
  # exceeds the pool the on-demand grow-the-pool path deadlocks a blocked
  # caller. A fixed 8 was the original bug (desktop TBB wanted ~15). Capping
  # TBB instead — tried via tbb::global_control(max_allowed_parallelism, 8) —
  # made it WORSE: that starved oneTBB's nested parallelism and deadlocked
  # even with a huge pool (verified on a 16-core machine; a pool-bumped,
  # uncapped engine sliced fine at the same pool size). So: no TBB cap, and
  # let the pool track the machine. `navigator.hardwareConcurrency` is
  # evaluated at load in web/worker/node(22); +4 covers incidental non-TBB
  # threads.
  $<$<BOOL:${SLIC3R_WASM_MT}>:-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency+4>
  # Turn any residual pool exhaustion into a loud, immediate abort (surfaced
  # via onAbort → WASM_ERROR → the app falls back to reload) instead of the
  # silent on-demand-Worker deadlock. With the pool tracking the machine this
  # should never fire; a fast, debuggable failure still beats a runtime hang.
  $<$<BOOL:${SLIC3R_WASM_MT}>:-sPTHREAD_POOL_SIZE_STRICT=2>
  # EMULATE_FUNCTION_POINTER_CASTS + a raised max-func-params was tried here
  # to fix a real crash (slicing the official Voron Design Cube v7 STL traps
  # with "memory access out of bounds"), on the initial hypothesis that it
  # was a mismatched-signature indirect call in the mesh-repair path. It
  # backfired: at -O2 with max-func-params raised to 64, the emulation
  # trampolines themselves became broken at runtime — even a trivial
  # 12-triangle cube started crashing the same way. That's strictly worse
  # than the original bug (which only hit specific real-world meshes), so
  # it was reverted. The real root cause turned out to be unrelated to
  # function-pointer signatures: uninitialized WallToolPathsParams fields
  # in Arachne's wall generator (see patches/apply.py sections
  # 8/8c/8d/8e/8f), fixed with default member initializers there instead —
  # verified against the Voron Cube, Stanford Bunny, and a 1.1M-triangle
  # model with Arachne enabled.
  -fexceptions
  # No --preload-file: the headless flat-config slicer never reads orca/resources
  # at runtime (verified in-host — a full slice opens only /tmp/*.stl/.gcode,
  # zero /resources access, and slicing is byte-identical with /resources removed).
  # Dropping the ~200 MB preload removes slicer.data entirely.  FORCE_FILESYSTEM
  # keeps MEMFS available for the bridge's /tmp STL-in / G-code-out files.
  -sFORCE_FILESYSTEM=1
  # Exported C functions consumed by the JS runtime and the common
  # one-wasm-slicer-api 0.2 contract plus the draft 0.3 project adapter.
  "-sEXPORTED_FUNCTIONS=[\
'_onewasm_session_create',\
'_onewasm_session_destroy',\
'_onewasm_init',\
'_onewasm_init_profile',\
'_onewasm_set_progress_callback',\
'_onewasm_cancel',\
'_onewasm_get_capabilities',\
'_onewasm_get_last_statistics',\
'_onewasm_project_set_objects',\
'_onewasm_project_get_manifest',\
'_onewasm_project_prepare',\
'_onewasm_project_slice',\
'_onewasm_project_get_asset',\
'_onewasm_project_export',\
'_onewasm_slice_stl',\
'_onewasm_slice_stl_multi',\
'_onewasm_prepare_plate',\
'_onewasm_obj_to_stl',\
'_onewasm_cad_to_stl',\
'_onewasm_write_3mf',\
'_onewasm_read_3mf',\
'_onewasm_free',\
'_onewasm_last_error',\
'_malloc',\
'_free'\
]"
  "-sEXPORTED_RUNTIME_METHODS=[\
'ccall',\
'cwrap',\
'getValue',\
'setValue',\
'UTF8ToString',\
'stringToUTF8',\
'lengthBytesUTF8',\
'HEAP8',\
'HEAPU8',\
'HEAP32',\
'HEAPU32'\
]"
)

# Rename Emscripten output to <ORCA_WEB_WASM_OUTPUT_NAME>.js/.wasm/.data
set_target_properties(slicer PROPERTIES
  OUTPUT_NAME "${ORCA_WEB_WASM_OUTPUT_NAME}"
  SUFFIX ".js"
)

# Copy artifacts to the repository's artifacts/ directory after build
add_custom_command(TARGET slicer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/${ORCA_WEB_WASM_OUTPUT_NAME}.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts/${ORCA_WEB_WASM_OUTPUT_NAME}.js"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_BINARY_DIR}/${ORCA_WEB_WASM_OUTPUT_NAME}.wasm"
    "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts/${ORCA_WEB_WASM_OUTPUT_NAME}.wasm"
  # No .data output: the engine is built without --preload-file.
  COMMENT "Copying WASM artifacts to artifacts/"
)
