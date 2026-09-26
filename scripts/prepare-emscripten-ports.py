#!/usr/bin/env python3
"""Add pthread and native Wasm setjmp/longjmp variants to Emscripten 3.1.74 ports."""

from __future__ import annotations

import argparse
import os
from pathlib import Path


def replace_known_block(
    text: str, old_blocks: tuple[str, ...], new_block: str, label: str, path: Path
) -> tuple[str, bool]:
    if new_block in text:
        return text, False

    matches = [old for old in old_blocks if old in text]
    if len(matches) != 1:
        raise RuntimeError(
            f"{label}: expected exactly one known upstream/legacy block in {path}, "
            f"found {len(matches)}"
        )
    return text.replace(matches[0], new_block, 1), True


def patch_port(ports: Path, name: str) -> bool:
    path = ports / f"{name}.py"
    text = path.read_text(encoding="utf-8")

    if name == "zlib":
        port_name = "zlib"
        archive = "libz"
        needed = "USE_ZLIB"
        original_get = (
            "def needed(settings):\n"
            "  return settings.USE_ZLIB\n"
            "\n\n"
            "def get(ports, settings, shared):"
        )
        legacy_get = (
            "variants = {'zlib-mt': {'PTHREADS': 1}}\n\n\n"
            "def needed(settings):\n"
            "  return settings.USE_ZLIB\n"
            "\n\n"
            "def get_lib_name(settings):\n"
            "  return 'libz-mt.a' if settings.PTHREADS else 'libz.a'\n"
            "\n\n"
            "def get(ports, settings, shared):"
        )
        old_build = (
            "    flags = ['-Wno-deprecated-non-prototype']\n"
            "    ports.build_port(source_path, final, 'zlib', srcs=srcs, flags=flags)\n"
            "\n"
            "  return [shared.cache.get_lib('libz.a', create, what='port')]"
        )
        legacy_build = (
            "    flags = ['-Wno-deprecated-non-prototype']\n"
            "    if settings.PTHREADS:\n"
            "      flags += ['-pthread']\n"
            "    ports.build_port(source_path, final, 'zlib', srcs=srcs, flags=flags)\n"
            "\n"
            "  return [shared.cache.get_lib(get_lib_name(settings), create, what='port')]"
        )
    else:
        port_name = "libjpeg"
        archive = "libjpeg"
        needed = "USE_LIBJPEG"
        original_get = (
            "def needed(settings):\n"
            "  return settings.USE_LIBJPEG\n"
            "\n\n"
            "def get(ports, settings, shared):"
        )
        legacy_get = (
            "variants = {'libjpeg-mt': {'PTHREADS': 1}}\n\n\n"
            "def needed(settings):\n"
            "  return settings.USE_LIBJPEG\n"
            "\n\n"
            "def get_lib_name(settings):\n"
            "  return 'libjpeg-mt.a' if settings.PTHREADS else 'libjpeg.a'\n"
            "\n\n"
            "def get(ports, settings, shared):"
        )
        old_build = (
            "    ports.build_port(source_path, final, 'libjpeg', exclude_files=excludes)\n"
            "\n"
            "  return [shared.cache.get_lib('libjpeg.a', create, what='port')]"
        )
        legacy_build = (
            "    flags = []\n"
            "    if settings.PTHREADS:\n"
            "      flags += ['-pthread']\n"
            "    ports.build_port(source_path, final, 'libjpeg', exclude_files=excludes, flags=flags)\n"
            "\n"
            "  return [shared.cache.get_lib(get_lib_name(settings), create, what='port')]"
        )

    variant_block = (
        f"variants = {{\n"
        f"  '{port_name}-mt': {{'PTHREADS': 1}},\n"
        f"  '{port_name}-wasm-sjlj': {{'SUPPORT_LONGJMP': 'wasm'}},\n"
        f"  '{port_name}-mt-wasm-sjlj': {{'PTHREADS': 1, 'SUPPORT_LONGJMP': 'wasm'}},\n"
        f"}}\n\n\n"
        f"def needed(settings):\n"
        f"  return settings.{needed}\n\n\n"
        f"def get_lib_name(settings):\n"
        f"  suffix = ''\n"
        f"  if settings.PTHREADS:\n"
        f"    suffix += '-mt'\n"
        f"  if settings.SUPPORT_LONGJMP == 'wasm':\n"
        f"    suffix += '-wasm-sjlj'\n"
        f"  return f'{archive}{{suffix}}.a'\n\n\n"
        "def get(ports, settings, shared):"
    )
    text, changed_variant = replace_known_block(
        text, (original_get, legacy_get), variant_block, f"{port_name} variants", path
    )

    if port_name == "zlib":
        new_build = (
            "    flags = ['-Wno-deprecated-non-prototype']\n"
            "    if settings.PTHREADS:\n"
            "      flags += ['-pthread']\n"
            "    if settings.SUPPORT_LONGJMP == 'wasm':\n"
            "      flags += ['-sSUPPORT_LONGJMP=wasm']\n"
            "    ports.build_port(source_path, final, 'zlib', srcs=srcs, flags=flags)\n"
            "\n"
            "  return [shared.cache.get_lib(get_lib_name(settings), create, what='port')]"
        )
    else:
        new_build = (
            "    flags = []\n"
            "    if settings.PTHREADS:\n"
            "      flags += ['-pthread']\n"
            "    if settings.SUPPORT_LONGJMP == 'wasm':\n"
            "      flags += ['-sSUPPORT_LONGJMP=wasm']\n"
            "    ports.build_port(source_path, final, 'libjpeg', exclude_files=excludes, flags=flags)\n"
            "\n"
            "  return [shared.cache.get_lib(get_lib_name(settings), create, what='port')]"
        )
    text, changed_build = replace_known_block(
        text, (old_build, legacy_build), new_build, f"{port_name} build flags", path
    )

    if changed_variant or changed_build:
        path.write_text(text, encoding="utf-8")
    return changed_variant or changed_build


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--emsdk",
        type=Path,
        default=Path(os.environ.get("EMSDK", "/opt/emsdk")),
    )
    args = parser.parse_args()
    ports = args.emsdk.resolve() / "upstream" / "emscripten" / "tools" / "ports"
    if not ports.is_dir():
        raise SystemExit(f"Emscripten ports directory not found: {ports}")

    try:
        changed = [name for name in ("zlib", "libjpeg") if patch_port(ports, name)]
    except (FileNotFoundError, RuntimeError) as error:
        raise SystemExit(f"Emscripten port preparation failed: {error}") from error

    libpng = (ports / "libpng.py").read_text(encoding="utf-8")
    required_libpng_features = (
        "'libpng-wasm-sjlj'",
        "'libpng-mt-wasm-sjlj'",
        "flags.append('-sSUPPORT_LONGJMP=wasm')",
    )
    if any(feature not in libpng for feature in required_libpng_features):
        raise SystemExit("Emscripten libpng.py is missing its Wasm SjLj port variants")

    print(
        "prepared Emscripten ports for native Wasm setjmp/longjmp"
        + (": " + ", ".join(changed) if changed else " (already prepared)")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
