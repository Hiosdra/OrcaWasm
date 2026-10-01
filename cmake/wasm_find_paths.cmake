# Included via CMAKE_PROJECT_INCLUDE_BEFORE, which runs just before the
# top-level project() call — i.e. AFTER the emscripten toolchain file has run.
# Setting these as normal variables here shadows the toolchain's ONLY settings,
# letting find_path/find_library search both sysroot and our deps-install/.
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Some Emscripten CMake toolchain versions report thread support without
# creating the Threads::Threads imported target. The build passes -pthread
# globally; provide the empty target for upstream targets that link it.
if(NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED GLOBAL)
endif()

# Directory-scope compile definitions for the entire WASM build tree.
# Using add_compile_definitions here (rather than CMAKE_CXX_FLAGS or
# target_compile_definitions) is the most reliable way to ensure these
# symbols are visible to every translation unit, including PCH and any
# file that includes OCCT-gated headers via the Model.hpp include chain.
add_compile_definitions(
    SLIC3R_WASM=1
    SLIC3R_NO_OCCT=1
    SLIC3R_NO_OPENVDB=1
    SLIC3R_NO_OPENCV=1
    # OrcaSlicer's Boost.Log ABI uses BOOST_LOG_NO_THREADS=1 even in the
    # pthread-enabled engine. The define selects its v2s_st symbols; without
    # it, consumers reference v2s_mt_posix and wasm-ld reports undefined
    # boost::log symbols at link time.
    BOOST_LOG_NO_THREADS=1
)
