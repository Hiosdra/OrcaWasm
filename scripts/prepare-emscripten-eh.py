#!/usr/bin/env python3
"""Add matching WebAssembly setjmp/longjmp variants to Emscripten ports.

Emscripten 3.1.74 already provides native-EH variants for libpng. Its zlib and
libjpeg port hooks only distinguish pthreads, so extend those hooks to include
the same SUPPORT_LONGJMP=wasm setting used by mixed C/C++ builds.
"""

from __future__ import annotations

import argparse
import os
import re
import tempfile
from pathlib import Path


def atomic_write(path: Path, text: str) -> None:
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", dir=path.parent, delete=False
    ) as temporary:
        temporary.write(text)
        temporary_path = Path(temporary.name)
    os.replace(temporary_path, path)


def port_variants(name: str) -> str:
    return (
        "variants = {\n"
        f"  '{name}-mt': {{'PTHREADS': 1}},\n"
        f"  '{name}-wasm-sjlj': {{'SUPPORT_LONGJMP': 'wasm'}},\n"
        f"  '{name}-mt-wasm-sjlj': {{'PTHREADS': 1, 'SUPPORT_LONGJMP': 'wasm'}},\n"
        "}\n"
    )


def library_name(prefix: str) -> str:
    return (
        "def get_lib_name(settings):\n"
        "  suffix = ''\n"
        "  if settings.PTHREADS:\n"
        "    suffix += '-mt'\n"
        "  if settings.SUPPORT_LONGJMP == 'wasm':\n"
        "    suffix += '-wasm-sjlj'\n"
        f"  return 'lib{prefix}' + suffix + '.a'\n"
    )


def add_variants(text: str, name: str, path: Path) -> str:
    desired = port_variants(name)
    assignment = re.compile(r"(?ms)^variants = \{[^\n]*\}\n|^variants = \{\n.*?^\}\n")
    found = assignment.search(text)
    if found:
        if found.group(0) == desired:
            return text
        return text[: found.start()] + desired + text[found.end() :]
    marker = "def needed(settings):\n"
    if text.count(marker) != 1:
        raise RuntimeError(f"{name} variants: expected one needed() hook in {path}")
    return text.replace(marker, desired + "\n" + marker, 1)


def add_library_name(text: str, prefix: str, path: Path) -> str:
    desired = library_name(prefix)
    function = re.compile(r"(?m)^def get_lib_name\(settings\):\n(?:[ \t].*\n)+")
    found = function.search(text)
    if found:
        return text[: found.start()] + desired + text[found.end() :]
    marker = "def get(ports, settings, shared):\n"
    if text.count(marker) != 1:
        raise RuntimeError(f"{prefix} archive selector: expected one get() hook in {path}")
    return text.replace(marker, desired + "\n" + marker, 1)


def patch_zlib(ports: Path) -> bool:
    path = ports / "zlib.py"
    original = path.read_text(encoding="utf-8")
    text = add_variants(original, "zlib", path)
    text = add_library_name(text, "z", path)
    old_compile = (
        "    flags = ['-Wno-deprecated-non-prototype']\n"
        "    if settings.PTHREADS:\n"
        "      flags += ['-pthread']\n"
        "    ports.build_port(source_path, final, 'zlib', srcs=srcs, flags=flags)"
    )
    new_compile = old_compile.replace(
        "    ports.build_port(",
        "    if settings.SUPPORT_LONGJMP == 'wasm':\n"
        "      flags += ['-sSUPPORT_LONGJMP=wasm']\n"
        "    ports.build_port(",
    )
    stock_compile = (
        "    flags = ['-Wno-deprecated-non-prototype']\n"
        "    ports.build_port(source_path, final, 'zlib', srcs=srcs, flags=flags)"
    )
    if new_compile not in text:
        if old_compile in text:
            text = text.replace(old_compile, new_compile, 1)
        elif stock_compile in text:
            text = text.replace(stock_compile, new_compile, 1)
        else:
            raise RuntimeError(f"zlib compile flags: expected one known build hook in {path}")
    text = text.replace(
        "shared.cache.get_lib('libz.a', create, what='port')",
        "shared.cache.get_lib(get_lib_name(settings), create, what='port')",
    )
    if "shared.cache.get_lib(get_lib_name(settings), create, what='port')" not in text:
        raise RuntimeError(f"zlib archive selector was not applied in {path}")
    if text == original:
        return False
    atomic_write(path, text)
    return True


def patch_libjpeg(ports: Path) -> bool:
    path = ports / "libjpeg.py"
    original = path.read_text(encoding="utf-8")
    text = add_variants(original, "libjpeg", path)
    text = add_library_name(text, "jpeg", path)
    old_compile = (
        "    flags = []\n"
        "    if settings.PTHREADS:\n"
        "      flags += ['-pthread']\n"
        "    ports.build_port(source_path, final, 'libjpeg', exclude_files=excludes, flags=flags)"
    )
    new_compile = old_compile.replace(
        "    ports.build_port(",
        "    if settings.SUPPORT_LONGJMP == 'wasm':\n"
        "      flags += ['-sSUPPORT_LONGJMP=wasm']\n"
        "    ports.build_port(",
    )
    stock_compile = "    ports.build_port(source_path, final, 'libjpeg', exclude_files=excludes)"
    if new_compile not in text:
        if old_compile in text:
            text = text.replace(old_compile, new_compile, 1)
        elif stock_compile in text:
            text = text.replace(stock_compile, new_compile, 1)
        else:
            raise RuntimeError(f"libjpeg compile flags: expected one known build hook in {path}")
    text = text.replace(
        "shared.cache.get_lib('libjpeg.a', create, what='port')",
        "shared.cache.get_lib(get_lib_name(settings), create, what='port')",
    )
    if "shared.cache.get_lib(get_lib_name(settings), create, what='port')" not in text:
        raise RuntimeError(f"libjpeg archive selector was not applied in {path}")
    if text == original:
        return False
    atomic_write(path, text)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--emsdk",
        type=Path,
        default=Path(os.environ.get("EMSDK", "/home/hiosdra/emsdk")),
    )
    args = parser.parse_args()
    ports = args.emsdk.resolve() / "upstream" / "emscripten" / "tools" / "ports"
    if not ports.is_dir():
        raise SystemExit(f"Emscripten ports directory not found: {ports}")

    changed = []
    if patch_zlib(ports):
        changed.append("zlib")
    if patch_libjpeg(ports):
        changed.append("libjpeg")
    libpng = (ports / "libpng.py").read_text(encoding="utf-8")
    for variant in ("libpng-wasm-sjlj", "libpng-mt-wasm-sjlj"):
        if f"'{variant}'" not in libpng:
            raise SystemExit(f"Pinned Emscripten libpng.py has no {variant} variant")

    print("prepared native WebAssembly EH ports: " + (", ".join(changed) if changed else "already prepared"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
