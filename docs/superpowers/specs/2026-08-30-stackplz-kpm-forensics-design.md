# stackplz KPM forensic observer design

Date: 2026-08-30

Status: implementation specification

## Purpose and authorization boundary

This change extends stackplz for defensive analysis on an explicitly authorized
Android device. It addresses two failure modes:

1. A live task can be absent from the ordinary `/proc` and task-list views, so
   stackplz cannot resolve its UID, mappings, or `task_struct` through those
   views even when the analyst already knows its numeric PID.
2. The existing hardware-breakpoint path creates `PERF_TYPE_BREAKPOINT` events
   with `perf_event_open`. Kernel tampering with the perf-event path can prevent
   those breakpoints from being installed or delivered.

The new code is record-only. It may observe scheduler-visible tasks, snapshot
hardware-debug state, install an explicitly requested EL0 breakpoint in an
otherwise unused comparator, and emit bounded event records. It must not hide
itself, persist automatically, patch target text, alter target data, suppress
security checks, or provide arbitrary kernel-memory/register writes.

The companion KPM is never loaded automatically. Existing stackplz behavior is
the default unless the analyst explicitly selects the KPM task source or direct
hardware-debug backend.

## Current implementation findings

The current hardware-breakpoint path is not a direct-register implementation:

- `user/module/brk.go` loads the perf ring-buffer object.
- the custom `ebpf/perf` dependency creates `PERF_TYPE_BREAKPOINT` events with
  `unix.PerfEventOpen`;
- `src/perf_mmap.c` supplies maps/event transport but does not program ARM64
  debug registers.

The current explicit-PID path also has procfs dependencies before tracing:

- `FindUidByPid` runs `ps -o uid= -p <pid>`;
- module and breakpoint-library resolution reads `/proc/<pid>/maps`.

Consequently, knowing a hidden PID is not currently enough to start stackplz,
and the hardware-breakpoint feature remains dependent on the perf subsystem.

## Selected architecture

The implementation keeps the existing eBPF/perf path and adds a companion KPM
with two narrow services:

```text
stackplz CLI
  |-- task-source=proc --------> existing ps/proc/maps behavior
  |-- task-source=kpm/auto ----> KPM scheduler observer
  |
  |-- brk-backend=perf --------> existing perf_event_open backend
  `-- brk-backend=kpm-direct --> KPM ARM64 debug-register backend

KPM companion
  |-- validated device profile
  |-- scheduler-visible task registry
  |-- per-CPU direct BRP/WRP ownership
  |-- exception/event capture
  `-- fixed-size record ring + bounded control protocol
```

This split avoids changing the behavior of existing users, makes fallback
visible, and keeps device-specific kernel logic out of the Go process.

`auto` is conservative:

- `--task-source=auto` tries procfs, then KPM if the procfs lookup fails and the
  KPM profile is ready.
- `--brk-backend=auto` may select KPM only after an explicit KPM status check.
  It must print the chosen backend. It must not silently fall back from an
  explicitly requested `kpm-direct` backend to perf.

## Trust and threat model

The observer is designed to remain useful when:

- `/proc`, `ps`, or VFS enumeration is filtered;
- the ordinary global task list is unlinked or filtered while a task still runs;
- `perf_event_open`, perf registration, or perf callback delivery is tampered
  with;
- Linux-managed debug-register slots are used by other debuggers.

It does not promise correctness against arbitrary kernel compromise. In
particular, an attacker able to rewrite exception vectors, scheduler call sites,
KPatch hook dispatch, or the KPM itself can defeat any in-kernel observer. The
implementation detects profile mismatches and inconsistent comparator state and
fails closed instead of claiming an unqualified bypass.

## Device profile as the single source of truth

All device-dependent values live in a checked-in JSON table. A deterministic
generator produces a C header for the KPM and a Go registry for user-space
validation. Generated files are checked in; CI verifies that regeneration is a
no-op.

Proposed layout:

```text
kpm/profiles/profiles.json
kpm/profiles/schema.json
kpm/generated/device_profiles.h
user/kpm/generated_profiles.go
tools/gen_kpm_profiles.py
```

Each profile contains:

- stable identity: profile ID, product, build fingerprint, kernel release,
  kernel build ID, ACK commit, KPatch ABI/version, page size, VA bits;
- task layout: `task_struct` size and the offsets needed for PID/TGID, state,
  parent, credentials, command, MM, signal, start times, thread linkage, and
  perf-event bookkeeping;
- perf-event layout used only for integrity observations;
- ARM64 debug capability limits and required debug-register feature level;
- required kernel symbols and, where unavoidable, a kernel-text-relative
  address plus a masked instruction signature;
