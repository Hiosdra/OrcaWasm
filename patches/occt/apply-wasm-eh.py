#!/usr/bin/env python3
"""Patch OCCT's compiler flags for Emscripten WebAssembly EH."""

from __future__ import annotations

import argparse
from pathlib import Path


ORIGINAL = """else()
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -fPIC")
  set (CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -fexceptions -fPIC")
  add_definitions(-DOCC_CONVERT_SIGNALS)
endif()
"""

PATCHED = """elseif (EMSCRIPTEN)
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fwasm-exceptions -fPIC")
  set (CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -sSUPPORT_LONGJMP=wasm -fPIC")
else()
  set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fexceptions -fPIC")
  set (CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -fexceptions -fPIC")
  add_definitions(-DOCC_CONVERT_SIGNALS)
endif()
"""

MARKER = 'set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fwasm-exceptions -fPIC")'


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source_dir", type=Path, help="root of the downloaded OCCT source tree")
    args = parser.parse_args()

    cmake_file = args.source_dir / "adm/cmake/occt_defs_flags.cmake"
    if not cmake_file.is_file():
        parser.error(f"OCCT compiler flags file not found: {cmake_file}")

    contents = cmake_file.read_text(encoding="utf-8")
    if MARKER in contents:
        print(f"[occt-patch] WebAssembly EH flags already applied: {cmake_file}")
        return 0
    if ORIGINAL not in contents:
        raise SystemExit(
            f"[occt-patch] expected OCCT {cmake_file} compiler-flags block was not found"
        )

    cmake_file.write_text(contents.replace(ORIGINAL, PATCHED, 1), encoding="utf-8")
    print(f"[occt-patch] set Emscripten EH flags and disabled signal conversion: {cmake_file}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
