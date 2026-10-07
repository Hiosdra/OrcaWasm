# Custom module-mode Boost finder for WASM cross-compilation.
#
# The Emscripten toolchain sets CMAKE_FIND_ROOT_PATH_MODE_INCLUDE/LIBRARY=ONLY,
# which makes standard find_path/find_library prepend the sysroot to every
# search path, causing them to miss our cross-compiled libs in BOOST_ROOT.
# This shim uses NO_CMAKE_FIND_ROOT_PATH on every search call to bypass that.
#
# Provides Boost::<component> imported targets and the standard FindBoost vars.

cmake_minimum_required(VERSION 3.16)

# ── Locate Boost root ──────────────────────────────────────────────────────────
if(DEFINED Boost_ROOT)
    set(_BR "${Boost_ROOT}")
elseif(DEFINED BOOST_ROOT)
    set(_BR "${BOOST_ROOT}")
elseif(DEFINED CMAKE_PREFIX_PATH)
    # Take the first entry from the ;-delimited list.
    string(REPLACE ";" ";" _pfx_list "${CMAKE_PREFIX_PATH}")
    list(GET _pfx_list 0 _BR)
else()
    message(FATAL_ERROR "FindBoost (WASM shim): set BOOST_ROOT or Boost_ROOT")
endif()

set(_BINC "${_BR}/include")
set(_BLIB "${_BR}/lib")

# ── Find include dir ───────────────────────────────────────────────────────────
find_path(Boost_INCLUDE_DIR
    NAMES "boost/config.hpp"
    PATHS "${_BINC}"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH     # bypass emscripten's sysroot-only restriction
)

# ── Parse version ──────────────────────────────────────────────────────────────
if(Boost_INCLUDE_DIR AND EXISTS "${Boost_INCLUDE_DIR}/boost/version.hpp")
    file(STRINGS "${Boost_INCLUDE_DIR}/boost/version.hpp" _v_lines
        REGEX "#define BOOST_VERSION ")
    string(REGEX MATCH "[0-9]+" _v_num "${_v_lines}")
    if(_v_num)
        math(EXPR Boost_MAJOR_VERSION   "${_v_num} / 100000")
        math(EXPR Boost_MINOR_VERSION   "(${_v_num} / 100) % 1000")
        math(EXPR Boost_SUBMINOR_VERSION "${_v_num} % 100")
        set(Boost_VERSION
            "${Boost_MAJOR_VERSION}.${Boost_MINOR_VERSION}.${Boost_SUBMINOR_VERSION}")
        set(Boost_VERSION_STRING "${Boost_VERSION}")
    endif()
endif()

# ── Header-only targets ────────────────────────────────────────────────────────
if(Boost_INCLUDE_DIR)
    foreach(_tgt Boost::headers Boost::boost)
        if(NOT TARGET "${_tgt}")
            add_library("${_tgt}" INTERFACE IMPORTED GLOBAL)
            set_target_properties("${_tgt}" PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}")
        endif()
    endforeach()
endif()

# ── Find each requested compiled component ─────────────────────────────────────
# Component name → library filename mapping (underscore names are the default).
# Add overrides here only when the file name differs from boost_<comp>.
set(_BMAP_log_setup     "boost_log_setup")
set(_BMAP_date_time     "boost_date_time")
set(_BMAP_program_options "boost_program_options")

set(Boost_LIBRARIES "")

foreach(_comp IN LISTS Boost_FIND_COMPONENTS)
    string(TOUPPER "${_comp}" _COMP)

    # Boost.System became header-only in 1.69. Boost 1.89 removed the
    # compatibility stub library, so asking find_library() for it breaks
    # otherwise valid installs (including our minimum supported 1.83+ range).
    # Keep the component target OrcaSlicer expects, backed by Boost headers.
    if(_comp STREQUAL "system" AND Boost_VERSION VERSION_GREATER_EQUAL "1.69.0")
        set("Boost_${_comp}_FOUND" TRUE)
        set("Boost_${_COMP}_FOUND" TRUE)
        set("Boost_${_COMP}_LIBRARY" "Boost::system")
        set("Boost_${_COMP}_LIBRARY_RELEASE" "Boost::system")

        if(NOT TARGET Boost::system)
            add_library(Boost::system INTERFACE IMPORTED GLOBAL)
            set_target_properties(Boost::system PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
            )
        endif()

        list(APPEND Boost_LIBRARIES Boost::system)
        continue()
    endif()

    # Determine library filename
    if(DEFINED "_BMAP_${_comp}")
        set(_lib "${_BMAP_${_comp}}")
    else()
        set(_lib "boost_${_comp}")
    endif()

    find_library(Boost_${_COMP}_LIBRARY
        NAMES "${_lib}"
        PATHS "${_BLIB}"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH   # bypass emscripten sysroot-only restriction
    )
    mark_as_advanced(Boost_${_COMP}_LIBRARY)

    if(Boost_${_COMP}_LIBRARY)
        set("Boost_${_comp}_FOUND" TRUE)
        set("Boost_${_COMP}_FOUND" TRUE)
        list(APPEND Boost_LIBRARIES "${Boost_${_COMP}_LIBRARY}")

        # Imported static target
        if(NOT TARGET "Boost::${_comp}")
            add_library("Boost::${_comp}" STATIC IMPORTED GLOBAL)
            set_target_properties("Boost::${_comp}" PROPERTIES
                IMPORTED_LOCATION         "${Boost_${_COMP}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIR}"
            )
        endif()
    else()
        set("Boost_${_comp}_FOUND" FALSE)
        set("Boost_${_COMP}_FOUND" FALSE)
    endif()
endforeach()

set(Boost_INCLUDE_DIRS "${Boost_INCLUDE_DIR}")

# ── Standard result handling ───────────────────────────────────────────────────
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Boost
    REQUIRED_VARS   Boost_INCLUDE_DIR
    VERSION_VAR     Boost_VERSION
    HANDLE_COMPONENTS
)
