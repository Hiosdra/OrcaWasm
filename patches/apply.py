#!/usr/bin/env python3
"""
Apply WASM compatibility changes to the OrcaSlicer source tree.

Strategy
--------
* CMakeLists.txt files are patched in-place (unavoidable — OrcaSlicer has no
  upstream WASM build support).
* C++ *stub .cpp* files (OCCT, OpenVDB, OpenCV, Draco) are NOT
  modified.  The CMake injection in section 4c marks originals as
  HEADER_FILE_ONLY and adds our override implementations from
  ``overrides/``.
* C++ *stub .hpp* files whose originals include unavailable headers (OCCT,
  OpenVDB, OpenCV) ARE copied from ``overrides/`` directly into
  the orca/ source tree (section 4b).  GCC/Clang always search the including
  file's directory before any -I path, so the only reliable override is to
  replace the file in-place.  The canonical stub source remains in overrides/.
* C++ *bugfixes* (narrowing, Eigen deduction, Boost.Log, Platform assert,
  thumbnail jpg→png) are still patched in-place; these are genuine
  compatibility issues that should be upstreamed to OrcaSlicer.

Run from the repository root with:

    python3 patches/apply.py [--check]

--check  dry-run: print what would change without writing files.
"""

import re
import sys
import shutil
import argparse
from pathlib import Path

ORCA      = Path(__file__).resolve().parent.parent / "orca"
OVERRIDES = Path(__file__).resolve().parent.parent / "overrides"

# Parsed here, at the top, rather than in `if __name__ == "__main__":` at the
# bottom of the file: every patch()/copy_override()/verify_contains() call
# below executes at module level as the interpreter reads the file top to
# bottom, so a DRY_RUN assignment placed after them (as it previously was)
# only took effect once every patch had already run for real — making
# --check apply every change it claims to only preview. Parse eagerly so
# DRY_RUN is correct for every call that follows.
_parser = argparse.ArgumentParser()
_parser.add_argument("--check", action="store_true")
_cli_args = _parser.parse_args()
DRY_RUN = _cli_args.check


def copy_override(rel_path: str) -> None:
    """Copy an override file from overrides/ into the orca/ source tree."""
    src = OVERRIDES / rel_path
    dst = ORCA / rel_path
    if not src.exists():
        print(f"  SKIP (override not found): {rel_path}")
        return
    if dst.exists() and dst.read_text(encoding="utf-8") == src.read_text(encoding="utf-8"):
        print(f"  OK (no change): {rel_path}")
        return
    if DRY_RUN:
        print(f"  WOULD COPY: {rel_path}")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    print(f"  COPY: {rel_path}")


def patch(rel_path: str, replacements: list[tuple[str, str, int]]) -> bool:
    """
    Apply a list of (pattern, replacement, expected_count) tuples to a file.
    expected_count == 0 means the match is optional (already applied or absent).
    Returns True if the file was (or would be) modified.
    """
    path = ORCA / rel_path
    if not path.exists():
        print(f"  SKIP (not found): {rel_path}")
        return False
    text = path.read_text(encoding="utf-8", errors="replace")
    original = text
    for pattern, replacement, expected in replacements:
        n = len(re.findall(pattern, text, re.MULTILINE | re.DOTALL))
        if expected > 0 and n == 0:
            print(f"  WARN pattern not found in {rel_path}: {pattern[:60]!r}")
        text = re.sub(pattern, replacement, text, flags=re.MULTILINE | re.DOTALL)
    if text == original:
        print(f"  OK (no change): {rel_path}")
        return False
    if DRY_RUN:
        print(f"  WOULD PATCH: {rel_path}")
        return True
    path.write_text(text, encoding="utf-8")
    print(f"  PATCHED: {rel_path}")
    return True


def verify_contains(rel_path: str, must_contain: str, description: str) -> None:
    """
    Hard-fail the patcher (and therefore the CI build) if `must_contain` is
    absent from the given file, regardless of whether this run's patch()
    call actually matched anything.

    patch() only WARNs on a regex mismatch — by design, since most patches
    are meant to be idempotent (a pattern legitimately won't match a file
    that's already patched from a prior run). That means a genuine failure
    (e.g. upstream OrcaSlicer reformatting the target code so the pattern
    never matches at all, patched or not) is silently indistinguishable
    from "already applied" and never fails CI. Use this for patches whose
    correctness is safety-critical — it checks the *outcome*, not whether a
    substitution fired this run, so it passes on a truly idempotent re-run
    but fails if the source no longer looks like what the patch expects.
    """
    path = ORCA / rel_path
    if not path.exists() or must_contain not in path.read_text(encoding="utf-8", errors="replace"):
        print(f"FATAL: {description}\n  Expected to find in {rel_path}: {must_contain!r}", file=sys.stderr)
        if not DRY_RUN:
            sys.exit(1)


