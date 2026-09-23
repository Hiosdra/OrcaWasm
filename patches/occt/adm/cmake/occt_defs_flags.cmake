# OCCT's stock Unix profile enables OCC_CONVERT_SIGNALS globally. That profile
# uses setjmp/longjmp signal conversion and conflicts with WebAssembly EH.
# Keep upstream flag setup, then disable only this definition for Emscripten.
include("${CMAKE_SOURCE_DIR}/adm/cmake/occt_defs_flags.cmake")

if (EMSCRIPTEN)
  remove_definitions(-DOCC_CONVERT_SIGNALS)
endif()