- compatibility quirks, including the KPatch d05 unload/RCU behavior.

The first profile is `oneplus-plk110-a16-b4999618-d05`, based on the authorized
device archive and exact ACK commit `b2a876903b495c444a94b16f50d1463ffe953957`.
It records a 4 KiB page size, 39-bit VA, eight CPUs, six BRPs, four WRPs, two
context comparators, `task_struct` size 5184, and these task offsets:

```text
usage=64 cpu=40 tasks=1592 mm=1672 active_mm=1680
exit_state=1696 pid=1800 tgid=1804 real_parent=1816 parent=1824
thread_pid=1904 pid_links=1912 thread_node=1976
start_time=2104 start_boottime=2112 real_cred=2296 cred=2304
comm=2320 signal=2392 perf_event_ctxp=2992 perf_event_list=3048
thread=3488 signal.thread_head=16
cred.size=184 cred.uid=8
```

The matching perf-event observation offsets are:

```text
event.size=1120 state=168 attr=216 attr.bp_type=268
attr.bp_addr=272 attr.bp_len=280 hw=360 arch.address=360
arch.trigger=368 arch.ctrl=376 hw.target=456 ctx=552
oncpu=664 cpu=668 owner=688 context.task=152
```

No profile may be selected by a weak product-name match alone. At KPM load, the
implementation validates all available identity fields, `init_task` structural
invariants, PID/command plausibility, per-CPU debug counts, and every required
symbol or masked text signature. A mismatch leaves the module in `PROFILE_REJECTED`
state: status and diagnostics remain available, but hooks and debug programming
are disabled.

Candidate kernel-memory reads during this validation use the pre-resolved
`copy_from_kernel_nofault` helper. A bad offset or pointer therefore rejects the
profile without a raw `memcpy` dereference in the KPatch adapter.

## Scheduler-visible task observation

### Why scheduler observation

A runnable task must pass through the scheduler even when procfs or the global
`task_struct.tasks` chain is deceptive. The observer therefore learns task
identity from the actual switch path and never treats task-list traversal as the
source of truth.

The exact target kernel calls `finish_task_switch(prev)` in the incoming task's
context. The KPM wraps this function with pre/post callbacks:

1. The pre callback restores any observer-owned debug comparator on that CPU.
   This runs before the kernel's incoming perf-event restore and prevents an old
   EL0 comparator from leaking to the next user task.
2. The original function runs, including Linux perf scheduling.
3. The post callback observes `current` and, when it matches an active binding,
   installs the requested direct comparator after Linux has restored its own
   slots.

This does not depend on `perf_event_task_sched_in/out` actually doing useful
work. An EL0-only comparator may remain enabled during the kernel portion of a
context switch, but is removed before another task returns to EL0.

### Selector and identity

A bind request supplies:

- numeric PID or TID;
- whether it matches `pid`, `tgid`, or either;
- optional UID and `comm` constraints;
- an optional known `start_boottime` for strong reuse protection.

The scheduler hot path compares only bounded scalar fields. On a match it copies
and publishes this immutable identity without retaining a `task_struct`
reference:

```text
task pointer cookie, pid, tgid, start_time, start_boottime, uid, comm, generation
```

The saved pointer value is diagnostic data, never a later dereference target.
While the candidate is the live `current` task, every scheduler/debug callback
re-reads and checks its PID/TGID, start time, and exit state. The generation
prevents stale commands from applying after rebind. A `do_exit` observer marks a
matching binding dead. PID reuse never silently inherits a breakpoint.

If a hidden task never becomes scheduled after a bind request, discovery remains
pending. Status reports this honestly; no unsafe global memory scan is attempted.

### Hot-path rules

Scheduler and debug-exception callbacks use only fixed per-CPU/per-binding
storage. They do not sleep, allocate, log, symbolize, access user memory, walk
unbounded lists, or take blocking locks. Event publication is a bounded copy
followed by an atomic commit.

## Direct ARM64 hardware-debug backend

### Scope

The initial backend supports EL0 execute breakpoints and EL0 read, write, or
read/write watchpoints at an explicit virtual address. Address canonicality,
alignment, length, BAS mask, and privilege fields are validated before a request
is armed. Kernel-address breakpoints and arbitrary control-register values are
not accepted through the control protocol.

### Comparator selection and coexistence

The KPM reads `ID_AA64DFR0_EL1` and caps the result by the selected profile. It
uses compile-time indexed helpers for `DBGBVR<n>_EL1`/`DBGBCR<n>_EL1` and
`DBGWVR<n>_EL1`/`DBGWCR<n>_EL1`; no perf registration API is used.

