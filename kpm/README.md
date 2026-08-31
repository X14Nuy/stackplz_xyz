# stackplz KPM companion

This directory contains a record-only KPatch-Next companion for stackplz. It
observes scheduler-visible task identities, audits ARM64 hardware debug state,
and can program an owned EL0 breakpoint/watchpoint slot without using the
perf_event breakpoint registration path.

The KPM is intentionally separate from the ordinary stackplz binary:

- stackplz defaults remain `proc` target lookup and the `perf` breakpoint
  backend;
- stackplz never loads, installs, persists, or unloads this KPM;
- the operator must select an exact compiled device profile;
- no control command writes target memory or accepts arbitrary system-register
  values.

See [the operator guide](../docs/KPM_FORENSICS.md) before deployment.

## Pinned build inputs

The current artifact is closed to:

- KPatch-Next SDK commit
  `0fe6d142266b80e5aa445a7ea1534f88a8f33a35`;
- KPatch ABI `kpver=d05`, kernel ABI `kver=60c17`;
- AArch64 ELF64 little-endian relocatable KPM format;
- device profile `oneplus-plk110-a16-b4999618-d05`.

The SDK must contain the repository patch
`patches/kpatch-next-d05-safe-kpm-unload.patch`. It adds the fixed
`kpm_safe_unload_v1` ABI marker, makes control buffers per-call, and lets a
KPM refuse unload until cleanup is provably quiescent.

From the stackplz repository root:

```sh
export KP_DIR=/absolute/path/to/KPatch-Next
test "$(git -C "$KP_DIR" rev-parse HEAD)" = \
  0fe6d142266b80e5aa445a7ea1534f88a8f33a35

# Apply exactly once. If the reverse check succeeds, it is already applied.
git -C "$KP_DIR" apply --check --reverse \
  "$PWD/kpm/patches/kpatch-next-d05-safe-kpm-unload.patch" ||
git -C "$KP_DIR" apply \
  "$PWD/kpm/patches/kpatch-next-d05-safe-kpm-unload.patch"
```

Use one serialized operator for KPM load/control/unload. The pinned d05 SDK
still has module-list writer synchronization limitations outside the scope of
this companion.

## Host tests

```sh
make kpm-generate-check
make kpm-host-test
```

`kpm-host-test` runs the C suites normally and again with ASan/UBSan.
ASan detects memory errors such as out-of-bounds and use-after-free; UBSan
detects undefined C behavior such as invalid shifts and arithmetic overflow.
They are host test instrumentation and are not linked into the Android KPM.

Go tests are run separately from the repository root:

```sh
go test -race ./user/kpm ./user/event ./user/util ./user/module ./cli/cmd
```

## Cross-build and artifact gate

```sh
make -C kpm clean
make -C kpm \
  TARGET_COMPILE=aarch64-linux-gnu- \
  KP_DIR="$KP_DIR" \
  all

make -C kpm/scripts verify \
  ARTIFACT=../stackplz-kpm.kpm \
  KP_DIR="$KP_DIR" \
  TARGET_COMPILE=aarch64-linux-gnu-
```

The artifact gate fails closed unless all of the following hold:

- the generated C and Go profile registries are current;
- the KPatch checkout is at the exact pinned commit and the safe-unload patch
  is applied exactly;
- the artifact is AArch64 ELF64 little-endian `REL` and contains
  `.kpm.info`, `.kpm.init`, `.kpm.ctl0`, and `.kpm.exit`;
- every undefined symbol is on the fixed import allowlist and has an exact
  `KP_EXPORT_SYMBOL` proof in the pinned SDK;
- no perf breakpoint registration import is present;
- disassembly contains DBGBVR, DBGBCR, DBGWVR, DBGWCR, and MDSCR_EL1 access.

`nm -u` showing `U symbol` does not mean the final KPM is broken. A KPM is a
relocatable object, so `U` means “an external import that the KPatch loader
must resolve.” The gate proves both sides: only the fixed KPatch ABI/string/
memory helpers/version variables may remain external, and every such import
must be exported by the pinned SDK. Unknown, libc, allocator, compiler-atomic,
or perf registration imports fail the build.

## Device profile table

Editable source:

- `profiles/profiles.json`: strict, closed profile data;
- `profiles/schema.json`: schema and field bounds;
- `../tools/gen_kpm_profiles.py`: semantic validation and deterministic code
  generation.

Generated outputs, which must not be edited by hand:

- `generated/device_profiles.h`;
- `../user/kpm/generated_profiles.go`.