# =============================================================================
# 1. Root CMakeLists.txt — add SLIC3R_WASM option; make heavy deps conditional
# =============================================================================
patch("CMakeLists.txt", [
    # Add SLIC3R_WASM option near the top (after project() line). The
    # negative lookahead makes this idempotent — without it, re-running
    # apply.py against an already-patched checkout (e.g. every local
    # rebuild via build-local-wsl.sh, which intentionally reuses the same
    # checkout instead of re-cloning) would match project(...) again and
    # append another duplicate option() line every single run.
    (
        r'(project\s*\(\s*OrcaSlicer[^)]*\))(?!\n\noption\(SLIC3R_WASM)',
        r'\1\n\noption(SLIC3R_WASM "Build for WebAssembly with Emscripten" OFF)',
        1,
    ),
    # No guard needed for wxWidgets: it's only found() in src/CMakeLists.txt
    # inside `if(SLIC3R_GUI)`, and the root CMakeLists.txt already forces
    # SLIC3R_GUI OFF for WASM builds.
    #
    # Downgrade CMP0167 so the legacy FindBoost.cmake (module mode) is used.
    # Our Boost is built with b2 which does not install BoostConfig.cmake.
    (
        r'cmake_policy\s*\(\s*SET\s+CMP0167\s+NEW\s*\)',
        r'cmake_policy(SET CMP0167 OLD)',
        0,
    ),
])

# =============================================================================
# No guard needed here either: add_subdirectory(slic3r) is already inside
# `if(SLIC3R_GUI)`, which WASM builds force OFF.
# libnoise: no longer excluded from WASM — Findlibnoise.cmake provides
# the WASM-compiled noise::noise target via CMAKE_MODULE_PATH.
# =============================================================================
# 3. GCode.hpp — fix narrowing in LayerResult::make_nop_layer_result
#    On 32-bit WASM, size_t is uint32_t; std::numeric_limits<coord_t>::max()
#    = INT64_MAX cannot narrow to size_t → [-Wc++11-narrowing] compile error.
# =============================================================================
patch("src/libslic3r/GCode.hpp", [
    (
        r'(\{"",\s*)std::numeric_limits<coord_t>::max\(\)',
        r'\1static_cast<size_t>(-1)',
        0,
    ),
])

# =============================================================================
# NOTE: §3b (Model.cpp read_from_step guard) was removed.
# OCCT is now compiled into the WASM engine; read_from_step compiles normally.
# =============================================================================
# 4. src/libslic3r/CMakeLists.txt — make OCCT / OpenCV / draco conditional
# =============================================================================
patch("src/libslic3r/CMakeLists.txt", [
    # Add SLIC3R_WASM compile definitions
    # SLIC3R_NO_OCCT is intentionally absent: OCCT is now compiled into the
    # WASM engine (see the "Build WASM deps — OCCT" step in
    # .github/workflows/build-wasm.yml, or build-local-wsl.sh for local
    # builds) and the real Format/STEP.cpp is used.
    (
        r'(target_compile_definitions\s*\(\s*libslic3r\s+PUBLIC)',
        r'\1\n  $<$<BOOL:${SLIC3R_WASM}>:SLIC3R_WASM;SLIC3R_NO_OPENVDB;SLIC3R_NO_OPENCV>',
        1,
    ),
    # OpenCASCADE find/link: no WASM guard — our cmake/FindOpenCASCADE.cmake
    # handles both native and WASM installs via OCCT_WASM_DIR.
    # (The two guard patterns that previously wrapped find_package and OCCT_LIBS
    # with if(NOT SLIC3R_WASM) have been removed.)
    #
    # OCCT 7.8 consolidated the STEP DataExchange toolkits — TKXDESTEP, TKSTEP,
    # TKSTEP209, TKSTEPAttr and TKSTEPBase were all merged into a single
    # TKDESTEP. Older OrcaSlicer OCCT_LIBS may still list the old 7.7 names, so
    # against our OCCT 7.8.1 the link fails with "unable to find library
    # -lTKSTEP" (these are not imported targets, so they fall back to bare -l).
    # Collapse the five obsolete entries to TKDESTEP (an imported target that
    # pulls its transitive deps by full path). expected=0 so this is a no-op if
    # OrcaSlicer already uses TKDESTEP.
    (
        r'TKXDESTEP\s+TKSTEP\s+TKSTEP209\s+TKSTEPAttr\s+TKSTEPBase',
        r'TKDESTEP',
        0,
    ),
    # No guard needed for `opencv_world` or draco: cmake/FindOpenCV.cmake
    # and Finddraco.cmake already stub `opencv_world` and `draco::draco` as
    # empty INTERFACE targets and set *_FOUND, and this repo's WASM build
    # always prepends cmake/ to CMAKE_MODULE_PATH, so those stubs are
    # the only Find modules that can ever resolve here. (OpenVDB's genexpr
    # guard below is kept — it interacts with the Layer 2 override injection
    # in a way these two don't.)
    # Wrap OpenVDB link
    (
        r'(OpenVDB::openvdb)',
        r'$<$<NOT:$<BOOL:${SLIC3R_WASM}>>:OpenVDB::openvdb>',
        0,
    ),
    # JPEG: embuilder pre-builds libjpeg into the Emscripten sysroot, so
    # find_package(JPEG) finds it automatically — no WASM guard needed.
    # No guard needed for FREETYPE_LIBRARIES either: cmake/FindFreetype.cmake
    # already stubs it to an empty INTERFACE target, so linking it
    # unconditionally is inert under WASM.
    # fontconfig (Linux non-WASM only)
    (
        r'(target_link_libraries\s*\(\s*libslic3r\s+PRIVATE\s+fontconfig\s*\))',
        r'if(NOT SLIC3R_WASM)\n    \1\nendif()',
        0,
    ),
    # noise::noise — no longer excluded from WASM; links against the
    # WASM-compiled libnoise provided by Findlibnoise.cmake.
    # encoding_check() runs a native binary — would fail on WASM runner
    (
        r'(encoding_check\s*\([^)]*\))',
        r'if(NOT SLIC3R_WASM)\n\1\nendif()',
        0,
    ),
])

