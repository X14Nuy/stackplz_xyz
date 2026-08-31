# KPM Hidden-Process Maps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconstruct a coherent hidden target's maps through the bound KPM task and inject it into stackplz's existing maps-dependent workflows.

**Architecture:** An optional profile-driven KPM backend retains the scheduler-observed task, renders all VMAs asynchronously through the kernel's `show_map_vma`, and exposes an immutable snapshot through bounded hex chunks. The Go client validates and caches that snapshot before the preparation binding is cleared; failures remain isolated from tracing features that do not require maps.

**Tech Stack:** freestanding C11 KPM core, KPatch-Next d05, Linux ARM64 6.12 ABI/BTF profiles, Go, Python profile generator, ADB, Android NDK.

**Spec:** `docs/superpowers/specs/2026-08-31-kpm-hidden-process-maps-design.md`

## Global Constraints

- Existing default task source `proc` and breakpoint backend `perf` remain unchanged.
- The current exact profile is `oneplus-plk110-a16-b4999618-d05` with BTF SHA-256 `75b509d7abaed46f57cf26c3780122b5abfae889cde4a22edf1812bba3ce3320`.
- Maps support is optional and cannot reject the core KPM profile.
- Snapshot rendering runs only on the unbound workqueue, never from control, scheduler, or debug-exception context.
- Snapshot size is at most 2 MiB; decoded chunks are at most 1536 bytes.
- Symbol names and layout offsets come only from the generated device profile; the control protocol accepts neither kernel addresses nor offsets.
- Preserve all existing uncommitted workspace changes. Per the existing execution override, do not create Git commits.
- Do not hot-unload the currently loaded KPM; reboot before replacing it.

---

### Task 1: Profile the maps ABI and generate both registries

**Files:**
- Modify: `kpm/profiles/schema.json`
- Modify: `kpm/profiles/profiles.json`
- Modify: `tools/gen_kpm_profiles.py`
- Modify: `tools/tests/test_gen_kpm_profiles.py`
- Regenerate: `kpm/generated/device_profiles.h`
- Regenerate: `user/kpm/generated_profiles.go`
- Modify: `user/kpm/generated_profiles_test.go`

**Interfaces:**
- Produces C `struct spz_maps_profile` and Go `MapsProfile`.
- Adds the maps symbol names listed in the design specification.
- Keeps maps symbol resolution outside the mandatory `SPZ_SYMBOL_COUNT` profile-ready path.

- [ ] **Step 1: Write failing generator tests**

Add tests that remove `show_map_vma`, place `seq_private` outside
`seq_file_size`, set `show_map_vma_args` to 3, and set chunk size above the
control-safe limit. Each fixture must be rejected. Extend the generated-profile
behavior test to assert literal current-device offsets and limits.

- [ ] **Step 2: Run the focused tests and verify RED**

Run: `python -m unittest tools.tests.test_gen_kpm_profiles -v`

Expected: FAIL because the schema/generator and generated types do not yet
contain the maps capability.

- [ ] **Step 3: Implement schema, semantic validation, and generation**

Add exact maps fields from the specification. Generator validation must use
width-aware bounds such as `seq_private + 8 <= seq_file_size` and
`mas_status + 4 <= vma_iterator_size` and emit deterministic C/Go fields.

- [ ] **Step 4: Regenerate and verify GREEN**

Run:

```sh
python tools/gen_kpm_profiles.py
python -m unittest tools.tests.test_gen_kpm_profiles -v
python tools/gen_kpm_profiles.py --check
go test ./user/kpm -run 'TestGenerated|TestDefault' -count=1
```

Expected: all PASS and regeneration is a no-op.

### Task 2: Add tested maps commands and immutable snapshot state

**Files:**
- Modify: `kpm/include/stackplz/core.h`
- Create: `kpm/include/stackplz/maps.h`
- Modify: `kpm/core/command.c`
- Create: `kpm/core/maps.c`
- Modify: `kpm/Makefile`
- Modify: `kpm/tests/Makefile`
- Modify: `kpm/tests/test_command.c`
- Create: `kpm/tests/test_maps.c`

**Interfaces:**
- Produces `SPZ_COMMAND_MAPS` and `SPZ_COMMAND_MAPS_READ`.
- Produces `spz_maps_capture_task`, `spz_maps_snapshot`, `spz_maps_read`, and `spz_maps_clear` over a backend that retains/releases tasks and renders/frees buffers.
- A maps-read result contains snapshot ID, offset, total, CRC32, EOF, pointer, and length; callers copy it before ending the read guard.

- [ ] **Step 1: Write parser RED tests**

Assert `maps` parses with no arguments and
`maps-read snapshot=7 offset=1536` parses exact decimal values. Assert missing,
duplicate, unknown, hexadecimal, overflowing, and extra arguments fail.

- [ ] **Step 2: Write state RED tests**