On the target CPU, after Linux's own restore, a slot is safe only when:

- the corresponding Linux `bp_on_reg[]` or `wp_on_reg[]` entry is empty; and
- the live hardware control register has its enable bit clear.

The chosen slot's value/control and relevant `MDSCR_EL1` bits are snapshotted in
the same CPU callback before programming. If software bookkeeping and hardware
state disagree, the operation returns `-EBUSY` with an integrity record. The KPM
never steals, disables, or reuses a Linux/ptrace/perf-owned slot.

Programming uses the architecture-required synchronization barriers. Only the
module-owned slot and the minimum required `MDSCR_EL1.MDE` state are changed.
Restoration is conditional: if the live slot no longer contains the exact value
and control written by the KPM, the module reports interference and does not
overwrite the new owner.

### Hit handling and re-arm

The exact ACK debug handlers iterate Linux's software slot arrays, so an
independently programmed slot is otherwise ignored. The KPM wraps
`breakpoint_handler` and `watchpoint_handler` before the original handler.

For an exception owned by the active binding, the callback:

1. validates CPU, slot, generation, current task identity, exception address,
   and live comparator contents;
2. copies a bounded register/event record;
3. disables only its own comparator;
4. either completes a one-shot request or arms a one-instruction re-arm state;
5. consumes only that owned exception.

Repeat mode uses the kernel's single-step exception path. The KPM records whether
the task already had user single-step enabled, enables the minimum step state,
and wraps `single_step_handler`. On the matching next step it restores the owned
comparator and only clears step state that the KPM added. If single-step was
already active, the original handler still receives the event. Unexpected CPU,
task, generation, or register state cancels re-arm and emits an integrity record.

One-shot mode is the safe default. Repeat mode must pass targeted unit/model
tests before it is exposed by the CLI.

### Per-CPU state machine

Each CPU has one fixed state object:

```text
EMPTY -> SNAPSHOTTED -> ARMED -> HIT_DISABLED -> STEP_PENDING -> ARMED
                     `---------------------------> ONE_SHOT_DONE