Generate and verify:

```sh
python3 tools/gen_kpm_profiles.py
python3 tools/gen_kpm_profiles.py --check
python3 -m unittest discover -s tools/tests -v
```

The table deliberately contains all device-specific values requested by the
runtime:

| Section | Required evidence |
| --- | --- |
| `product` | exact manufacturer/model/device/fingerprint, Android release, SDK, security patch |
| `kernel` | exact release, build ID, ACK commit, BTF SHA-256, page/VA size, structure sizes, `TASK_COMM_LEN`, banner buffer |
| `kpatch` | kpver, kver, SDK version/commit, on-device control path, module name, safe-unload symbol, `kf_*` names, whether those exports are function pointers |
| `task` / `cred` | BTF/vmlinux-proven offsets and containing structure bounds |
| `perf_event` | read-only audit offsets and structure bounds |
| `debug` | raw ID_AA64DFR0_EL1 and decoded BRP/WRP/context-comparator counts |
| `layout` | work_struct and pt_regs sizes/offsets plus per-CPU pointer stride |
| `maps` | snapshot/chunk limits plus exact seq_file, proc_maps_private, maple-tree iterator, mm_struct and vm_area_struct ABI |
| `symbols` | exact kernel symbol names for scheduler, exception, workqueue, time, no-fault read, and quiescence helpers |
| `hooks` | argument counts for each hooked kernel function (`hook_wrap1` vs `hook_wrap3`) |
| `quirks` | control/unload/CPU-hotplug policy, whether safe-unload is required, and whether a truncated `linux_banner` prefix is enough |

Document-level `default_profile` selects the current device. CLI defaults (`--kpm-profile`, `--kpm-control`, `--kpm-module`) and the KPM load argument fallback come from that entry. Duplicate the JSON object to adapt another kernel; do not scatter symbol names or offsets in C/Go.

### Adapting another test device

1. Capture an immutable evidence bundle: full build fingerprint, `uname -a`,
   kernel build ID, exact source/ACK commit, KPatch `kpver`/`kver` and SDK
   commit, BTF/vmlinux hashes, page size, CPU count, and raw
   ID_AA64DFR0_EL1.
2. Derive every structure size and offset from the matching BTF/vmlinux.
   Confirm each configured offset plus field width remains within its
   containing structure.
3. Confirm every configured symbol exists in the matching kernel and verify
   its exact prototype from the matching source. In particular, do not infer
   handler argument positions from a nearby kernel tag. If a name is stripped
   or ambiguous, archive a masked instruction signature tied to the exact
   vmlinux/source as review evidence; ambiguity must reject, not select the
   nearest-looking address.
4. Duplicate the JSON entry, assign a new unique profile ID, and replace all
   evidence. Never reuse a profile because only the model name looks similar.
   If that device is the one you are using now, set document-level
   `default_profile` to the new ID so CLI flags and empty KPM load args follow
   it.
5. Regenerate both registries and run all host tests.
6. Cross-build with the exact pinned KPatch SDK, run the artifact gate, then
   perform the complete physical-device acceptance checklist with before/after
   debug-register and owner-bookkeeping snapshots.
7. Add negative device tests: altered fingerprint/release, page size, CPU
   count, DFR0, missing symbols, occupied debug slots, PID reuse, and cleanup
   failure must all reject or remain inert.

Offsets are compiled into the closed registry; the device control interface
does not accept arbitrary offsets. This makes an incorrect adaptation a build
review problem instead of a runtime free-form kernel-memory interface.

## Manual lifecycle

KPatch d05 CLI syntax, executed manually on the authorized device:

```sh
KPATCH=/data/adb/modules/KPatch-Next/bin/kpatch
PROFILE=oneplus-plk110-a16-b4999618-d05

$KPATCH kpm load /data/local/tmp/stackplz-kpm.kpm
$KPATCH kpm load /data/local/tmp/stackplz-kpm.kpm "profile=$PROFILE"
$KPATCH kpm ctl0 stackplz-kpm "status"
$KPATCH kpm ctl0 stackplz-kpm "audit"
$KPATCH kpm ctl0 stackplz-kpm "clear"
```

Never unload while a binding, configured breakpoint, enabled slot, running
handler, maps snapshot, or asynchronous request remains live. Hot unload on
the tested KPatch d05 device previously blocked, so this release does not
recommend `kpm unload stackplz-kpm`. Clear the module, verify an empty status,
then reboot before replacing or removing it. Reboot is the recovery boundary;
it is not evidence that a prior hot unload succeeded.
