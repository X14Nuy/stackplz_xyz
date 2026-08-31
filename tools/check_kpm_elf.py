#!/usr/bin/env python3
"""Fail closed when a KPM ELF imports anything outside the pinned KPatch ABI."""

from __future__ import annotations

import argparse
import pathlib
import subprocess
from collections.abc import Iterable


ALLOWED_IMPORTS = frozenset(
    {
        "compat_copy_to_user",
        "hook_unwrap_remove",
        "hook_wrap",
        "kallsyms_lookup_name",
        "kf_memchr",
        "kf_memcmp",
        "kf_memcpy",
        "kf_memset",
        "kf_strcmp",
        "kf_strlen",
        "kf_strncmp",
        "kpm_safe_unload_v1",
        "kpver",
        "kver",
        "printk",
    }
)

REQUIRED_IMPORTS = frozenset(
    {
        "compat_copy_to_user",
        "hook_unwrap_remove",
        "hook_wrap",
        "kallsyms_lookup_name",
        "kpver",
        "kver",
    }
)

DIRECT_EXPORTS = {
    "compat_copy_to_user": "kernel/patch/common/utils.c",
    "hook_unwrap_remove": "kernel/base/hook.c",
    "hook_wrap": "kernel/base/hook.c",
    "kallsyms_lookup_name": "kernel/base/start.c",
    "kpm_safe_unload_v1": "kernel/patch/module/module.c",
    "kpver": "kernel/base/start.c",
    "kver": "kernel/base/start.c",
    "printk": "kernel/base/start.c",
}

REQUIRED_SECTIONS = (".kpm.info", ".kpm.init", ".kpm.ctl0", ".kpm.exit")


def parse_undefined_symbols(output: str) -> set[str]:
    imports: set[str] = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2] == "U":
            imports.add(fields[-1])
    return imports


def validate_imports(imports: set[str]) -> None:
    unexpected = sorted(imports - ALLOWED_IMPORTS)
    if unexpected:
        raise ValueError(f"unexpected import(s): {', '.join(unexpected)}")
    missing = sorted(REQUIRED_IMPORTS - imports)
    if missing:
        raise ValueError(f"missing required import(s): {', '.join(missing)}")


def validate_elf_metadata(header: str, sections: str) -> None:
    fields: dict[str, str] = {}
    for line in header.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            fields[key.strip()] = value.strip()
    expected = {
        "Class": lambda value: value == "ELF64",
        "Data": lambda value: "little endian" in value,
        "Type": lambda value: value.startswith("REL"),
        "Machine": lambda value: value == "AArch64",
    }
    for key, predicate in expected.items():
        if not predicate(fields.get(key, "")):
            raise ValueError(f"invalid KPM ELF header field: {key}")
    for section in REQUIRED_SECTIONS:
        if section not in sections:
            raise ValueError(f"missing KPM section: {section}")


def _export_declaration(symbol: str) -> tuple[pathlib.Path, str]:
    if symbol.startswith("kf_"):
        return (
            pathlib.Path("kernel/patch/ksyms/libs.c"),
            f"KP_EXPORT_SYMBOL(kfunc({symbol[3:]}));",
        )
    relative = DIRECT_EXPORTS.get(symbol)
    if relative is None:
        raise ValueError(f"no export proof rule for import: {symbol}")
    return pathlib.Path(relative), f"KP_EXPORT_SYMBOL({symbol});"


def verify_kpatch_exports(kpatch_dir: pathlib.Path, imports: Iterable[str]) -> None:
    for symbol in sorted(imports):
        relative, declaration = _export_declaration(symbol)
        source = kpatch_dir / relative
        try:
            content = source.read_text(encoding="utf-8")
        except OSError as error:
            raise ValueError(f"cannot inspect KPatch export source {source}: {error}") from error
        if declaration not in content:
            raise ValueError(f"import not exported by pinned KPatch source: {symbol}")


def _run(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout


def verify(elf: pathlib.Path, kpatch_dir: pathlib.Path, nm: str, readelf: str) -> set[str]:
    if not elf.is_file():
        raise ValueError(f"KPM ELF does not exist: {elf}")
    imports = parse_undefined_symbols(_run([nm, "-u", str(elf)]))
    validate_imports(imports)
    validate_elf_metadata(
        _run([readelf, "-h", str(elf)]),
        _run([readelf, "-S", str(elf)]),
    )
    verify_kpatch_exports(kpatch_dir, imports)
    return imports


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=pathlib.Path)
    parser.add_argument("--kpatch-dir", required=True, type=pathlib.Path)
    parser.add_argument("--nm", default="aarch64-linux-gnu-nm")
    parser.add_argument("--readelf", default="aarch64-linux-gnu-readelf")
    args = parser.parse_args()
    try:
        imports = verify(args.elf, args.kpatch_dir, args.nm, args.readelf)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"KPM ELF verification failed: {error}\n")
    print("verified KPM imports: " + " ".join(sorted(imports)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
