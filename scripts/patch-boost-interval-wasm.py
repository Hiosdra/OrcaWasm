#!/usr/bin/env python3
"""Make Boost.Interval headers parse on Emscripten without fake fenv support.

CGAL's surface-sweep umbrella header includes algebraic-kernel templates that
mention Boost.Interval even when this build only instantiates segment traits.
WebAssembly has no floating-point environment, so this patch lets those
templates parse but makes any actual Boost.Interval arithmetic fail at compile
time.  CGAL itself is built with its supported round-to-nearest interval mode.
"""

import argparse
import sys
from pathlib import Path


OLD_ROUNDING_BLOCK = """#if defined(BOOST_NUMERIC_INTERVAL_NO_HARDWARE) && !defined(BOOST_NO_FENV_H)
#  include <boost/numeric/interval/detail/c99_rounding_control.hpp>
#endif

#if defined(BOOST_NUMERIC_INTERVAL_NO_HARDWARE)
#  undef BOOST_NUMERIC_INTERVAL_NO_HARDWARE
#  error Boost.Numeric.Interval: Please specify rounding control mechanism.
#endif"""
NEW_ROUNDING_BLOCK = """#if defined(BOOST_NUMERIC_INTERVAL_NO_HARDWARE) && defined(__EMSCRIPTEN__)
#  include <boost/numeric/interval/detail/emscripten_rounding_unavailable.hpp>
#  undef BOOST_NUMERIC_INTERVAL_NO_HARDWARE
#elif defined(BOOST_NUMERIC_INTERVAL_NO_HARDWARE) && !defined(BOOST_NO_FENV_H)
#  include <boost/numeric/interval/detail/c99_rounding_control.hpp>
#endif
#if defined(BOOST_NUMERIC_INTERVAL_NO_HARDWARE)
#  undef BOOST_NUMERIC_INTERVAL_NO_HARDWARE
#  error Boost.Numeric.Interval: Please specify rounding control mechanism.
#endif"""
OLD_SPECIALIZATIONS = """template<>
struct rounded_math<float>
  : save_state<rounded_arith_opp<float> >
{};

template<>
struct rounded_math<double>
  : save_state<rounded_arith_opp<double> >
{};

template<>
struct rounded_math<long double>
  : save_state<rounded_arith_opp<long double> >
{};"""
NEW_SPECIALIZATIONS = """#if defined(__EMSCRIPTEN__)
template<>
struct rounded_math<float>
  : save_state<rounded_arith_opp<float, detail::emscripten_rounding_unavailable<float> > >
{};

template<>
struct rounded_math<double>
  : save_state<rounded_arith_opp<double, detail::emscripten_rounding_unavailable<double> > >
{};

template<>
struct rounded_math<long double>
  : save_state<rounded_arith_opp<long double, detail::emscripten_rounding_unavailable<long double> > >
{};
#else
template<>
struct rounded_math<float>
  : save_state<rounded_arith_opp<float> >
{};

template<>
struct rounded_math<double>
  : save_state<rounded_arith_opp<double> >
{};

template<>
struct rounded_math<long double>
  : save_state<rounded_arith_opp<long double> >
{};
#endif"""
NEW_MARKER = "detail::emscripten_rounding_unavailable<double>"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--include-dir", type=Path, required=True)
    args = parser.parse_args()

    header = args.include_dir / "boost/numeric/interval/hw_rounding.hpp"
    if not header.is_file():
        print(f"FATAL: Boost.Interval header not found: {header}", file=sys.stderr)
        return 1

    contents = header.read_text(encoding="utf-8")
    if NEW_MARKER in contents:
        print("[boost] Boost.Interval Emscripten guard already patched")
        return 0

    guard_count = contents.count(OLD_ROUNDING_BLOCK)
    specialization_count = contents.count(OLD_SPECIALIZATIONS)
    if guard_count != 1 or specialization_count != 1:
        print(
            "FATAL: expected one upstream Boost.Interval rounding block and "
            f"specialization block in {header}; found {guard_count} and {specialization_count}",
            file=sys.stderr,
        )
        return 1

    contents = contents.replace(OLD_ROUNDING_BLOCK, NEW_ROUNDING_BLOCK)
    contents = contents.replace(OLD_SPECIALIZATIONS, NEW_SPECIALIZATIONS)
    header.write_text(contents, encoding="utf-8")
    print("[boost] disabled unsafe Boost.Interval arithmetic on Emscripten")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