# =============================================================================
# 3c. FuzzySkin.cpp — thread_local compatibility
#     Emscripten single-threaded mode may not support thread_local or
#     std::this_thread without -sUSE_PTHREADS; replace with static equivalents.
#     If a future OrcaSlicer version writes "static thread_local" instead of
#     bare `thread_local`, the regex below will produce "static static" and
#     fail loudly at compile time rather than silently miscompiling.
# =============================================================================
patch("src/libslic3r/Feature/FuzzySkin/FuzzySkin.cpp", [
    (
        r'\bthread_local\b',
        r'static',
        1,
    ),
    (
        r'rd\.entropy\(\)\s*>\s*0\s*\?\s*rd\(\)\s*:\s*std::hash<std::thread::id>\(\)\(std::this_thread::get_id\(\)\)',
        r'rd()',
        1,
    ),
])

# =============================================================================
# 4a. AABBTreeLines.hpp — Eigen template deduction fix
#     origin.cast<Scalar>() returns a lazy CwiseUnaryOp that does not match
#     Eigen::Matrix in deduction; explicitly construct with decltype(nearest_point).
# =============================================================================
patch("src/libslic3r/AABBTreeLines.hpp", [
    (
        r'(distance_to_squared\(line,\s*)(origin\.template cast<typename LineType::Scalar>\(\))(?:\.eval\(\))?',
        r'\1decltype(nearest_point)(\2)',
        0,
    ),
])

# =============================================================================
# 4b. Override headers — copy into orca/ source tree
#     GCC/Clang search the *including file's* directory before any -I path
#     for #include "..." directives, so the only reliable way to override a
#     header that lives next to its includer is to physically replace it.
#     The canonical stub content stays in overrides/.
# NOTE: Format/STEP.hpp is NOT overridden — the real OrcaSlicer header is used
#       now that OCCT is compiled into the engine.
# =============================================================================
copy_override("src/libslic3r/OpenVDBUtils.hpp")
copy_override("src/libslic3r/ObjColorUtils.hpp")

# =============================================================================
# 4c. src/libslic3r/CMakeLists.txt — inject WASM override sources
#     Appended (not inline-patched) so it survives minor upstream reshuffles.
#     Marks original stub-only files as HEADER_FILE_ONLY (not compiled) and
#     adds the clean override files from overrides/.
#     Also injects the overrides include path at higher priority than orca/src/
#     so that #include "Format/STEP.hpp" etc. find our stubs first.
# =============================================================================
LIBSLIC3R_OVERRIDES_INJECTION = """\

# ── WASM overrides (injected by patches/apply.py) ─────────────────────────────
# Original C++ stub files are excluded from compilation; clean override
# implementations from overrides/ are added instead.
# This keeps the orca/ source tree free of C++ modifications.
if(SLIC3R_WASM AND DEFINED ORCA_WEB_OVERRIDES_DIR)
  # Files always present in OrcaSlicer
  # NOTE: Format/STEP.cpp is NOT stubbed — the real implementation is compiled
  #       with OCCT (see the "Build WASM deps — OCCT" step in
  #       .github/workflows/build-wasm.yml, or build-local-wsl.sh locally).
  set(_wasm_orig_stubs
    "${CMAKE_CURRENT_SOURCE_DIR}/Format/DRC.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Format/svg.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/OpenVDBUtils.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ObjColorUtils.cpp"
  )
  # Files that may be reorganised in future OrcaSlicer versions
  set(_wasm_maybe_stubs
    "${CMAKE_CURRENT_SOURCE_DIR}/SLA/Hollowing.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Shape/TextShape.cpp"
  )
  foreach(_f IN LISTS _wasm_maybe_stubs)
    if(EXISTS "${_f}")
      list(APPEND _wasm_orig_stubs "${_f}")
    endif()
  endforeach()
  set_source_files_properties(${_wasm_orig_stubs} PROPERTIES HEADER_FILE_ONLY TRUE)

  target_sources(libslic3r PRIVATE
    "${ORCA_WEB_OVERRIDES_DIR}/src/libslic3r/Format/DRC.cpp"
    "${ORCA_WEB_OVERRIDES_DIR}/src/libslic3r/Format/svg.cpp"
    "${ORCA_WEB_OVERRIDES_DIR}/src/libslic3r/OpenVDBUtils.cpp"
    "${ORCA_WEB_OVERRIDES_DIR}/src/libslic3r/ObjColorUtils.cpp"
    "${ORCA_WEB_OVERRIDES_DIR}/src/libslic3r/SLA/Hollowing.cpp"
    "${ORCA_WEB_OVERRIDES_DIR}/src/libslic3r/Shape/TextShape.cpp"
  )

endif()
"""

