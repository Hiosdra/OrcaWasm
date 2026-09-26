#!/usr/bin/env python3
"""Remove oneTBB's legacy Emscripten exception-mode override."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def patch_one_tbb(source: Path) -> bool:
    compiler_flags = source / "cmake" / "compilers" / "Clang.cmake"
    if not compiler_flags.is_file():
        raise FileNotFoundError(f"oneTBB compiler profile not found: {compiler_flags}")

    original = compiler_flags.read_text(encoding="utf-8")
    marker = "OrcaWasm supplies -fwasm-exceptions through CMAKE_CXX_FLAGS."
    if marker in original:
        return False

    pattern = re.compile(
        r"^(\s*)set\(TBB_COMMON_COMPILE_FLAGS\s+"
        r"\$\{TBB_COMMON_COMPILE_FLAGS\}\s+-fexceptions\)\s*$",
        re.MULTILINE,
    )
    updated, count = pattern.subn(
        lambda match: f"{match.group(1)}# {marker}", original
    )
    if count != 1:
        raise RuntimeError(
            "expected exactly one oneTBB legacy -fexceptions override, "
            f"found {count} in {compiler_flags}"
        )

    compiler_flags.write_text(updated, encoding="utf-8")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("onetbb_source", type=Path)
    args = parser.parse_args()

    try:
        changed = patch_one_tbb(args.onetbb_source)
    except (FileNotFoundError, RuntimeError) as error:
        print(f"oneTBB EH patch failed: {error}", file=sys.stderr)
        return 1

    print(
        "oneTBB EH profile patched"
        if changed
        else "oneTBB EH profile already patched"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
