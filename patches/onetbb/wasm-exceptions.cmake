# The Emscripten Clang profile selects the legacy JavaScript exception model.
# OrcaWasm passes native WebAssembly EH flags through the parent CMake build.
if(NOT DEFINED TBB_SOURCE_DIR)
    set(TBB_SOURCE_DIR ".")
endif()

set(_clang_file "${TBB_SOURCE_DIR}/cmake/compilers/Clang.cmake")
if(NOT EXISTS "${_clang_file}")
    message(FATAL_ERROR "oneTBB Clang compiler profile not found: ${_clang_file}")
endif()

file(READ "${_clang_file}" _clang_contents)
set(_clang_patched "${_clang_contents}")
if(_clang_contents MATCHES "TBB_COMMON_COMPILE_FLAGS[^\n]*-fexceptions" OR
   _clang_contents MATCHES "TBB_TEST_LINK_FLAGS[^\n]*-fexceptions")
    string(REPLACE
        [=[  set(TBB_COMMON_COMPILE_FLAGS ${TBB_COMMON_COMPILE_FLAGS} -fexceptions)
]=]
        [=[  # The parent Emscripten build supplies -fwasm-exceptions.
]=]
        _clang_patched
        "${_clang_patched}")
    string(REPLACE
        [=[  set(TBB_TEST_LINK_FLAGS  ${TBB_COMMON_LINK_FLAGS} -fexceptions -sINITIAL_MEMORY=65536000 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1)
]=]
        [=[  set(TBB_TEST_LINK_FLAGS  ${TBB_COMMON_LINK_FLAGS} -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sINITIAL_MEMORY=65536000 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=1)
]=]
        _clang_patched
        "${_clang_patched}")
endif()
if(_clang_patched MATCHES "TBB_COMMON_COMPILE_FLAGS[^\n]*-fexceptions" OR
   _clang_patched MATCHES "TBB_TEST_LINK_FLAGS[^\n]*-fexceptions")
    message(FATAL_ERROR "oneTBB still selects legacy -fexceptions in its Emscripten profile")
endif()
if(NOT _clang_patched STREQUAL _clang_contents)
    file(WRITE "${_clang_file}" "${_clang_patched}")
endif()