_libslic3r_cmake = ORCA / "src/libslic3r/CMakeLists.txt"
if _libslic3r_cmake.exists():
    _lc = _libslic3r_cmake.read_text(encoding="utf-8")
    _marker = "# ── WASM overrides"
    if _marker not in _lc:
        if not DRY_RUN:
            _libslic3r_cmake.write_text(_lc + LIBSLIC3R_OVERRIDES_INJECTION, encoding="utf-8")
        print(f"  {'WOULD PATCH' if DRY_RUN else 'PATCHED'}: src/libslic3r/CMakeLists.txt (overrides injection)")
    else:
        # Already injected — check content matches current template.
        # If apply.py changed (e.g. FuzzySkin removed from injection), the old
        # injected block must be replaced so stale overrides don't linger in
        # local orca/ checkouts across apply.py updates.
        _inject_start = _lc.index(_marker)
        if _lc[_inject_start:].rstrip() != LIBSLIC3R_OVERRIDES_INJECTION.rstrip():
            _lc_updated = _lc[:_inject_start].rstrip() + "\n" + LIBSLIC3R_OVERRIDES_INJECTION
            if not DRY_RUN:
                _libslic3r_cmake.write_text(_lc_updated, encoding="utf-8")
            print(f"  {'WOULD UPDATE' if DRY_RUN else 'UPDATED'}: src/libslic3r/CMakeLists.txt (overrides injection changed)")
        else:
            print("  OK (no change): src/libslic3r/CMakeLists.txt (overrides injection)")

# =============================================================================
# 5. Platform.cpp — suppress unknown-platform static_assert
#    Emscripten is not in OrcaSlicer's known-platform list.
# =============================================================================
patch("src/libslic3r/Platform.cpp", [
    (
        r'(static_assert\s*\(\s*false\s*,\s*"Unknown platform detected"\s*\);)',
        r'#ifndef SLIC3R_WASM\n    \1\n#endif',
        0,
    ),
])

# =============================================================================
# 6. GCode/Thumbnails.cpp — JPEG compatibility for Emscripten
#    Replace compress_thumbnail_jpg body to strip alpha before compressing
#    (Emscripten ships standard IJG libjpeg, not libjpeg-turbo; JCS_EXT_RGBA
#    at value 13 is not a valid input colour space in standard libjpeg).
# =============================================================================

_thumb_cpp = ORCA / "src/libslic3r/GCode/Thumbnails.cpp"
if _thumb_cpp.exists():
    _tc = _thumb_cpp.read_text(encoding="utf-8", errors="replace")
    if "// WASM: strip alpha" not in _tc:
        _m = re.search(
            r'std::unique_ptr<CompressedImageBuffer>\s+compress_thumbnail_jpg\s*\([^)]*\)\s*',
            _tc,
        )
        if _m:
            _brace_start = _tc.index('{', _m.end())
            _depth = 0
            _body_end = _brace_start
            for _i in range(_brace_start, len(_tc)):
                if _tc[_i] == '{':
                    _depth += 1
                elif _tc[_i] == '}':
                    _depth -= 1
                    if _depth == 0:
                        _body_end = _i + 1
                        break
            # Replace RGBA+JCS_EXT_RGBA path with a standard-libjpeg RGB path.
            # Emscripten's IJG libjpeg does not support 4-component input — convert
            # RGBA→RGB (dropping alpha) and use JCS_RGB instead.
            _new_body = (
                "{\n"
                "    // WASM: strip alpha channel; Emscripten's libjpeg is standard IJG\n"
                "    // (no JCS_EXT_RGBA support), so convert RGBA → RGB before compressing.\n"
                "    const unsigned int in_row  = data.width * 4;\n"
                "    const unsigned int out_row = data.width * 3;\n"
                "    std::vector<unsigned char> rgb(data.height * out_row);\n"
                "    for (unsigned int y = 0; y < data.height; ++y) {\n"
                "        const unsigned char* s = data.pixels.data() + (data.height - 1 - y) * in_row;\n"
                "        unsigned char*       d = rgb.data() + y * out_row;\n"
                "        for (unsigned int x = 0; x < data.width; ++x, s += 4, d += 3) {\n"
                "            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];\n"
                "        }\n"
                "    }\n"
                "    std::vector<unsigned char*> rows(data.height);\n"
                "    for (unsigned int y = 0; y < data.height; ++y)\n"
                "        rows[y] = rgb.data() + y * out_row;\n"
                "\n"
                "    unsigned char* jbuf = nullptr;\n"
                "    unsigned long  jsz  = 0;\n"
                "    jpeg_error_mgr       jerr;\n"
                "    jpeg_compress_struct cinfo;\n"
                "    cinfo.err = jpeg_std_error(&jerr);\n"
                "    jpeg_create_compress(&cinfo);\n"
                "    jpeg_mem_dest(&cinfo, &jbuf, &jsz);\n"
                "    cinfo.image_width      = data.width;\n"
                "    cinfo.image_height     = data.height;\n"
                "    cinfo.input_components = 3;\n"
                "    cinfo.in_color_space   = JCS_RGB;\n"
                "    jpeg_set_defaults(&cinfo);\n"
                "    jpeg_set_quality(&cinfo, 85, TRUE);\n"
                "    jpeg_start_compress(&cinfo, TRUE);\n"
                "    jpeg_write_scanlines(&cinfo, rows.data(), data.height);\n"
                "    jpeg_finish_compress(&cinfo);\n"
                "    jpeg_destroy_compress(&cinfo);\n"
                "\n"
                "    auto out = std::make_unique<CompressedJPG>();\n"
                "    out->data = jbuf;  // malloc-allocated by libjpeg\n"
                "    out->size = size_t(jsz);\n"
                "    return out;\n"
                "}"
            )
            _tc_new = _tc[:_brace_start] + _new_body + _tc[_body_end:]
            if not DRY_RUN:
                _thumb_cpp.write_text(_tc_new, encoding="utf-8")
            print(f"  {'WOULD PATCH' if DRY_RUN else 'PATCHED'}: src/libslic3r/GCode/Thumbnails.cpp (rgba→rgb jpeg)")
        else:
            print("  WARN: compress_thumbnail_jpg not found in Thumbnails.cpp")
    else:
        print("  OK (no change): src/libslic3r/GCode/Thumbnails.cpp (jpeg patch)")

