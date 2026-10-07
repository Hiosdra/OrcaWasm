#!/usr/bin/env python3
"""Enable Boost.Interval's C99 rounding adapter on Emscripten.

Boost 1.92 marks WebAssembly as lacking the C++11 fenv extensions.  Its
Boost.Interval header uses that broad feature macro to reject the platform,
even though Emscripten's C++17 target supplies the C99 fenv API used by the
adapter.  CGAL is compiled with CGAL_ALWAYS_ROUND_TO_NEAREST on WASM, so its
own interval arithmetic does not rely on directed hardware rounding modes.
"""

import argparse
import sys
from pathlib import Path


OLD_GUARD = (
    "#if defined(BOOST_NUMERIC_INTERVAL_NO_HARDWARE) "
    "&& !defined(BOOST_NO_FENV_H)"
)
NEW_GUARD = (
    "#if defined(BOOST_NUMERIC_INTERVAL_NO_HARDWARE) "
    "&& (!defined(BOOST_NO_FENV_H) "
    "|| (defined(__EMSCRIPTEN__) && __cplusplus >= 201103L))"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--include-dir", type=Path, required=True)
    args = parser.parse_args()

    header = args.include_dir / "boost/numeric/interval/hw_rounding.hpp"
    if not header.is_file():
        print(f"FATAL: Boost.Interval header not found: {header}", file=sys.stderr)
        return 1

    contents = header.read_text(encoding="utf-8")
    if NEW_GUARD in contents:
        print("[boost] Boost.Interval Emscripten rounding guard already patched")
        return 0

    count = contents.count(OLD_GUARD)
    if count != 1:
        print(
            f"FATAL: expected one Boost.Interval rounding guard in {header}, found {count}",
            file=sys.stderr,
        )
        return 1

    header.write_text(contents.replace(OLD_GUARD, NEW_GUARD), encoding="utf-8")
    print("[boost] enabled the C99 rounding adapter for Emscripten")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
