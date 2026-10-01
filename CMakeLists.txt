cmake_minimum_required(VERSION 3.22)
project(orca_web_wasm C CXX)

if(NOT EMSCRIPTEN)
  message(FATAL_ERROR
    "This project must be configured with Emscripten.\n"
    "Use:  emcmake cmake <source_dir> [options]")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── Inject our custom Find modules before any system ones ────────────────────
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ── Force WASM / headless build options ─────────────────────────────────────
set(SLIC3R_WASM       ON  CACHE BOOL "" FORCE)
set(SLIC3R_GUI        OFF CACHE BOOL "" FORCE)
set(SLIC3R_STATIC     ON  CACHE BOOL "" FORCE)
set(BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(ORCA_TOOLS        OFF CACHE BOOL "" FORCE)
set(SLIC3R_BUILD_SANDBOXES OFF CACHE BOOL "" FORCE)

# ── Dependency paths (exported from build.sh via environment) ────────────────
# Boost (built for WASM by deps/build_boost.sh)
set(BOOST_ROOT        "$ENV{BOOST_WASM_ROOT}"  CACHE PATH "Boost built for WASM")
set(BOOST_INC         "${BOOST_ROOT}/include"  CACHE PATH "")
set(Boost_USE_STATIC_LIBS     ON)
set(Boost_NO_SYSTEM_PATHS     ON)
set(Boost_NO_BOOST_CMAKE      ON)

# GMP / MPFR (built for WASM by deps/build_math.sh)
set(GMP_INCLUDE_DIR   "$ENV{GMP_WASM_INC}"   CACHE PATH "")
set(GMP_LIBRARIES     "$ENV{GMP_WASM_LIB}"   CACHE PATH "")
set(MPFR_INCLUDE_DIR  "$ENV{MPFR_WASM_INC}"  CACHE PATH "")
set(MPFR_LIBRARIES    "$ENV{MPFR_WASM_LIB}"  CACHE PATH "")

# ── OpenCASCADE (OCCT) — STEP/IGES import ────────────────────────────────────
# OCCT_WASM_DIR is passed by build.sh after deps/build_occt.sh runs.
# FindOpenCASCADE.cmake (in cmake/) handles both WASM and native installs.
if(DEFINED OCCT_WASM_DIR)
  set(OCCT_WASM_DIR "${OCCT_WASM_DIR}" CACHE PATH "OCCT install for WASM")
endif()
find_package(OpenCASCADE REQUIRED)
message(STATUS "OCCT include: ${OpenCASCADE_INCLUDE_DIR}")

# ── OrcaSlicer source paths ──────────────────────────────────────────────────
set(ORCA_ROOT  "${CMAKE_CURRENT_SOURCE_DIR}/orca")
set(ORCA_SRC   "${ORCA_ROOT}/src")
set(ORCA_DEPS  "${ORCA_ROOT}/deps_src")  # bundled headers (eigen, nlohmann, etc.)

# ── Add libslic3r (patched by patches/apply.py before configure) ─────────────
# Pass all needed variables into the subdirectory scope.
set(LIBSLIC3R_SOURCE_DIR "${ORCA_SRC}/libslic3r")
if(NOT EXISTS "${LIBSLIC3R_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "orca/ submodule not found at ${ORCA_ROOT}.\n"
    "Run:  git submodule update --init --depth 1 -- orca")
endif()

add_subdirectory("${ORCA_SRC}/libslic3r" libslic3r_build)

# ── Bridge ───────────────────────────────────────────────────────────────────
add_subdirectory(bridge)

# ── Final WASM module ────────────────────────────────────────────────────────
add_subdirectory(wasm)