# =============================================================================
# 7. utils.cpp — single-threaded Boost.Log compatibility
#    Boost is built with BOOST_LOG_NO_THREADS so the MT sink type and
#    current_thread_id attribute don't exist.
# =============================================================================
patch("src/libslic3r/utils.cpp", [
    (
        r'boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>',
        r'boost::log::sinks::unlocked_sink<boost::log::sinks::text_file_backend>',
        0,
    ),
    (
        r'<<\s*"\[Thread\s*"\s*<<\s*expr::attr<attrs::current_thread_id::value_type>\("ThreadID"\)\s*<<\s*"\]"',
        r'',
        0,
    ),
])

# =============================================================================
# 8. Arachne/SkeletalTrapezoidation.cpp — guard a known degenerate case
#    getOrCreateBeading() has a comment acknowledging "This bug is due to too
#    small central edges": when a node has no incident edges to measure a
#    distance from, `dist` is left at numeric_limits<coord_t>::max() and only
#    an assert() (stripped in our -DNDEBUG Release build) guards it before
#    `dist * 2` overflows into beading_strategy.getOptimalBeadCount(). The
#    garbage bead_count then drives out-of-bounds access in
#    propagateBeadingsDownward()/interpolate() downstream — reproducible with
#    ordinary real-world meshes (Voron Design Cube v7, Stanford Bunny, and a
#    1.1M-triangle model) as a WASM "memory access out of bounds" trap, which
#    a native release build likely tolerates silently instead of catching.
#    Fall back to a safe minimum bead_count instead of proceeding with the
#    overflowed distance.
# =============================================================================
patch("src/libslic3r/Arachne/SkeletalTrapezoidation.cpp", [
    (
        r'assert\(dist != std::numeric_limits<coord_t>::max\(\)\);\s*\n(\s*)node->data\.bead_count = beading_strategy\.getOptimalBeadCount\(dist \* 2\);',
        r'assert(dist != std::numeric_limits<coord_t>::max());\n'
        r'\1if (dist == std::numeric_limits<coord_t>::max()) {\n'
        r'\1    // Degenerate node (no incident edges to measure) — see comment above\n'
        r'\1    // getOrCreateBeading(). Avoid overflowing `dist * 2` into getOptimalBeadCount().\n'
        r'\1    BOOST_LOG_TRIVIAL(warning) << "SkeletalTrapezoidation: degenerate node with no measurable distance, using bead_count=1";\n'
        r'\1    node->data.bead_count = 1;\n'
        r'\1} else {\n'
        r'\1    node->data.bead_count = beading_strategy.getOptimalBeadCount(dist * 2);\n'
        r'\1}',
        1,
    ),
])

