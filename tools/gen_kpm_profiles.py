#!/usr/bin/env python3
"""Validate the device profile table and generate C/Go registries."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_INPUT = ROOT / "kpm" / "profiles" / "profiles.json"
DEFAULT_C_OUTPUT = ROOT / "kpm" / "generated" / "device_profiles.h"
DEFAULT_GO_OUTPUT = ROOT / "user" / "kpm" / "generated_profiles.go"

PROFILE_KEYS = {
    "id", "product", "kernel", "kpatch", "task", "cred",
    "perf_event", "debug", "layout", "maps", "symbols", "hooks", "quirks",
}
SECTION_FIELDS: dict[str, tuple[tuple[str, str], ...]] = {
    "product": (
        ("manufacturer", "string"), ("model", "string"),
        ("device", "string"), ("fingerprint", "string"),
        ("android_release", "string"), ("sdk", "u32"),
        ("security_patch", "string"),
    ),
    "kernel": (
        ("release", "string"), ("build_id", "string"),
        ("ack_commit", "string"), ("btf_sha256", "string"),
        ("page_size", "u32"), ("va_bits", "u32"),
        ("task_struct_size", "u32"), ("cred_size", "u32"),
        ("perf_event_size", "u32"), ("task_comm_len", "u32"),
        ("linux_banner_capacity", "u32"),
    ),
    "kpatch": (
        ("kpver", "string"), ("kver", "string"),
        ("sdk_version", "string"), ("sdk_commit", "string"),
        ("control_path", "string"), ("module_name", "string"),
        ("safe_unload_symbol", "string"), ("memcpy_kfunc", "string"),
        ("memset_kfunc", "string"), ("kfunc_exports_are_pointers", "bool"),
    ),
    "task": (
        ("thread_info_flags", "u32"), ("cpu", "u32"),
        ("state", "u32"), ("usage", "u32"), ("tasks", "u32"),
        ("mm", "u32"), ("active_mm", "u32"), ("exit_state", "u32"),
        ("pid", "u32"), ("tgid", "u32"), ("real_parent", "u32"),
        ("parent", "u32"), ("thread_pid", "u32"),
        ("pid_links", "u32"), ("thread_node", "u32"),
        ("start_time", "u32"), ("start_boottime", "u32"),
        ("real_cred", "u32"), ("cred", "u32"), ("comm", "u32"),
        ("signal", "u32"), ("perf_event_ctxp", "u32"),
        ("perf_event_list", "u32"), ("thread", "u32"),
        ("signal_thread_head", "u32"),
    ),
    "cred": (("uid", "u32"),),
    "perf_event": (
        ("state", "u32"), ("attr", "u32"), ("attr_bp_type", "u32"),
        ("attr_bp_addr", "u32"), ("attr_bp_len", "u32"),
        ("hw", "u32"), ("arch_address", "u32"),
        ("arch_trigger", "u32"), ("arch_ctrl", "u32"),
        ("hw_target", "u32"), ("ctx", "u32"), ("oncpu", "u32"),
        ("cpu", "u32"), ("owner", "u32"), ("context_task", "u32"),
    ),
    "debug": (
        ("dfr0", "u64"), ("debug_arch", "u32"), ("brps", "u32"),
        ("wrps", "u32"), ("ctx_cmps", "u32"), ("max_cpus", "u32"),
    ),
    "layout": (
        ("work_struct_size", "u32"), ("work_data", "u32"),
        ("work_entry", "u32"), ("work_func", "u32"),
        ("work_data_init", "u64"), ("pt_regs_size", "u32"),
        ("pt_regs_regs", "u32"), ("pt_regs_sp", "u32"),
        ("pt_regs_pc", "u32"), ("pt_regs_pstate", "u32"),
        ("per_cpu_pointer_stride", "u32"),
    ),
    "maps": (
        ("max_snapshot_bytes", "u32"), ("max_chunk_bytes", "u32"),
        ("seq_file_size", "u32"), ("seq_buf", "u32"),
        ("seq_size", "u32"), ("seq_from", "u32"),
        ("seq_count", "u32"), ("seq_pad_until", "u32"),
        ("seq_private", "u32"), ("proc_maps_private_size", "u32"),
        ("proc_task", "u32"), ("proc_mm", "u32"),
        ("proc_iter", "u32"), ("vma_iterator_size", "u32"),
        ("mas_tree", "u32"), ("mas_index", "u32"),
        ("mas_last", "u32"), ("mas_node", "u32"),
        ("mas_status", "u32"), ("mm_struct_size", "u32"),
        ("mm_mt", "u32"), ("vma_struct_size", "u32"),
        ("vma_start", "u32"), ("vma_end", "u32"),
        ("mas_start_node", "u64"), ("ma_start_status", "u32"),
        ("show_map_vma_args", "u32"),
    ),
    "symbols": (
        ("linux_banner", "string"), ("init_task", "string"),
        ("finish_task_switch", "string"), ("do_exit", "string"),
        ("breakpoint_handler", "string"),
        ("watchpoint_handler", "string"),
        ("single_step_handler", "string"), ("bp_on_reg", "string"),
        ("wp_on_reg", "string"), ("per_cpu_offset", "string"),
        ("nr_cpu_ids", "string"), ("system_unbound_wq", "string"),
        ("queue_work_on", "string"),
        ("flush_work", "string"),
        ("synchronize_rcu_tasks", "string"),
        ("schedule_on_each_cpu", "string"),
        ("ktime_get_mono_fast_ns", "string"),
        ("copy_from_kernel_nofault", "string"),
        ("show_map_vma", "string"), ("find_vma", "string"),
        ("mas_walk", "string"), ("get_task_mm", "string"),
        ("mmput", "string"), ("mmap_read_lock_killable", "string"),
        ("mmap_read_unlock", "string"),
        ("rust_helper_get_task_struct", "string"),
        ("rust_helper_put_task_struct", "string"),
        ("vmalloc_noprof", "string"), ("vfree", "string"),
    ),
    "hooks": (
        ("finish_task_switch_args", "u32"),
        ("do_exit_args", "u32"),
        ("breakpoint_handler_args", "u32"),
        ("watchpoint_handler_args", "u32"),
        ("single_step_handler_args", "u32"),
    ),
    "quirks": (
        ("control_under_rcu", "bool"),
        ("unload_requires_quiescent", "bool"),
        ("cpu_hotplug_while_armed", "bool"),
        ("safe_unload_required", "bool"),
        ("linux_banner_prefix_ok", "bool"),
    ),
}

GO_TYPE_NAMES = {
    "product": "ProductProfile", "kernel": "KernelProfile",
    "kpatch": "KPatchProfile", "task": "TaskOffsets",
    "cred": "CredOffsets", "perf_event": "PerfEventOffsets",
    "debug": "DebugProfile", "symbols": "KernelSymbols",
    "layout": "KernelLayouts", "maps": "MapsProfile", "hooks": "HookABI",
    "quirks": "ProfileQuirks",
}

GO_WORDS = {
    "id": "ID", "pid": "PID", "tgid": "TGID", "uid": "UID",
    "cpu": "CPU", "sdk": "SDK", "va": "VA", "btf": "BTF",
    "sha256": "SHA256", "ack": "ACK", "dfr0": "DFR0",
    "brps": "BRPs", "wrps": "WRPs", "ctx": "Ctx",
    "mm": "MM", "rcu": "RCU", "kpatch": "KPatch", "cpus": "CPUs",
    "bp": "BP", "hw": "HW", "ids": "IDs", "oncpu": "OnCPU",
    "kfunc": "KFunc", "ok": "OK", "abi": "ABI", "vma": "VMA",
}

GO_EXACT_NAMES = {
    "mm_mt": "MMMapleTree",
}


def exact_keys(value: Any, expected: set[str], path: str) -> None:
    if not isinstance(value, dict):
        raise ValueError(f"{path} must be an object")
    actual = set(value)
    unknown = sorted(actual - expected)
    missing = sorted(expected - actual)
    if unknown:
        raise ValueError(f"unknown key at {path}: {unknown[0]}")
    if missing:
        raise ValueError(f"missing key at {path}: {missing[0]}")


def integer(value: Any, path: str, minimum: int = 0, maximum: int = 0xFFFFFFFF) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{path} must be an integer")
    if value < minimum or value > maximum:
        raise ValueError(f"{path} out of range")
    return value


def text(value: Any, path: str, maximum: int = 256) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise ValueError(f"{path} must be a non-empty string")
    try:
        value.encode("ascii")
    except UnicodeEncodeError as error:
        raise ValueError(f"{path} must be ASCII") from error
    return value


def validate_profile(profile: dict[str, Any]) -> None:
    exact_keys(profile, PROFILE_KEYS, "profile")
    profile_id = text(profile["id"], "profile.id", 128)
    if re.fullmatch(r"[a-z0-9][a-z0-9._-]*", profile_id) is None:
        raise ValueError("profile.id has invalid characters")

    for section, fields in SECTION_FIELDS.items():
        exact_keys(profile[section], {name for name, _ in fields}, section)
        for name, kind in fields:
            value = profile[section][name]
            path = f"{section}.{name}"
            if kind == "string":
                text(value, path)
            elif kind == "bool":
                if not isinstance(value, bool):
                    raise ValueError(f"{path} must be boolean")
            elif kind == "u64":
                integer(value, path, 0, 0xFFFFFFFFFFFFFFFF)
            else:
                integer(value, path)

    product = profile["product"]
    integer(product["sdk"], "product.sdk", 1, 1000)
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", product["security_patch"]) is None:
        raise ValueError("product.security_patch has invalid format")

    kernel = profile["kernel"]
    for key, length in (("build_id", 40), ("ack_commit", 40), ("btf_sha256", 64)):
        if re.fullmatch(rf"[0-9a-f]{{{length}}}", kernel[key]) is None:
            raise ValueError(f"kernel.{key} has invalid digest")
    if kernel["page_size"] not in (4096, 16384, 65536):
        raise ValueError("kernel.page_size is unsupported")
    integer(kernel["va_bits"], "kernel.va_bits", 32, 52)
    for key in ("task_struct_size", "cred_size", "perf_event_size"):
        integer(kernel[key], f"kernel.{key}", 1, 65536)
    integer(kernel["task_comm_len"], "kernel.task_comm_len", 16, 16)
    integer(kernel["linux_banner_capacity"], "kernel.linux_banner_capacity", 64, 512)
    banner_need = len("Linux version ") + len(kernel["release"]) + 2
    if kernel["linux_banner_capacity"] < banner_need:
        raise ValueError("kernel.linux_banner_capacity too small for kernel.release")

    kpatch = profile["kpatch"]
    if not kpatch["control_path"].startswith("/"):
        raise ValueError("kpatch.control_path must be an absolute path")
    if re.fullmatch(r"[A-Za-z0-9._-]{1,64}", kpatch["module_name"]) is None:
        raise ValueError("kpatch.module_name has invalid characters")

    task_size = kernel["task_struct_size"]
    for name, _ in SECTION_FIELDS["task"]:
        limit = task_size if name != "signal_thread_head" else 4096
        integer(profile["task"][name], f"task.{name}", 0, limit - 1)
    if profile["task"]["comm"] + kernel["task_comm_len"] > task_size:
        raise ValueError("task.comm out of range")
    integer(profile["cred"]["uid"], "cred.uid", 0, kernel["cred_size"] - 4)
    for name, _ in SECTION_FIELDS["perf_event"]:
        integer(profile["perf_event"][name], f"perf_event.{name}", 0, kernel["perf_event_size"] - 1)

    debug = profile["debug"]
    integer(debug["debug_arch"], "debug.debug_arch", 6, 15)
    integer(debug["brps"], "debug.brps", 1, 16)
    integer(debug["wrps"], "debug.wrps", 1, 16)
    integer(debug["ctx_cmps"], "debug.ctx_cmps", 0, 16)
    integer(debug["max_cpus"], "debug.max_cpus", 1, 64)
    dfr0 = debug["dfr0"]
    if (dfr0 & 0xF) != debug["debug_arch"]:
        raise ValueError("debug.dfr0 debug architecture mismatch")
    if ((dfr0 >> 12) & 0xF) + 1 != debug["brps"]:
        raise ValueError("debug.dfr0 BRP count mismatch")
    if ((dfr0 >> 20) & 0xF) + 1 != debug["wrps"]:
        raise ValueError("debug.dfr0 WRP count mismatch")
    if ((dfr0 >> 28) & 0xF) + 1 != debug["ctx_cmps"]:
        raise ValueError("debug.dfr0 context comparator mismatch")

    layout = profile["layout"]
    integer(layout["work_struct_size"], "layout.work_struct_size", 32, 256)
    for name, width in (("work_data", 8), ("work_entry", 16), ("work_func", 8)):
        offset = integer(layout[name], f"layout.{name}")
        if offset + width > layout["work_struct_size"]:
            raise ValueError(f"layout.{name} out of range")
    integer(layout["pt_regs_size"], "layout.pt_regs_size", 272, 1024)
    for name, width in (("pt_regs_regs", 31 * 8), ("pt_regs_sp", 8),
                        ("pt_regs_pc", 8), ("pt_regs_pstate", 8)):
        offset = integer(layout[name], f"layout.{name}")
        if offset + width > layout["pt_regs_size"]:
            raise ValueError(f"layout.{name} out of range")
    if layout["per_cpu_pointer_stride"] != 8:
        raise ValueError("layout.per_cpu_pointer_stride must be 8 on ARM64")

    maps = profile["maps"]
    integer(maps["max_snapshot_bytes"], "maps.max_snapshot_bytes", 4096, 16 * 1024 * 1024)
    integer(maps["max_chunk_bytes"], "maps.max_chunk_bytes", 1, 1536)
    integer(maps["seq_file_size"], "maps.seq_file_size", 64, 256)
    for name, width in (
        ("seq_buf", 8), ("seq_size", 8), ("seq_from", 8),
        ("seq_count", 8), ("seq_pad_until", 8), ("seq_private", 8),
    ):
        offset = integer(maps[name], f"maps.{name}")
        if offset + width > maps["seq_file_size"]:
            raise ValueError(f"maps.{name} out of range")
    integer(maps["proc_maps_private_size"], "maps.proc_maps_private_size", 32, 512)
    integer(maps["vma_iterator_size"], "maps.vma_iterator_size", 32, 256)
    for name, width in (("proc_task", 8), ("proc_mm", 8)):
        offset = integer(maps[name], f"maps.{name}")
        if offset + width > maps["proc_maps_private_size"]:
            raise ValueError(f"maps.{name} out of range")
    proc_iter = integer(maps["proc_iter"], "maps.proc_iter")
    if proc_iter + maps["vma_iterator_size"] > maps["proc_maps_private_size"]:
        raise ValueError("maps.proc_iter out of range")
    for name, width in (
        ("mas_tree", 8), ("mas_index", 8), ("mas_last", 8),
        ("mas_node", 8), ("mas_status", 4),
    ):
        offset = integer(maps[name], f"maps.{name}")
        if offset + width > maps["vma_iterator_size"]:
            raise ValueError(f"maps.{name} out of range")
    integer(maps["mm_struct_size"], "maps.mm_struct_size", 64, 65536)
    mm_mt = integer(maps["mm_mt"], "maps.mm_mt")
    if mm_mt >= maps["mm_struct_size"]:
        raise ValueError("maps.mm_mt out of range")
    integer(maps["vma_struct_size"], "maps.vma_struct_size", 32, 4096)
    for name in ("vma_start", "vma_end"):
        offset = integer(maps[name], f"maps.{name}")
        if offset + 8 > maps["vma_struct_size"]:
            raise ValueError(f"maps.{name} out of range")
    integer(maps["mas_start_node"], "maps.mas_start_node", 1, 0xFFFFFFFFFFFFFFFF)
    integer(maps["ma_start_status"], "maps.ma_start_status", 0, 7)
    integer(maps["show_map_vma_args"], "maps.show_map_vma_args", 2, 2)

    for name, _ in SECTION_FIELDS["hooks"]:
        value = integer(profile["hooks"][name], f"hooks.{name}", 1, 3)
        if value not in (1, 3):
            raise ValueError(f"hooks.{name} must be 1 or 3")


def load_document(path: pathlib.Path) -> tuple[str, list[dict[str, Any]]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    exact_keys(document, {"schema_version", "default_profile", "profiles"}, "document")
    if document["schema_version"] != 1 or not isinstance(document["profiles"], list) or not document["profiles"]:
        raise ValueError("invalid schema version or profiles")
    default_id = text(document["default_profile"], "default_profile", 128)
    profiles = document["profiles"]
    for profile in profiles:
        validate_profile(profile)
    ids = [profile["id"] for profile in profiles]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate profile id")
    if default_id not in ids:
        raise ValueError("default_profile does not match a profile id")
    return default_id, sorted(profiles, key=lambda profile: profile["id"])


def load_profiles(path: pathlib.Path) -> list[dict[str, Any]]:
    _, profiles = load_document(path)
    return profiles


def quoted(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def c_type(kind: str) -> str:
    return {"string": "const char *", "u32": "uint32_t", "u64": "uint64_t", "bool": "uint8_t"}[kind]


def c_value(value: Any, kind: str) -> str:
    if kind == "string":
        return quoted(value)
    if kind == "bool":
        return "UINT8_C(1)" if value else "UINT8_C(0)"
    if kind == "u64":
        return f"UINT64_C({value})"
    return f"UINT32_C({value})"


def render_c(profiles: list[dict[str, Any]], default_id: str) -> str:
    profiles = sorted(profiles, key=lambda profile: profile["id"])
    default = next(profile for profile in profiles if profile["id"] == default_id)
    lines = [
        "/* Code generated by tools/gen_kpm_profiles.py; DO NOT EDIT. */",
        "#ifndef STACKPLZ_GENERATED_DEVICE_PROFILES_H",
        "#define STACKPLZ_GENERATED_DEVICE_PROFILES_H",
        "", "#include <stdint.h>", "",
    ]
    for section, fields in SECTION_FIELDS.items():
        lines.append(f"struct spz_{section}_profile {{")
        for name, kind in fields:
            lines.append(f"    {c_type(kind)} {name};")
        lines.extend(["};", ""])
    lines.append("struct spz_device_profile {")
    lines.append("    const char *id;")
    for section in SECTION_FIELDS:
        lines.append(f"    struct spz_{section}_profile {section};")
    lines.extend(["};", "", "static const struct spz_device_profile SPZ_DEVICE_PROFILES[] = {"])
    for profile in profiles:
        lines.extend(["    {", f"        .id = {quoted(profile['id'])},"])
        for section, fields in SECTION_FIELDS.items():
            lines.append(f"        .{section} = {{")
            for name, kind in fields:
                lines.append(f"            .{name} = {c_value(profile[section][name], kind)},")
            lines.append("        },")
        lines.append("    },")
    lines.extend([
        "};", "",
        "#define SPZ_DEVICE_PROFILE_COUNT \\",
        "    (sizeof(SPZ_DEVICE_PROFILES) / sizeof(SPZ_DEVICE_PROFILES[0]))",
        "",
        f"#define SPZ_DEFAULT_DEVICE_PROFILE_ID {quoted(default_id)}",
        f"#define SPZ_DEFAULT_KPM_MODULE_NAME {quoted(default['kpatch']['module_name'])}",
        "", "#endif", "",
    ])
    return "\n".join(lines)


def go_name(name: str) -> str:
    if name in GO_EXACT_NAMES:
        return GO_EXACT_NAMES[name]
    return "".join(GO_WORDS.get(word, word[:1].upper() + word[1:]) for word in name.split("_"))


def go_type(kind: str) -> str:
    return {"string": "string", "u32": "uint32", "u64": "uint64", "bool": "bool"}[kind]


def go_value(value: Any, kind: str) -> str:
    if kind == "string":
        return quoted(value)
    if kind == "bool":
        return "true" if value else "false"
    return str(value)


def render_go(profiles: list[dict[str, Any]], default_id: str) -> str:
    profiles = sorted(profiles, key=lambda profile: profile["id"])
    lines = ["// Code generated by tools/gen_kpm_profiles.py; DO NOT EDIT.", "", "package kpm", ""]
    for section, fields in SECTION_FIELDS.items():
        lines.append(f"type {GO_TYPE_NAMES[section]} struct {{")
        width = max(len(go_name(name)) for name, _ in fields)
        for name, kind in fields:
            lines.append(f"\t{go_name(name).ljust(width)} {go_type(kind)}")
        lines.extend(["}", ""])
    device_fields = [("ID", "string")] + [
        (go_name(section), GO_TYPE_NAMES[section]) for section in SECTION_FIELDS
    ]
    device_width = max(len(name) for name, _ in device_fields)
    lines.append("type DeviceProfile struct {")
    for name, type_name in device_fields:
        lines.append(f"\t{name.ljust(device_width)} {type_name}")
    lines.extend(["}", "", "var DeviceProfiles = []DeviceProfile{"])
    for profile in profiles:
        lines.extend(["\t{", f"\t\tID: {quoted(profile['id'])},"])
        for section, fields in SECTION_FIELDS.items():
            lines.append(f"\t\t{go_name(section)}: {GO_TYPE_NAMES[section]}{{")
            width = max(len(go_name(name)) for name, _ in fields)
            for name, kind in fields:
                label = (go_name(name) + ":").ljust(width + 1)
                lines.append(f"\t\t\t{label} {go_value(profile[section][name], kind)},")
            lines.append("\t\t},")
        lines.append("\t},")
    lines.extend([
        "}", "", "func FindDeviceProfile(id string) (DeviceProfile, bool) {",
        "\tfor _, profile := range DeviceProfiles {", "\t\tif profile.ID == id {",
        "\t\t\treturn profile, true", "\t\t}", "\t}",
        "\treturn DeviceProfile{}, false", "}", "",
        f"const DefaultProfileID = {quoted(default_id)}",
        "",
        "func DefaultDeviceProfile() DeviceProfile {",
        "\tprofile, found := FindDeviceProfile(DefaultProfileID)",
        "\tif !found {",
        "\t\tpanic(\"missing default device profile \" + DefaultProfileID)",
        "\t}",
        "\treturn profile",
        "}", "",
    ])
    return "\n".join(lines)


def write_or_check(path: pathlib.Path, content: str, check: bool) -> bool:
    content = content.rstrip("\n") + "\n"
    current = path.read_text(encoding="utf-8") if path.exists() else None
    if check:
        return current == content
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")
    return True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=pathlib.Path, default=DEFAULT_INPUT)
    parser.add_argument("--c-output", type=pathlib.Path, default=DEFAULT_C_OUTPUT)
    parser.add_argument("--go-output", type=pathlib.Path, default=DEFAULT_GO_OUTPUT)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    default_id, profiles = load_document(args.input)
    outputs = (
        (args.c_output, render_c(profiles, default_id)),
        (args.go_output, render_go(profiles, default_id)),
    )
    stale = []
    for path, content in outputs:
        if not write_or_check(path, content, args.check):
            stale.append(str(path))
        elif not args.check:
            print(f"generated {path}")
    if stale:
        print("stale generated files: " + ", ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