any state -> RESTORING -> EMPTY
any mismatch -> QUARANTINED (report only; no blind overwrite)
```

The state includes slot type/index, saved and programmed registers, saved
`MDSCR_EL1`, target generation, task identity, and a monotonically increasing
transition counter. Illegal transitions fail closed.

## Events and control protocol

KPatch d05 `control0` responses are bounded, so the KPM maintains a fixed binary
ring and returns at most one encoded record per poll. A record contains:

- ABI version, type, size, sequence, timestamp, CPU, flags;
- binding/breakpoint IDs and generation;
- task pointer cookie, PID, TGID, UID, `comm`, start times;
- exception class, comparator kind/index, requested and observed addresses;
- programmed and observed value/control/MDSCR state;
- `x0..x30`, SP, PC, and PSTATE for hit records.

No kernel pointers are presented as reusable control inputs. The task address is
an opaque diagnostic cookie only. Records use a little-endian versioned binary
layout encoded as hexadecimal for `control0`; the Go client validates length,
version, and CRC before decoding.

The text command grammar is deliberately narrow and bounded:

```text
status
profile
bind pid=<n> mode=pid|tgid|either [uid=<n>] [comm=<s>] [start=<n>]
break id=<n> kind=x|r|w|rw addr=<hex> len=<n> mode=once|repeat
enable id=<n>
disable id=<n>
clear
poll [after=<seq>]
audit
```

For a bound task, the raw `status` response transports the complete fixed
16-byte command field as `comm_hex=<32 hex digits>`. This keeps whitespace in a
valid Linux task name from becoming a second protocol token. The Go client
decodes it to text and accepts the legacy `comm=` form only when it is a safe
single atom.

Unknown keys, duplicate keys, overlong input, numeric overflow, non-ASCII input,
and unsupported operations are rejected. `audit` reports debug capabilities,
Linux bookkeeping occupancy, and live register occupancy without changing them.

## Go/CLI integration

New explicit options:

```text
--task-source proc|kpm|auto       (default proc)
--brk-backend perf|kpm-direct|auto (default perf)
--kpm-profile <id>
--kpm-control <path>
--uid <numeric uid>
--maps-file <captured maps file>
--brk-base <absolute mapping base>
--brk-mode once|repeat
```

For `task-source=kpm`, a numeric PID does not call `ps` or open `/proc/<pid>`.
The UID may come from the KPM binding result or an explicit `--uid` override.
Absolute `--brk` addresses require no maps view. `--brk-lib` in KPM mode
requires either `--maps-file` or `--brk-base`; otherwise startup returns an
actionable error rather than silently touching procfs.

The Go KPM package consists of:

- a small command runner interface, with a production implementation for the
  configured `kpmctl`/`kpatch kpm ctl0` path and a fake for tests;
- strict command builders and response decoders;
- generated profile metadata;
- lifecycle coordination that always attempts `disable`/`clear` on normal exit
  and signal cancellation.

Loading and unloading the KPM remain separate administrator actions. stackplz
checks readiness but never installs or persists the module itself.

## Lifecycle and unload safety

Initialization proceeds as a transaction:

1. resolve and validate the exact profile;
2. resolve/verify symbols and text signatures;
3. initialize fixed rings and state;
4. install hooks one at a time;
5. publish `READY` only after all hooks succeed.

Failure rolls back in reverse order. Clear/unload follows this order:

1. reject new commands and disarm all bindings;
2. run a synchronous callback on every online CPU to conditionally restore owned
   comparators and MDSCR state;
3. wait for in-flight handlers/readers;
4. remove hooks in reverse order;
5. clear copied task identities and fixed resources.

KPatch-Next d05 invokes module control/unload under an RCU read-side critical
section. The profile marks this quirk, and the module uses the validated d05
compatibility teardown pattern instead of calling a blocking grace-period wait
from that context. Later ABIs must have separate profile validation; the shim is
not assumed portable.

CPU-online changes are rejected while a direct breakpoint is armed in the first
version. The user must clear the breakpoint, change CPU topology, then re-arm.

## Failure behavior

The implementation fails closed and reports a structured reason for:

- no exact profile or profile invariant mismatch;
- missing/ambiguous symbol or text-signature mismatch;
- no genuinely free comparator;
- comparator ownership changing underneath the KPM;
- stale task identity or PID reuse;
- ring overflow (with a counted loss record);
- unexpected single-step/debug exception state;
- unsupported CPU hotplug or KPatch ABI.

It never claims that a task is absent merely because it has not yet appeared in
a scheduler callback.

## Verification strategy

### Host and deterministic tests

- profile-schema validation and deterministic generation;
- Go command/response parsing, PID-without-proc behavior, maps/base resolution,
  backend selection, and cleanup-on-cancel tests;
- x86-host C tests with mocked registers, per-CPU state, task identities, hook
  callbacks, ring wrap/loss, parser fuzz corpus, and every rollback point;
- lifecycle model tests for migration, PID reuse, concurrent clear/hit,
  interference quarantine, pre-existing single-step, and slot exhaustion;
- sanitizers and strict warnings for host-test code.

### Cross-build and static evidence

- build the KPM against pinned KPatch-Next d05 SDK `0.13.5-2` at commit
  `0fe6d142266b80e5aa445a7ea1534f88a8f33a35`;
- cross-build stackplz for Android ARM64;
- disassemble the KPM and verify direct `DBGB*`/`DBGW*`/`MDSCR_EL1` accesses are
  present while perf registration APIs are absent from the direct backend;
- audit undefined symbols and section sizes;
- verify generated profile files are current.

### Authorized-device acceptance tests

When the device is available, the evidence bundle must cover:

- exact profile acceptance and a deliberately mismatched profile rejection;
- discovery of a test task hidden from procfs/task-list enumeration but still
  scheduled, without using `/proc/<pid>`;
- execute and watchpoint hits with known test addresses and captured registers;
- task migration across CPUs, fork/exit, PID reuse, and one-shot/repeat behavior;
- coexistence with an ordinary ptrace/perf breakpoint and clean `-EBUSY` on
  exhaustion;
- detection of perf-event failure while the direct backend still records its
  own test hit;
- repeated enable/disable and at least 100 load/control/unload cycles;
- before/after register snapshots proving exact restoration and no residual
  hooks or live task-pointer dereferences.

Until those device tests run, the handoff must label them `NOT TESTED` and must
not state that runtime behavior on the physical device is guaranteed.

## Compatibility and non-goals

- Existing CLI defaults and the perf backend remain compatible.
- The initial KPM profile supports only the documented PLK110 build/KPatch pair.
- Adding a device means adding evidence and a profile, not guessing offsets.
- The KPM does not reconstruct hidden procfs mappings. Analysts must use an
  absolute address, a trusted captured maps file, or an explicit module base.
- The KPM does not provide kernel breakpoints, arbitrary register programming,
  memory modification, process concealment, persistence, or automatic loading.
- Raw register captures are authoritative output. Symbolization and higher-level
  stack reconstruction remain user-space/offline operations.