# =============================================================================
# 8c. WallToolPaths.cpp — guard shorterThan() against an empty shape
#     Found via UBSan (see section 8b): shorterThan() takes `&shape.back()`
#     unconditionally. Called from removeSmallLines() on each ExtrusionLine
#     in a toolpath; degenerate/thin real-world geometry (Voron Design Cube
#     v7, Stanford Bunny) can produce a line with zero junctions, so
#     `.back()` on the empty vector is undefined behavior — WASM traps on it
#     as "memory access out of bounds"; a native build likely reads garbage
#     instead. An empty shape has zero length, which is trivially "shorter
#     than" any positive check_length, so returning true early is both safe
#     and the semantically correct answer, not just a guard.
# =============================================================================
patch("src/libslic3r/Arachne/WallToolPaths.cpp", [
    (
        r'(template<typename T> bool shorterThan\(const T &shape, const coord_t check_length\)\s*\n\{\s*\n)(\s*)(const auto \*p0)',
        r'\1\2if (shape.empty())\n'
        r'\2    // Empty shape: zero length is always shorter than check_length.\n'
        r'\2    return true;\n'
        r'\2\3',
        1,
    ),
])

# =============================================================================
# 8d. SkeletalTrapezoidation.cpp — guard interpolate() against an empty
#     toolpath_locations vector
#     interpolate() (the 4-arg overload, called directly from
#     propagateBeadingsDownward — the exact function in the crash's named
#     stack trace) computes `left.toolpath_locations.size() - 1` into a
#     signed coord_t without checking for empty() first: size_t(0) - 1
#     underflows before the (implementation-defined, not truly UB, but a
#     smell) narrowing conversion back to coord_t. The 3-arg interpolate()
#     overload below it then writes `ret.toolpath_locations[inset_idx]` /
#     `ret.bead_widths[inset_idx]` for inset_idx up to
#     min(left,right).bead_widths.size() — if `ret` (a copy of whichever of
#     left/right has larger total_thickness) has a smaller toolpath_locations
#     than that bound, it's an out-of-bounds write. Guard both.
# =============================================================================
patch("src/libslic3r/Arachne/SkeletalTrapezoidation.cpp", [
    (
        r'(coord_t next_inset_idx;\n    for \(next_inset_idx = left\.toolpath_locations\.size\(\) - 1; next_inset_idx >= 0; next_inset_idx--\))',
        r'if (left.toolpath_locations.empty())\n'
        r'    { // Nothing to search — behave as if no next inset was found.\n'
        r'        return ret;\n'
        r'    }\n'
        r'    \1',
        1,
    ),
    (
        r'(for \(size_t inset_idx = 0; inset_idx < std::min\(left\.bead_widths\.size\(\), right\.bead_widths\.size\(\)\); inset_idx\+\+\)\n    \{)',
        r'for (size_t inset_idx = 0; inset_idx < std::min({left.bead_widths.size(), right.bead_widths.size(), ret.bead_widths.size(), ret.toolpath_locations.size()}); inset_idx++)\n    {',
        1,
    ),
])

# =============================================================================
# 8e. WallToolPaths.cpp — skip removeSmallLines()'s shorterThan() check for
#     a junction-less ExtrusionLine
#     Found via UBSan after fixing 8c/8d: with those in place, the crash
#     moves to a precise, named location — WallToolPaths.cpp:696 (in the
#     as-patched file; effectively the shorterThan() call in
#     removeSmallLines()) — "runtime error: -nan is outside the range of
#     representable values of type 'long long'". min_width is left at its
#     numeric_limits<coord_t>::max() sentinel when `line` has zero
#     junctions (the for-loop over `line` never executes), and that
#     sentinel then flows into `min_width / 2` / `min_width *
#     min_length_factor`, producing a NaN/overflow when converted back to
#     coord_t for shorterThan()'s check_length parameter. A junction-less
#     line has nothing to measure a minimum width from, so skip the
#     removal check for it rather than compute with the sentinel.
# =============================================================================
patch("src/libslic3r/Arachne/WallToolPaths.cpp", [
    (
        r'(coord_t        min_width = std::numeric_limits<coord_t>::max\(\);\n            for \(const ExtrusionJunction &j : line\)\n                min_width = std::min\(min_width, j\.w\);\n)(\s*)(// Only use min_length_factor)',
        r'\1\2if (min_width == std::numeric_limits<coord_t>::max()) continue; // junction-less line: nothing to measure\n'
        r'\2\3',
        1,
    ),
])

