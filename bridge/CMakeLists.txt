# Resolve OrcaSlicer src/ dir (for libslic3r/*.hpp).
if(NOT DEFINED ORCA_SRC)
  message(FATAL_ERROR "ORCA_SRC is not defined. "
    "Set it via -DORCA_SRC=... or via the parent CMakeLists.")
endif()

add_library(orca_web_bridge STATIC slicer.cpp)

target_include_directories(orca_web_bridge
  BEFORE PUBLIC
    # OrcaSlicer src/ for libslic3r/*.hpp
    "${ORCA_SRC}"
)

target_compile_definitions(orca_web_bridge PUBLIC
  SLIC3R_WASM
  __EMSCRIPTEN__
)

# OCCT headers needed because slicer.cpp includes Format/STEP.hpp directly.
# The toolkits themselves are linked transitively via libslic3r.
if(DEFINED OpenCASCADE_INCLUDE_DIR)
  target_include_directories(orca_web_bridge BEFORE PUBLIC "${OpenCASCADE_INCLUDE_DIR}")
endif()

# libslic3r exposes Boost, Eigen3, nlohmann_json etc. via its PUBLIC interface.
target_link_libraries(orca_web_bridge PUBLIC libslic3r)