Use a real fake backend that counts task retains/releases and allocates a
literal two-line maps buffer. Assert one retain per generation, refresh
publication only after successful rendering, stale snapshot rejection, exact
chunk boundaries, EOF, failed-refresh preservation, and complete clear.

- [ ] **Step 3: Run and verify RED**

Run: `make -C kpm/tests test TEST='test_command test_maps'`

Expected: compile/test failure because the new commands and maps state do not
exist.

- [ ] **Step 4: Implement the minimal parser and maps state**

Keep command token counts bounded. Use atomic publication/read gating so a
buffer cannot be freed while `maps-read` copies from it. CRC32 must reuse
`spz_crc32_ieee`.

- [ ] **Step 5: Run and verify GREEN under sanitizers**

Run:

```sh
make -C kpm/tests clean test TEST='test_command test_maps'
make -C kpm/tests sanitize TEST='test_command test_maps'
```

Expected: all PASS with no ASan/UBSan diagnostics.

### Task 3: Integrate the optional asynchronous KPatch renderer

**Files:**
- Modify: `kpm/include/stackplz/profile.h`
- Modify: `kpm/platform/kpatch/compat.h`
- Modify: `kpm/platform/kpatch/runtime.c`
- Modify: `kpm/platform/kpatch/control.c`
- Modify: `kpm/platform/kpatch/main.c`
- Create: `kpm/platform/kpatch/maps.c`
- Modify: `kpm/Makefile`
- Modify: `kpm/tests/test_async.c`
- Modify: `kpm/tests/test_control.c`
- Create: `kpm/tests/test_maps_renderer.c`

**Interfaces:**
- Resolves the maps symbol set separately and publishes `maps_supported`.
- Scheduler binding capture retains the live task; async clear releases it.
- `SPZ_ASYNC_MAPS` calls the renderer; `maps-read` returns one bounded hex chunk.

- [ ] **Step 1: Write lifecycle and control RED tests**

Assert a newly bound task is retained once, repeated schedules do not add
references, rebind-before-clear returns `-EBUSY`, `maps` returns an async
request, status reports maps metadata, maps-read serializes exact bytes, and
clear frees the snapshot and releases the reference. Assert an unsupported
maps backend leaves status `state=ready` and syscall/debug lifecycle usable.

- [ ] **Step 2: Write renderer RED tests**

With profiled byte-array fixtures and fake `find_vma`, `mas_walk`, and
`show_map_vma` functions, assert iterator/tree initialization, strict vm_end
progress, coherent two-VMA output, overflow rejection, iterator mismatch, and
lock/MM cleanup on every error.

- [ ] **Step 3: Run and verify RED**

Run: `make -C kpm/tests test TEST='test_async test_control test_maps_renderer'`

Expected: FAIL because the maps async operation, optional resolver, and
renderer are absent.

- [ ] **Step 4: Implement optional symbol resolution and the renderer**

Convert symbol addresses to typed function pointers through unions. Build
profiled `seq_file`, private, and iterator storage with checked offset writes.
Call `vmalloc_noprof`, `get_task_mm`, mmap lock/unlock, `find_vma`, `mas_walk`,
`show_map_vma`, `mmput`, and `vfree` only from the async worker.

- [ ] **Step 5: Implement control/status integration and clear ordering**

Hex-encode at most 1536 decoded bytes. During clear: disable/restore debug
state, drain handlers, block maps readers, free maps, release task, clear
binding/breakpoint, then reset the event ring.

- [ ] **Step 6: Run and verify GREEN**

Run:

```sh
make -C kpm/tests clean test
make -C kpm/tests sanitize
```

Expected: every host C test and sanitizer run PASS.

### Task 4: Assemble and validate maps in the Go KPM client

**Files:**
- Modify: `user/kpm/types.go`
- Modify: `user/kpm/command.go`
- Modify: `user/kpm/client.go`
- Modify: `user/kpm/command_test.go`
- Modify: `user/kpm/client_test.go`

**Interfaces:**
- Produces `func (client *Client) SnapshotMaps(ctx context.Context) ([]byte, error)`.
- Produces `ErrMapsUnsupported`, strict maps-read metadata parsing, and IEEE CRC32 verification.

- [ ] **Step 1: Write client RED tests**

Script runner responses for a two-chunk snapshot and assert literal bytes are
returned. Add separate cases for unsupported capability, stale snapshot,
wrong offset/total/CRC, odd hex, oversized data, early EOF, and context cancel.
Each case asserts the real command sequence.

- [ ] **Step 2: Run and verify RED**

Run: `go test ./user/kpm -run 'Maps|Snapshot' -count=1`

Expected: FAIL because the maps API is absent.

- [ ] **Step 3: Implement command builders and client assembly**

Refactor async submission to return its request ID without changing existing
Enable/Disable/Clear behavior. Read chunks from offset zero until EOF, enforce
stable metadata and the 2 MiB bound, then validate `crc32.ChecksumIEEE`.

- [ ] **Step 4: Run and verify GREEN with race detection**

Run: `go test -race ./user/kpm -count=1`

