# Support two build contexts:
#  A) root CMakeLists.txt (standalone / local dev):
#       ORCA_SRC and TBB_SHIM_DIR are set by the parent CMakeLists.
#  B) OrcaSlicer's root CMakeLists.txt (CI / patches/apply.py injection):
#       ORCA_SRC is set by the bridge injection block.
#       ORCA_WEB_SHIM_DIR is set by -DORCA_WEB_SHIM_DIR=... on the cmake command line.

# Resolve shim dir (TBB stubs must come before any real TBB headers).
if(DEFINED ORCA_WEB_SHIM_DIR)
  set(_SHIM_DIR "${ORCA_WEB_SHIM_DIR}")
elseif(DEFINED TBB_SHIM_DIR)
  set(_SHIM_DIR "${TBB_SHIM_DIR}")
else()
  set(_SHIM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../wasm/shims")
endif()

# Resolve OrcaSlicer src/ dir (for libslic3r/*.hpp).
if(NOT DEFINED ORCA_SRC)
  message(FATAL_ERROR "ORCA_SRC is not defined. "
    "Set it via -DORCA_SRC=... or via the parent CMakeLists.")
endif()

add_library(orca_web_bridge STATIC slicer.cpp)

target_include_directories(orca_web_bridge
  BEFORE PUBLIC
    # TBB shims override any real TBB headers
    "${_SHIM_DIR}"
    # OrcaSlicer src/ for libslic3r/*.hpp
    "${ORCA_SRC}"
)

target_compile_definitions(orca_web_bridge PUBLIC
  SLIC3R_WASM
  __EMSCRIPTEN__
)

# Set (via -DSLIC3R_WASM_MT=ON on the cmake command line) for the
# multithreaded build variant. Flips the one threading-aware line in
# slicer.cpp's private legacy multi-object adapter (auto-arrange); everything else is
# unaffected — real parallelism lives entirely inside libslic3r via the real
# oneTBB library selected through _SHIM_DIR/FindTBB.cmake above (built from
# source in CI).
option(SLIC3R_WASM_MT "Build against real oneTBB with Emscripten pthread threading" OFF)
if(SLIC3R_WASM_MT)
  target_compile_definitions(orca_web_bridge PUBLIC SLIC3R_WASM_MT)
endif()

# OCCT headers needed because slicer.cpp includes Format/STEP.hpp directly.
# The toolkits themselves are linked transitively via libslic3r.
if(DEFINED OpenCASCADE_INCLUDE_DIR)
  target_include_directories(orca_web_bridge BEFORE PUBLIC "${OpenCASCADE_INCLUDE_DIR}")
endif()

# libslic3r exposes Boost, Eigen3, nlohmann_json etc. via its PUBLIC interface.
target_link_libraries(orca_web_bridge PUBLIC libslic3r)