# =============================================================================
# 8f. WallToolPaths.hpp — default-initialize WallToolPathsParams fields
#     Next crash after 8e (same UBSan location, unchanged): min_length_factor
#     itself DOES have a documented fallback in make_paths_params() (0.5f),
#     but min_bead_width, min_feature_size and wall_transition_length are
#     only conditionally assigned there with NO else-fallback — if the
#     corresponding print_object_config option isn't present (our headless
#     WASM build ships no bundled profile JSON, unlike a real OrcaSlicer
#     install), those fields stay uninitialized garbage. Rather than chase
#     exactly which uninitialized field's garbage bit pattern produces the
#     observed NaN in this particular build, default-initialize the whole
#     struct to the same values make_paths_params() already documents as
#     intended fallbacks (or 0/false for the ones with none documented) —
#     a normal, idiomatic fix that only changes behavior for previously-UB
#     construction paths.
#
#     The four percent-of-nozzle-diameter fields (min_bead_width,
#     min_feature_size, wall_transition_length, wall_transition_filter_
#     deviation) were originally zeroed here — that's NOT what "no override
#     present" means upstream. PrintConfig.cpp defines real, deliberately
#     non-zero defaults for these (min_bead_width 85%, min_feature_size 25%,
#     wall_transition_length 100%, wall_transition_filter_deviation 25%, all
#     as a percentage of nozzle diameter; wall_transition_angle 10 degrees
#     flat, not percent-based). Zero for wall_transition_length/angle in
#     particular tells Arachne to allow a wall-count transition at every
#     infinitesimal width change instead of upstream's intended ~1-nozzle-
#     width/10-degree smoothing window — on simple geometry (no width
#     variation) this is inert, but zero values would tell Arachne to allow
#     a wall-count transition at every infinitesimal width change instead of
#     upstream's intended ~1-nozzle-width/10-degree smoothing window. Every
#     printer profile this app ships uses a 0.4mm nozzle (see
#     src/data/orca-profiles.json), so hardcode each percent-based default
#     as its absolute-mm equivalent for 0.4mm rather than 0.
#
#     NOTE (2026-07-07): this patch was originally believed to also fix the
#     Voron Design Cube multi-minute hang. It does not — the hang reproduces
#     with these exact params passed explicitly through the config (and
#     orc_init starts from FullPrintConfig defaults anyway, so
#     make_paths_params() never actually sees a missing option). The real
#     cause was Boost.Log's broken default sink amplifying Arachne's
#     per-edge warning storm — see the DisableBoostLogOnInit comment in
#     bridge/slicer.cpp. This patch stays as defensive
#     initialization for genuinely-uninitialized construction paths.
# =============================================================================
patch("src/libslic3r/Arachne/WallToolPaths.hpp", [
    (
        r'float   min_bead_width;\n    float   min_feature_size;\n    float   min_length_factor;\n    float   wall_transition_length;\n    float   wall_transition_angle;\n    float   wall_transition_filter_deviation;\n    int     wall_distribution_count;\n    bool    is_top_or_bottom_layer;',
        r'float   min_bead_width = 0.34f;\n'
        r'    float   min_feature_size = 0.1f;\n'
        r'    float   min_length_factor = 0.5f;\n'
        r'    float   wall_transition_length = 0.4f;\n'
        r'    float   wall_transition_angle = 10.f;\n'
        r'    float   wall_transition_filter_deviation = 0.1f;\n'
        r'    int     wall_distribution_count = 1;\n'
        r'    bool    is_top_or_bottom_layer = false;',
        1,
    ),
])
verify_contains(
    "src/libslic3r/Arachne/WallToolPaths.hpp",
    "float   min_bead_width = 0.34f;",
    "WallToolPathsParams default-init patch (8f) did not apply — the Arachne "
    "uninitialized-struct crash this fixes may have regressed (upstream "
    "OrcaSlicer may have reformatted this struct; update the regex above).",
)

# =============================================================================
# 8g. Thread.cpp — stub thread naming on Emscripten instead of linking a
#     real pthread_setname_np
#     Thread.cpp's generic "posix" branch (the #else after the __APPLE__
#     special-case) calls pthread_setname_np()/pthread_getname_np()
#     unconditionally on any non-Windows/non-Apple platform, which includes
#     Emscripten (it defines the usual posix macros). This build is
#     single-threaded (no real pthreads), so these symbols
#     don't exist; normal Release linking has so far gotten away with it
#     because nothing reachable from an ordinary slice calls set_thread_name()
#     — wasm-ld's --gc-sections silently drops the whole function, symbol and
#     all, before it ever needs to resolve. That's fragile, not fixed: adding
#     -fsanitize=undefined (for UBSan diagnostic builds — see
#     .github/workflows/build-wasm-debug.yml) changes what the linker keeps
#     live, and the same dead code becomes a real "undefined symbol:
#     pthread_setname_np" link failure. Give Emscripten its own no-op branch
#     (matching the existing __APPLE__ "not supported" pattern immediately
#     above it) so the symbol is never referenced at all, regardless of what
#     the linker decides to keep.
# =============================================================================
patch("src/libslic3r/Thread.cpp", [
    (
        r'#else\n\n// posix\nbool set_thread_name\(std::thread &thread, const char \*thread_name\)\n\{\n   \tpthread_setname_np\(thread\.native_handle\(\), thread_name\);',
        r'#elif defined(__EMSCRIPTEN__)\n\n'
        r'// Single-threaded WASM build — no real pthread_setname_np.\n'
        r'// Thread naming is a debugging aid only; no-op rather than link against\n'
        r'// a symbol this build does not provide.\n'
        r'bool set_thread_name(std::thread &thread, const char *thread_name)\n'
        r'{\n'
        r'\treturn false;\n'
        r'}\n\n'
        r'bool set_thread_name(boost::thread &thread, const char *thread_name)\n'
        r'{\n'
        r'\treturn false;\n'
        r'}\n\n'
        r'bool set_current_thread_name(const char *thread_name)\n'
        r'{\n'
        r'\treturn false;\n'
        r'}\n\n'
        r'std::optional<std::string> get_current_thread_name()\n'
        r'{\n'
        r'\treturn std::nullopt;\n'
        r'}\n\n'
        r'#else\n\n// posix\nbool set_thread_name(std::thread &thread, const char *thread_name)\n{\n   \tpthread_setname_np(thread.native_handle(), thread_name);',
        1,
    ),
])
verify_contains(
    "src/libslic3r/Thread.cpp",
    "defined(__EMSCRIPTEN__)",
    "Thread.cpp Emscripten thread-naming stub (8g) did not apply — upstream "
    "OrcaSlicer may have reformatted this file; update the regex above.",
)