Expected: PASS with no races.

### Task 5: Inject KPM maps into existing stackplz workflows

**Files:**
- Modify: `user/event/event_mmap2.go`
- Modify: `user/event/event_mmap2_test.go`
- Modify: `cli/cmd/kpm_integration.go`
- Modify: `cli/cmd/kpm_integration_test.go`
- Modify: `cli/cmd/breakpoint_prepare.go`
- Modify: `cli/cmd/breakpoint_prepare_test.go`
- Modify: `cli/cmd/root.go`

**Interfaces:**
- Produces `event.LoadMapsContent(pid uint32, content []byte) error`.
- KPM target preparation attempts a snapshot before `clearKPMPreparationBinding` and populates library directories from the cached result.
- Cached KPM maps allow `resolveBreakpointBaseForSource` to resolve `--brk-lib` without a file/base override.

- [ ] **Step 1: Write maps-cache RED tests**

Assert valid content atomically replaces an older cache and an empty/malformed
replacement returns an error without leaving an empty cache.

- [ ] **Step 2: Write CLI RED tests**

Assert KPM `--brk-lib` no longer fails validation solely because file/base is
absent, a successful snapshot is loaded before clear, optional unsupported maps
does not block syscall setup, and a maps-dependent breakpoint reports the
snapshot/lookup failure.

- [ ] **Step 3: Run and verify RED**

Run:

```sh
go test ./user/event -run Maps -count=1
go test ./cli/cmd -run 'KPM|Breakpoint|Maps' -count=1
```

Expected: FAIL for the missing content API and old KPM validation branch.

- [ ] **Step 4: Implement cache injection and preparation ordering**

Parse into a temporary `ProcMaps`, require at least one mapping, then publish
under `maps_lock`. Attempt KPM snapshot only when no maps file was supplied;
log optional failures and preserve them for maps-dependent error reporting.

- [ ] **Step 5: Run and verify GREEN**

Run: `go test -race ./user/event ./cli/cmd ./user/module ./user/util -count=1`

Expected: PASS with no races.

### Task 6: Cross-build, deploy, and close the physical-device loop

**Files:**
- Modify: `docs/KPM_FORENSICS.md`
- Modify: `kpm/README.md`
- Modify: `tests/kpm/device_acceptance.md`
- Modify: `artifacts/kpm/device-test-report-20260831.md`
- Regenerate: `artifacts/kpm/stackplz-kpm.kpm`
- Regenerate: `artifacts/kpm/SHA256SUMS`

**Interfaces:**
- Produces verified AArch64 KPM and stackplz artifacts for the authorized PLK110 device.
- Produces captured visible/hidden maps and non-zero feature evidence.

- [ ] **Step 1: Run the complete host regression gate**

Run:

```sh
python -m unittest discover -s tools/tests -v
python tools/gen_kpm_profiles.py --check
go test -race ./user/kpm ./user/event ./user/util ./user/module ./cli/cmd -count=1
make -C kpm/tests clean test sanitize
git diff --check
```

Expected: all PASS; only pre-existing line-ending notices may remain.

- [ ] **Step 2: Cross-build on `192.168.229.128`**

Use the existing pinned KPatch checkout and Android NDK. Build the KPM, run
`kpm/scripts/verify_artifact.sh`, build ARM64 stackplz, and record SHA-256,
`readelf`, undefined-symbol, and disassembly results.

- [ ] **Step 3: Reboot and load the replacement KPM**

Do not hot-unload. Reboot the phone, confirm ADB/root readiness, push the exact
artifacts, load `stackplz-kpm`, and require status `state=ready
maps_supported=1` for the exact profile.

- [ ] **Step 4: Compare visible maps**

Run the write-loop fixture visibly, bind it, fetch a KPM snapshot, capture
`/proc/<pid>/maps`, and compare sorted stable mappings plus required file paths.
No truncated line, backward/duplicate VMA, or CRC failure is accepted.

- [ ] **Step 5: Exercise the hidden target without manual maps/base input**

Hide the fixture through the existing test mount, prove `ps` and proc maps do
not expose it, then run KPM target-source tests for syscall eBPF, uprobe plus
user-stack symbolization, perf `--brk-lib`, and KPM-direct `--brk-lib`. Require
non-zero events and correct PID/TID/library names for each applicable path.

- [ ] **Step 6: Verify consecutive sessions and final state**

Run at least two maps/tracing/clear cycles. Confirm `binding=none`,
`configured=0`, `enabled=0`, no published maps snapshot, no fixture/mount
residue, and no new kernel errors. Leave the verified KPM loaded; do not hot
unload it.

- [ ] **Step 7: Update evidence and documentation**

Record exact commands, counts, hashes, limitations, and final device state in
the report and acceptance matrix. Do not claim a capability unless its captured
device run passed.

## Execution choice

The user approved the design and requested completion in the current session.
Execute inline with `superpowers:executing-plans`; no subagents and no Git
commits are authorized.