# =============================================================================
# 8h. Forward native WebAssembly EH flags into OrcaSlicer's dependency builds.
#     CMake's ExternalProject helper omits parent compile flags by default.
#     Preserve the wasm32 native-EH C/C++ flags in all child builds and replace
#     oneTBB's incompatible Emscripten `-fexceptions` override.
# =============================================================================
patch("deps/CMakeLists.txt", [
    (
        r'(?m)^([ \t]*-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=\$\{CMAKE_CXX_COMPILER_LAUNCHER\})(?!\n[ \t]*"-DCMAKE_C_FLAGS:STRING=\$\{CMAKE_C_FLAGS\}")$',
        r'\1\n            "-DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS}"\n            "-DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS}"',
        0,
    ),
])
if not DRY_RUN:
    verify_contains(
        "deps/CMakeLists.txt",
        '"-DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS}"',
        "OrcaSlicer's child builds do not inherit wasm longjmp C flags.",
    )
    verify_contains(
        "deps/CMakeLists.txt",
        '"-DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS}"',
        "OrcaSlicer's child builds do not inherit native WebAssembly EH CXX flags.",
    )

patch("deps/TBB/TBB.cmake", [
    (
        r'(?s)^if \(FLATPAK AND "\$\{CMAKE_CXX_COMPILER_ID\}" STREQUAL "GNU"\)\s*'
        r'set\(_patch_command .*?\)\s*else\(\)\s*set\(_patch_command ""\)\s*endif\(\)',
        'if (EMSCRIPTEN)\n'
        '    set(_patch_command ${CMAKE_COMMAND} -DTBB_SOURCE_DIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/wasm-exceptions.cmake)\n'
        'elseif (FLATPAK AND "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")\n'
        '    set(_patch_command ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/GNU.cmake ./cmake/compilers/GNU.cmake)\n'
        'else()\n'
        '    set(_patch_command "")\n'
        'endif ()',
        0,
    ),
])
_tbb_cmake = ORCA / "deps" / "TBB" / "TBB.cmake"
if _tbb_cmake.exists():
    _tbb_text = _tbb_cmake.read_text(encoding="utf-8")
    if "${CMAKE_CURRENT_LIST_DIR}/wasm-exceptions.cmake" not in _tbb_text:
        if DRY_RUN:
            print("  WOULD VERIFY: oneTBB native-EH patch command after patch")
        else:
            _msg = "oneTBB native-EH patch command was not configured"
            print(f"FATAL: {_msg}", file=sys.stderr)
            sys.exit(1)

    _tbb_patch = _tbb_cmake.parent / "wasm-exceptions.cmake"
    _tbb_patch_source = Path(__file__).resolve().parent / "onetbb" / "wasm-exceptions.cmake"
    _tbb_patch_text = _tbb_patch_source.read_text(encoding="utf-8")
    if not _tbb_patch.exists() or _tbb_patch.read_text(encoding="utf-8") != _tbb_patch_text:
        if DRY_RUN:
            print("  WOULD WRITE: deps/TBB/wasm-exceptions.cmake")
        else:
            _tbb_patch.write_text(_tbb_patch_text, encoding="utf-8")
            print("  WROTE: deps/TBB/wasm-exceptions.cmake")

# =============================================================================
# 9. Root CMakeLists.txt — append the bridge + WASM link target
# =============================================================================
BRIDGE_INJECTION = """\

# ── WASM bridge (injected by patches/apply.py) ─────────────────────────────────
if(SLIC3R_WASM AND DEFINED ORCA_WEB_BRIDGE_DIR)
  # Expose OrcaSlicer src/ as ORCA_SRC for the bridge CMakeLists.
  set(ORCA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  add_subdirectory("${ORCA_WEB_BRIDGE_DIR}" bridge)
  add_subdirectory("${ORCA_WEB_WASM_DIR}"   wasm)
endif()
"""

_orca_root_cmake = ORCA / "CMakeLists.txt"
if _orca_root_cmake.exists():
    _content = _orca_root_cmake.read_text(encoding="utf-8")
    if "WASM bridge (injected by patches/apply.py)" not in _content:
        if not DRY_RUN:
            _orca_root_cmake.write_text(_content + BRIDGE_INJECTION, encoding="utf-8")
        print(f"  {'WOULD PATCH' if DRY_RUN else 'PATCHED'}: CMakeLists.txt (bridge injection)")
    else:
        print("  OK (no change): CMakeLists.txt (bridge injection)")

# =============================================================================
if __name__ == "__main__":
    print("\norca-wasm WASM patcher — done.\n")
