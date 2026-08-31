# stackplz KPM Forensics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a device-profiled, record-only KPatch-Next companion that can identify scheduler-visible tasks without procfs and implement explicit EL0 ARM64 hardware break/watchpoints without `perf_event_open`, while preserving stackplz's current defaults.

**Architecture:** A generated JSON profile table is shared by Go and the KPM. The KPM wraps the exact scheduler/debug handlers, copies identity only while a task is `current`, owns only a demonstrably free per-CPU comparator, and exports bounded records through `ctl0`; Go adds strict client/session code plus explicit CLI backend selection and offline mapping inputs.

**Tech Stack:** Go 1.20+, Cobra, Python 3 standard library, freestanding GNU C11, ARM64 system registers, KPatch-Next SDK 0.13.5-2/d05, `aarch64-linux-gnu-gcc`, host C tests with GCC/Clang.

**Spec:** `docs/superpowers/specs/2026-08-30-stackplz-kpm-forensics-design.md`

## Global Constraints

- Authorization is limited to the user's own Android test device; code is record-only and explicitly activated.
- Existing `proc` task discovery and `perf` breakpoint behavior remain the defaults.
- The KPM must never auto-load, persist, hide, patch target text/data, or accept arbitrary kernel/register writes.
- Initial device support is only `oneplus-plk110-a16-b4999618-d05`, ACK commit `b2a876903b495c444a94b16f50d1463ffe953957`, KPatch-Next commit `0fe6d142266b80e5aa445a7ea1534f88a8f33a35`.
- Hot paths must not sleep, allocate, print, symbolize, read user memory, take blocking locks, or run unbounded loops.
- A saved task pointer is an opaque cookie and is never dereferenced outside a callback where that same task is live `current`.
- Direct debug programming is EL0-only and uses only a slot whose Linux per-CPU owner pointer is null and live enable bit is clear.
- Profile, ownership, lifecycle, or register mismatches fail closed and emit diagnostics; there is no silent fallback from an explicit `kpm-direct` request.
- Physical-device acceptance remains `NOT TESTED` until the authorized device is available.

## File map

`kpm/profiles/` owns human-reviewable device facts. `tools/gen_kpm_profiles.py` validates and generates `kpm/generated/device_profiles.h` plus `user/kpm/generated_profiles.go`. `user/kpm/` owns the shell-free control protocol and decoding. `kpm/core/` is platform-neutral fixed-state logic; `kpm/platform/arm64/` is the only raw-system-register layer; `kpm/platform/kpatch/` owns symbol resolution, hooks, async CPU work, and KPM entry points. `user/module/kpm_brk.go` adapts the Go KPM session to stackplz's module lifecycle, while focused helpers in `cli/cmd/` keep procfs-free decisions testable.

---

### Task 1: Strict Go KPM protocol, runner, and event decoder

**Files:**
- Create: `user/kpm/types.go`
- Create: `user/kpm/command.go`
- Create: `user/kpm/client.go`
- Create: `user/kpm/event.go`
- Create: `user/kpm/command_test.go`
- Create: `user/kpm/client_test.go`
- Create: `user/kpm/event_test.go`

**Interfaces:**
- Produces: `type Runner interface { Control(context.Context, string) (string, error) }`.
- Produces: `NewExecRunner(binary, module string) (Runner, error)`, invoking `binary kpm ctl0 module command` with `exec.CommandContext` and no shell.
- Produces: `type Client`, `NewClient(Runner, profileID string)`, `Status`, `Bind`, `ConfigureBreakpoint`, `Enable`, `Disable`, `Clear`, `Poll`, and `WaitBound`.
- Produces: `DecodeEventHex(string) (Event, error)` and versioned `Event`/`Registers` types.

- [x] **Step 1: Write failing command-builder and runner tests**

Cover enum validation, ASCII/length limits, comm character restrictions, canonical user address, execute alignment, watchpoint length/alignment, duplicate-free key generation, and exact argv capture:

```go
func TestExecRunnerDoesNotUseShell(t *testing.T) {
    fake := helperProcess(t)
    runner, err := NewExecRunner(fake, "stackplz-kpm")
    if err != nil { t.Fatal(err) }
    _, _ = runner.Control(context.Background(), "status")
    assertArgv(t, []string{"kpm", "ctl0", "stackplz-kpm", "status"})
}

func TestBreakpointRejectsKernelAddress(t *testing.T) {
    _, err := BreakCommand(Breakpoint{ID: 1, Kind: BreakExecute,
        Address: 0xffff000000001000, Length: 4, Mode: BreakOnce})
    if !errors.Is(err, ErrInvalidAddress) { t.Fatalf("got %v", err) }
}
```

- [x] **Step 2: Run and confirm failure**

Run: `go test ./user/kpm -run 'Test(ExecRunner|Breakpoint|Bind)' -v`

Expected: FAIL because the protocol types do not exist.

- [x] **Step 3: Implement protocol types and canonical command builders**

Use typed enums and value objects:

```go
type BindMode string
const (BindPID BindMode = "pid"; BindTGID BindMode = "tgid"; BindEither BindMode = "either")
type BreakKind string
const (BreakExecute BreakKind = "x"; BreakRead BreakKind = "r"; BreakWrite BreakKind = "w"; BreakReadWrite BreakKind = "rw")
type BreakMode string
const (BreakOnce BreakMode = "once"; BreakRepeat BreakMode = "repeat")

type BindingRequest struct { PID uint32; Mode BindMode; UID *uint32; Comm string; StartBootTime *uint64 }
type Breakpoint struct { ID uint32; Kind BreakKind; Address uint64; Length uint8; Mode BreakMode }
```

Builders emit keys in a fixed order, reject whitespace/control characters, cap commands at 512 bytes, and never expose a generic raw-command API from the CLI layer.

- [x] **Step 4: Implement response parsing and client state checks**

`parseResponse` accepts exactly one `status=<signed> version=<unsigned>` header followed by bounded `key=value` lines. It rejects duplicate keys, unknown protocol versions, malformed integers, output over 4096 bytes, and nonzero operation status as `*ControlError`. `Client.Status` requires `state=ready` and an exact profile ID before mutation calls.

- [x] **Step 5: Write failing binary event fixtures**

Generate a little-endian fixture for ABI version 1 and assert sequence, identity, comparator state, all x0-x30, SP/PC/PSTATE, and CRC. Add truncation, extra-byte, wrong-size, wrong-version, and corrupted-CRC cases.

- [x] **Step 6: Implement event decoding**

Define a 432-byte v1 record with offsets: header 0..31, binding/break/generation 32..47, task identity 48..103, exception/debug metadata 104..151, x0..x30 at 152..399, SP/PC/PSTATE at 400/408/416, reserved word at 424, and trailing CRC32 at 428. Decode with `encoding/binary`, verify magic `SPZE`, `Size == len(raw)`, and CRC32/IEEE before exposing values. Kernel task addresses remain named `TaskCookie` and are never accepted in outbound commands.

- [x] **Step 7: Run and commit**

Run: `go test ./user/kpm -race -v`

Expected: PASS with the fake runner proving argv boundaries and every invalid fixture rejected.

Commit: `feat: add strict KPM control client`

---

### Task 2: Device profile schema and deterministic generators

**Files:**
- Create: `kpm/profiles/schema.json`
- Create: `kpm/profiles/profiles.json`
- Create: `tools/gen_kpm_profiles.py`
- Create: `tools/tests/test_gen_kpm_profiles.py`
- Create: `kpm/generated/device_profiles.h`
- Create: `user/kpm/generated_profiles.go`
- Create: `user/kpm/generated_profiles_test.go`

**Interfaces:**
- Produces: C `struct spz_device_profile`, `SPZ_DEVICE_PROFILES[]`, and `SPZ_DEVICE_PROFILE_COUNT`.
- Produces: Go `type DeviceProfile`, `var DeviceProfiles`, and `func FindDeviceProfile(id string) (DeviceProfile, bool)`.
- Produces: `python tools/gen_kpm_profiles.py --check` for deterministic CI verification.

- [x] **Step 1: Write failing generator tests**

Test exact PLK110 values, rejection of missing/unknown/negative/duplicate fields, stable sort by profile ID, C string escaping, and byte-for-byte `--check` behavior:

```python
class ProfileTests(unittest.TestCase):
    def test_plk110_offsets_are_exact(self):
        profile = generator.load_profiles(PROFILES)[0]
        self.assertEqual(profile["task"]["pid"], 1800)
        self.assertEqual(profile["task"]["comm"], 2320)
        self.assertEqual(profile["debug"]["brps"], 6)
        self.assertEqual(profile["kernel"]["ack_commit"],
                         "b2a876903b495c444a94b16f50d1463ffe953957")

    def test_unknown_key_is_rejected(self):
        bad = copy.deepcopy(self.valid)
        bad["task"]["guessed_offset"] = 4
        with self.assertRaisesRegex(ValueError, "unknown key"):
            generator.validate_profile(bad)
```

- [x] **Step 2: Run tests and confirm the missing module failure**

Run: `python -m unittest tools.tests.test_gen_kpm_profiles -v`

Expected: FAIL because `tools/gen_kpm_profiles.py` and the profile files do not exist.

- [x] **Step 3: Add the strict schema and the first profile**

The JSON includes identity, task, credential, perf-event, debug, symbol, and quirk sections. Required task values are those in the spec; add the device-BTF values `thread_info.flags=0`, `cred.size=184`, `cred.uid=8`, `max_cpus=8`, `brps=6`, and `wrps=4`:

```json
{
  "id": "oneplus-plk110-a16-b4999618-d05",
  "kernel": {
    "release": "6.12.23-android16-5-gb2a876903b49-ab14541642-4k",
    "build_id": "30b974f296b5639f597e345adb578a0cbcabeb03",
    "ack_commit": "b2a876903b495c444a94b16f50d1463ffe953957",
    "page_size": 4096,
    "va_bits": 39
  },
  "kpatch": {"kpver": "d05", "sdk_commit": "0fe6d142266b80e5aa445a7ea1534f88a8f33a35", "control_under_rcu": true},
  "debug": {"debug_arch": 9, "brps": 6, "wrps": 4, "ctx_cmps": 2, "max_cpus": 8}
}
```

Required symbols are `linux_banner`, `init_task`, `finish_task_switch`, `do_exit`, `breakpoint_handler`, `watchpoint_handler`, `single_step_handler`, `bp_on_reg`, `wp_on_reg`, `__per_cpu_offset`, `nr_cpu_ids`, `system_unbound_wq`, `queue_work_on`, `schedule_on_each_cpu`, and `ktime_get_mono_fast_ns`. The schema uses `additionalProperties: false` at every object level and bounds every offset to its containing structure.

- [x] **Step 4: Implement the standard-library generator**

Implement these exact call points and strict top-level loader:

```python
def load_profiles(path: pathlib.Path) -> list[dict]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if set(document) != {"schema_version", "profiles"}:
        raise ValueError("unknown or missing top-level key")
    if document["schema_version"] != 1 or not isinstance(document["profiles"], list):
        raise ValueError("invalid schema version or profiles")
    profiles = document["profiles"]
    for profile in profiles:
        validate_profile(profile)
    ids = [profile["id"] for profile in profiles]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate profile id")
    return sorted(profiles, key=lambda profile: profile["id"])

def write_or_check(path: pathlib.Path, content: str, check: bool) -> bool:
    content = content.rstrip("\n") + "\n"
    current = path.read_text(encoding="utf-8") if path.exists() else None
    if check:
        return current == content
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")
    return True
```

`validate_profile`, `render_c`, and `render_go` are total functions over the closed schema: they use exact-key sets for each nested object, range-check every integer, escape every generated string, and render every field rather than copying unvalidated JSON fragments.

`--check` exits 1 and names stale outputs without writing. Normal mode writes UTF-8/LF with a final newline and sorts profiles by ID.

- [x] **Step 5: Generate both outputs and add a Go registry test**

```go
func TestFindDeviceProfile(t *testing.T) {
    p, ok := FindDeviceProfile("oneplus-plk110-a16-b4999618-d05")
    if !ok || p.Task.PID != 1800 || p.Debug.BRPs != 6 {
        t.Fatalf("wrong generated profile: %#v", p)
    }
    if _, ok := FindDeviceProfile("unknown"); ok {
        t.Fatal("unknown profile matched")
    }
}
```

- [x] **Step 6: Run and commit**

Run:

```bash
python -m unittest tools.tests.test_gen_kpm_profiles -v
python tools/gen_kpm_profiles.py --check
go test ./user/kpm -run TestFindDeviceProfile -v
```

Expected: all PASS and generation reports no stale files.

Commit: `feat: add generated KPM device profiles`

---

### Task 3: Procfs-free target and breakpoint address preparation

**Files:**
- Create: `cli/cmd/target_prepare.go`
- Create: `cli/cmd/target_prepare_test.go`
- Modify: `user/util/helper.go`
- Modify: `user/event/event_mmap2.go`
- Create: `user/event/event_mmap2_test.go`

**Interfaces:**
- Produces: `type TargetSource string` with `proc`, `kpm`, `auto` and `ParseTargetSource`.
- Produces: `type TargetResolver interface { UIDFromProc(uint32) (uint32, error); IdentityFromKPM(context.Context, uint32) (kpm.Identity, error) }`.
- Produces: `preparePIDTargets(context.Context, TargetResolver, TargetSource, []uint32, *uint32) ([]ResolvedTarget, error)`.
- Produces: `event.LoadMapsFile(pid uint32, filename string) error` and `resolveBreakpointBase(pid, lib, mapsFile string, explicitBase uint64) (uint64, error)`.

- [x] **Step 1: Write a no-proc regression test**

```go
func TestPreparePIDTargetsKPMNeverCallsProc(t *testing.T) {
    fake := &fakeTargetResolver{kpmIdentity: kpm.Identity{PID: 31337, UID: 10234}}
    got, err := preparePIDTargets(context.Background(), fake, TargetSourceKPM,
        []uint32{31337}, nil)
    if err != nil { t.Fatal(err) }
    if fake.procCalls != 0 { t.Fatalf("proc called %d times", fake.procCalls) }
    if got[0].UID != 10234 { t.Fatalf("uid=%d", got[0].UID) }
}
```

Add `auto` fallback tests, explicit UID override tests, multi-PID rejection for direct breakpoint mode, and errors that preserve both proc and KPM causes.

- [x] **Step 2: Run and confirm failure**

Run: `go test ./cli/cmd -run 'TestPreparePIDTargets' -v`

Expected: FAIL because the pure preparation helper is missing.

- [x] **Step 3: Make PID lookup return errors instead of panicking**

Replace `FindUidByPid(pid) uint32` with `FindUidByPid(pid) (uint32, error)`. Keep the existing `ps -o uid= -p` behavior only in the proc implementation, trim output, and return contextual parse/exit errors. Update all callers.

- [x] **Step 4: Implement source selection as a pure dependency-injected helper**

`kpm` never calls `UIDFromProc`; `proc` never calls KPM; `auto` tries proc once and then KPM once. An explicit UID bypasses both UID lookups but KPM mode still waits for scheduler identity to prevent arming an unknown/reused PID.

- [x] **Step 5: Write offline maps/base tests**

Use a temp maps fixture containing `/data/app/example/lib/arm64/libtarget.so`, then assert library-name and full-path lookup. Assert KPM mode with `--brk-lib` and neither maps nor base returns: `--brk-lib with --task-source=kpm requires --maps-file or --brk-base`.

- [x] **Step 6: Implement offline map loading and explicit base precedence**

`LoadMapsFile` uses `os.ReadFile` and existing `ParseMapsContent`; it never falls back to `/proc`. Base precedence is `--brk-base`, then `--maps-file`, then proc maps only for `task-source=proc/auto` when proc resolution succeeded.

- [x] **Step 7: Run and commit**

Run:

```bash
go test ./cli/cmd -run 'TestPreparePIDTargets|TestResolveBreakpointBase' -v
go test ./user/event -run 'TestLoadMapsFile' -v
```

Expected: PASS and the KPM fake reports zero proc calls.

Commit: `refactor: isolate proc-free target preparation`

---

### Task 4: CLI flags, conservative backend selection, and KPM module lifecycle

**Files:**
- Modify: `user/config/config_global.go`
- Modify: `user/config/config_module.go`
- Modify: `cli/cmd/root.go`
- Create: `cli/cmd/kpm_options_test.go`
- Modify: `user/module/const.go`
- Create: `user/module/kpm_brk.go`
- Create: `user/module/kpm_brk_test.go`
- Modify: `user/module/imodule.go`

**Interfaces:**
- Adds flags `--task-source`, `--brk-backend`, `--kpm-profile`, `--kpm-control`, `--kpm-module`, `--maps-file`, `--brk-base`, `--brk-mode`, and `--kpm-bind-timeout`.
- Produces: `type BreakBackend string` with `perf`, `kpm-direct`, `auto` and `selectBreakBackend`.
- Produces: registered module `MODULE_NAME_KPM_BRK` implementing `module.IModule` with a polling KPM client and idempotent cleanup.

- [x] **Step 1: Write failing config/default tests**

Assert defaults remain `task-source=proc`, `brk-backend=perf`, `brk-mode=once`, module `stackplz-kpm`, binary `/data/adb/modules/KPatch-Next/bin/kpatch`, and profile `oneplus-plk110-a16-b4999618-d05`. Assert explicit KPM does not fall back and `auto` logs/returns its selected backend.

- [x] **Step 2: Run and confirm failure**

Run: `go test ./cli/cmd -run 'TestKPMOptions|TestSelectBreakBackend' -v`

Expected: FAIL because KPM options are absent.

- [x] **Step 3: Add configuration fields and Cobra flags**

Add the exact fields to both config structs, copy them in `InitCommonConfig`, register flags beside existing breakpoint flags, and validate combinations before any target procfs access. Fix the existing breakpoint-length predicate from `<= 0 && > 8` to `== 0 || > 8`.

- [x] **Step 4: Refactor persistent preparation around the pure target helper**

Lazily read `/data/system/packages.list` only when name/UID/package lookup is needed. For KPM targets, populate PID and returned UID filters without `FindUidByPid`, `FindLibPaths`, `SaveMaps`, or `CacheMaps`. Load `--maps-file` before `FindLibInMaps`; use `--brk-base` directly. Existing proc mode follows the old path.

- [x] **Step 5: Write failing module lifecycle tests with a fake runner**

Test command order `status -> bind -> wait bound -> break -> enable`, poll decoding, cancellation, startup rollback, and exactly-once `disable -> clear` cleanup even when polling returns an error:

```go
func TestKPMBRKCloseAlwaysDisarms(t *testing.T) {
    fake := newScriptedRunner(ready, bound, configured, enabled)
    mod := newKPMBRKForTest(fake)
    if err := mod.Start(); err != nil { t.Fatal(err) }
    if err := mod.Close(); err != nil { t.Fatal(err) }
    fake.requireSuffix(t, "disable id=1", "clear")
}
```

- [x] **Step 6: Implement `KPMBRK` without eBPF/perf dependencies in its run path**

The adapter may satisfy unused `IModule` eBPF methods with empty collections, but its `Run` must call its own `Start` and launch a context-bound `poll` loop. It logs one stable text/JSON record per decoded hit. `Close` cancels polling, waits for the goroutine, then attempts both disable and clear; it joins cleanup errors instead of skipping the second action.

- [x] **Step 7: Select the module in `runFunc` and remove premature `os.Exit`**

Choose `MODULE_NAME_KPM_BRK` only for the selected direct backend. Make startup return errors through a testable helper instead of exiting before deferred cleanup. Existing perf selection continues to choose `MODULE_NAME_BRK`.

- [x] **Step 8: Run and commit**

Run:

```bash
go test ./cli/cmd ./user/module -race -v
go test ./user/kpm ./user/event ./user/util -race -v
```

Expected: PASS; the fake proves explicit KPM never invokes perf or proc runners and cleanup is idempotent.

Commit: `feat: integrate explicit KPM task and breakpoint backends`

---

### Task 5: Shared C ABI, command parser, and fixed event rings

**Files:**
- Create: `kpm/include/stackplz/abi.h`
- Create: `kpm/include/stackplz/core.h`
- Create: `kpm/core/command.c`
- Create: `kpm/core/ring.c`
- Create: `kpm/core/crc32.c`
- Create: `kpm/tests/test_command.c`
- Create: `kpm/tests/test_ring.c`
- Create: `kpm/tests/test_main.c`
- Create: `kpm/tests/Makefile`

**Interfaces:**
- Produces: `int spz_parse_command(char *, size_t, struct spz_command *)` for the closed grammar in the spec.
- Produces: `int spz_ring_push(struct spz_ring *ring, uint32_t cpu, const struct spz_event *event)` and `int spz_ring_pop_after(struct spz_ring *ring, uint64_t after, struct spz_event *out)`.
- Produces: the exact packed event layout consumed by Go Task 1 and compile-time size/offset assertions.

- [x] **Step 1: Write parser and ring tests before C implementation**

Table-test all valid commands and reject empty/overlong/non-ASCII input, unknown/duplicate keys, extra positional tokens, signed PID, zero IDs, overflow, kernel VAs, invalid alignment/length/type/mode, and embedded control characters. Ring tests cover per-CPU order, global sequence monotonicity, wrap/loss counting, stale `after`, and CRC stability.

- [x] **Step 2: Run and confirm link failures**

Run: `make -C kpm/tests clean test`

Expected: FAIL with undefined `spz_parse_command`/`spz_ring_*` symbols.

- [x] **Step 3: Define bounded ABI types and static assertions**

Use `SPZ_MAX_CPUS=8`, `SPZ_RING_CAPACITY=64`, `SPZ_COMM_LEN=16`, `SPZ_MAX_COMMAND=512`, fixed-width integers, no native pointers in the wire layout, magic `SPZE`, ABI version 1, the 432-byte offsets fixed in Task 1, and a trailing IEEE CRC32. Assert every Go-decoded offset in C.

- [x] **Step 4: Implement the closed parser without allocation**

Tokenize a caller-owned buffer, track a bitmask of seen keys, use overflow-checked local numeric parsers, and fill a tagged `struct spz_command`. Do not call `kstrto*` in core code so host and KPM builds exercise the same logic.

- [x] **Step 5: Implement fixed per-CPU single-producer rings**

Publish payload then commit sequence with `__atomic_store_n(&slot->commit, sequence, __ATOMIC_RELEASE)`; consume with acquire loads. Full rings overwrite nothing: increment `lost`, reject the push, and publish a bounded loss event when space exists.

- [x] **Step 6: Run sanitizers and commit**

Run:

```bash
make -C kpm/tests clean test
make -C kpm/tests sanitize
```

Expected: all C tests PASS under `-Wall -Wextra -Werror`, ASan, and UBSan.

Commit: `feat: add bounded KPM command and event core`

---

### Task 6: Runtime profile validation and scheduler task identity model

**Files:**
- Create: `kpm/include/stackplz/profile.h`
- Create: `kpm/include/stackplz/task.h`
- Create: `kpm/core/profile.c`
- Create: `kpm/core/task.c`
- Create: `kpm/tests/test_profile.c`
- Create: `kpm/tests/test_task.c`
- Modify: `kpm/tests/Makefile`

**Interfaces:**
- Consumes: generated `SPZ_DEVICE_PROFILES` from Task 2.
- Produces: `spz_profile_select(id)`, `spz_profile_validate(profile, runtime_ops, reason, cap)`.
- Produces: `spz_binding_set`, `spz_binding_observe_current`, `spz_binding_matches_current`, `spz_binding_mark_exit`, and `spz_binding_clear`.
- Runtime operations expose bounded symbol lookup, kernel banner, current-task bytes, debug counts, CPU count, and per-CPU owner pointers; tests provide fakes.

- [x] **Step 1: Write profile failure-injection tests**

Start from a valid fake runtime and independently corrupt profile ID, kernel release, page size, CPU count, BRP/WRP count, every required symbol, `init_task` PID/TGID/comm, credential pointer, and task offset bounds. Assert exact reason codes and that `hooks_allowed` stays false after any failure.

- [x] **Step 2: Run and confirm failure**

Run: `make -C kpm/tests test TEST=test_profile`

Expected: FAIL because the profile runtime interface is missing.

- [x] **Step 3: Implement transactional validation**

Selection is exact string equality. Validation first checks generated bounds, then kernel banner/release, all symbols, `nr_cpu_ids <= max_cpus`, current `ID_AA64DFR0_EL1` counts, and `init_task` invariants. It records the initial online CPU bound and rejects direct arming if that bound changes. Publish resolved addresses and `SPZ_PROFILE_READY` only after the final check; zero the runtime table on failure.

- [x] **Step 4: Write task-binding/PID-reuse tests**

Use byte arrays with generated offsets. Cover PID/TGID/either matching, UID/comm/start constraints, first observation, same identity on another CPU, mismatched start time becoming `STALE`, `do_exit` becoming `EXITED`, generation rollover that skips zero, and clearing while an observer snapshots the old generation.

- [x] **Step 5: Implement copy-only current-task observation**

Read fields only from the callback's live current pointer. Publish `TaskCookie`, scalar identity, and generation atomically; never retain a dereferenceable pointer. `spz_binding_matches_current` re-reads PID/TGID/start/exit state from its live argument and rejects a generation mismatch.

- [x] **Step 6: Run and commit**

Run: `make -C kpm/tests clean test sanitize`

Expected: PASS with all profile corruption and PID-reuse cases covered.

Commit: `feat: validate KPM profiles and scheduler identities`

---

### Task 7: Direct debug-register ownership state machine

**Files:**
- Create: `kpm/include/stackplz/debug.h`
- Create: `kpm/core/debug.c`
- Create: `kpm/tests/mock_debug_regs.c`
- Create: `kpm/tests/test_debug.c`
- Modify: `kpm/tests/Makefile`

**Interfaces:**
- Produces: `struct spz_debug_ops` with indexed read/write functions for BVR/BCR/WVR/WCR/MDSCR and Linux owner lookup.
- Produces: `spz_debug_validate_request`, `spz_debug_arm_current`, `spz_debug_restore_cpu`, `spz_debug_handle_break`, `spz_debug_handle_watch`, and `spz_debug_handle_step`.
- Produces: per-CPU states `EMPTY`, `SNAPSHOTTED`, `ARMED`, `HIT_DISABLED`, `STEP_PENDING`, `ONE_SHOT_DONE`, `RESTORING`, and `QUARANTINED`.

- [x] **Step 1: Write state-transition and coexistence tests**

Use six mock BRPs/four WRPs. Assert highest free slot selection, owner-null plus enable-clear requirement, `-EBUSY` on slot exhaustion or bookkeeping/register disagreement, exact value/control encoding, one-shot disable, repeat step/re-arm, pre-existing SS preservation, migration restore/re-arm, and no write when a programmed slot was changed by another owner.

- [x] **Step 2: Run and confirm failure**

Run: `make -C kpm/tests test TEST=test_debug`

Expected: FAIL because the state machine does not exist.

- [x] **Step 3: Implement request validation and control encoding**

Execute requests require length 4 and word-aligned address. Watchpoints accept lengths 1, 2, 4, or 8 and may span only one aligned eight-byte window. Encode BAS at bits 5..12, load/store at bits 3..4, EL0 privilege `2` at bits 1..2, and enable at bit 0. Reject kernel/cross-window addresses and raw controls.

- [x] **Step 4: Implement conditional arm/restore**

Snapshot owner, value, control, and MDSCR on the same CPU. Choose the highest safe index, program value then control, set only `MDSCR.MDE`, and issue barriers through ops. Restore only if live value/control equals the module's armed or hit-disabled image. Clear an MDE bit added by the module only when no Linux owner/live enabled comparator requires it; otherwise preserve it and emit interference.

- [x] **Step 5: Implement owned exception and step handling**

An execute hit requires matching current identity, PC/BVR/BAS, CPU, slot, and generation. A watch hit also validates ESR access direction and watched byte range. Copy registers before disabling. `once` atomically disables the request; `repeat` snapshots SS state, sets only needed SS bits, and re-arms on the matching step. A kernel-mode watch hit records `KERNEL_UACCESS` and degrades to one-shot rather than enabling unsafe kernel stepping.

- [x] **Step 6: Add randomized model testing**

Generate 100,000 deterministic operations across schedule-in/out, hit, step, enable/disable, foreign overwrite, and clear. After each operation assert no two owners share a slot, non-owned registers are unchanged, quarantined slots are never written, and `EMPTY` contains no saved ownership.

- [x] **Step 7: Run and commit**

Run: `make -C kpm/tests clean test sanitize MODEL_STEPS=100000`

Expected: PASS with no sanitizer finding and a stable seed printed for reproduction.

Commit: `feat: add direct ARM64 debug ownership model`

---

### Task 8: ARM64 raw register backend and disassembly proof

**Files:**
- Create: `kpm/platform/arm64/debug_regs.h`
- Create: `kpm/platform/arm64/debug_regs.c`
- Create: `kpm/tests/test_arm64_encoding.c`
- Modify: `kpm/tests/Makefile`

**Interfaces:**
- Implements Task 7 `spz_debug_ops` with direct `DBGBVR<n>_EL1`, `DBGBCR<n>_EL1`, `DBGWVR<n>_EL1`, `DBGWCR<n>_EL1`, `MDSCR_EL1`, `ID_AA64DFR0_EL1`, `SP_EL0`, and `TPIDR_EL1` access.
- Produces: `spz_arm64_num_brps`, `spz_arm64_num_wrps`, `spz_arm64_current_task`, `spz_arm64_current_cpu_offset`, and `spz_arm64_barrier`.

- [x] **Step 1: Write control-encoding tests independent of ARM64 execution**

Cross-compile a small test object that references indices 0, 5, and 15 and assert compile-time range handling. Host tests compare expected controls: execute address `0x1000`, length 4 -> BAS `0x0f`; watch address `0x1003`, length 2 -> BAS `0x18`; read/write type -> both load/store bits.

- [x] **Step 2: Implement constant-index register accessors**

Use switch cases generated by a local macro so every `mrs/msr` operand is compile-time constant. Out-of-range reads return a typed error and writes do nothing. Each programming sequence ends `dsb ish; isb`; no dynamic system-register string or perf symbol appears in this directory.

- [x] **Step 3: Cross-compile and inspect instructions**

Run on the VM:

```bash
aarch64-linux-gnu-gcc -std=gnu11 -O2 -ffreestanding -mgeneral-regs-only -c kpm/platform/arm64/debug_regs.c -o /tmp/debug_regs.o
aarch64-linux-gnu-objdump -d /tmp/debug_regs.o
aarch64-linux-gnu-nm -u /tmp/debug_regs.o
```

Expected: disassembly contains `mrs/msr` for DBGB*/DBGW*/MDSCR; undefined-symbol output contains no `perf_event`, `register_*hw_breakpoint`, or allocator API.

- [x] **Step 4: Commit**

Commit: `feat: program ARM64 debug registers directly`

---

### Task 9: KPatch runtime, hooks, asynchronous CPU restoration, and KPM entry

**Files:**
- Create: `kpm/platform/kpatch/compat.h`
- Create: `kpm/platform/kpatch/runtime.c`
- Create: `kpm/platform/kpatch/hooks.c`
- Create: `kpm/platform/kpatch/async.c`
- Create: `kpm/platform/kpatch/control.c`
- Create: `kpm/platform/kpatch/main.c`
- Create: `kpm/Makefile`
- Create: `kpm/tests/test_hooks.c`
- Create: `kpm/tests/test_async.c`
- Modify: `kpm/tests/Makefile`

**Interfaces:**
- Consumes: generated profile, core parser/ring/task/debug APIs, raw ARM64 ops, and pinned KPatch headers.
- Produces KPM name `stackplz-kpm`, version `0.1.0`, `KPM_INIT`, `KPM_CTL0`, and `KPM_EXIT`.
- Wraps exact symbols with `hook_wrap1(finish_task_switch)`, `hook_wrap1(do_exit)`, and `hook_wrap3` for breakpoint/watchpoint/single-step handlers.
- Produces async request states `FREE`, `PENDING`, `RUNNING`, `DONE` for enable/disable/clear/audit CPU work.

- [x] **Step 1: Write hook-order and rollback tests using fake hook APIs**

Assert install order profile -> finish-switch -> exit -> breakpoint -> watchpoint -> step; inject failure at every hook and assert reverse unhook of only installed entries. Verify finish pre restores old CPU state, original executes, finish post observes/arms current. Verify owned exceptions set `skip_origin=1, ret=0`; unrelated exceptions never change fargs.

- [x] **Step 2: Write asynchronous lifecycle tests**

Model d05 calling control/exit inside an RCU read section. Assert mutating commands enqueue fixed work and return a request ID without waiting; worker calls `schedule_on_each_cpu`; `request` polling reports completion. Exit returns `-EBUSY` while any binding/debug/async state is live and succeeds only after completed clear, without invoking `synchronize_rcu`.

- [x] **Step 3: Run and confirm failures**

Run: `make -C kpm/tests test TEST='test_hooks test_async'`

Expected: FAIL because KPatch adapters are missing.

- [x] **Step 4: Resolve and validate the exact runtime ABI**

Resolve all generated symbol names through `kallsyms_lookup_name`. Convert addresses to typed function pointers through unions, not object/function pointer casts. Compute per-CPU owner arrays as `symbol + __per_cpu_offset[cpu]`. Current task comes only from `SP_EL0`; current CPU matches `TPIDR_EL1` against the bounded offset table.

- [x] **Step 5: Implement scheduler and exit hooks**

At finish pre, call conditional restore for the current CPU before Linux perf sched-in. At finish post, copy/validate live current identity and arm after Linux's restore. The exit-before callback only compares/copies current identity and marks the binding exited. All callbacks are bounded and silent.

- [x] **Step 6: Implement debug exception hooks**

Map KPatch `hook_fargs3_t` exactly to `(addr/unused, esr, regs)`. Call core handlers with current CPU/task and raw register ops. Owned hits publish a v1 ring event before disabling and consume only their own exception. The single-step hook restores repeat mode and allows the original handler only when step state pre-existed.

- [x] **Step 7: Implement fixed async work and d05-safe teardown**

Use a statically allocated work item queued on resolved `system_unbound_wq`. Outside control's RCU section it invokes resolved `schedule_on_each_cpu` with bounded callbacks for enable/restore/audit. Audit snapshots each Linux owner pointer, live register, and the profiled perf-event fields only after bounded pointer/plausibility checks. `clear` disables new arms, restores all CPUs, waits for handler counters in worker context, clears identities, and marks request done. KPM exit performs no blocking grace wait and refuses unsafe unload.

- [x] **Step 8: Implement bounded `ctl0` responses**

Copy at most 512 command bytes into local storage, parse with Task 5, serialize status/profile/bind/break/request/audit fields with `scnprintf`, and return one `event=<hex>` per poll. Copy at most `min(outlen, 4096)` through `compat_copy_to_user`. Unknown/duplicate operations return negative status and no mutation.

- [x] **Step 9: Implement transactional KPM init/exit**

Init requires `profile=<exact-id>`, validates before hooks, initializes fixed state, then installs hooks with rollback. Exit checks quiescence, unwraps in reverse order, zeroes runtime addresses, and returns success. It never auto-arms or writes debug registers at load.

- [x] **Step 10: Run host tests, cross-build, and commit**

Run:

```bash
make -C kpm/tests clean test sanitize
make -C kpm clean all KP_DIR=/path/to/KPatch-Next TARGET_COMPILE=aarch64-linux-gnu-
aarch64-linux-gnu-readelf -h -S -s kpm/stackplz-kpm.kpm
aarch64-linux-gnu-nm -u kpm/stackplz-kpm.kpm
```

Expected: host tests PASS; KPM is AArch64 relocatable; undefined symbols are limited to the pinned KPatch/runtime allowlist and contain no perf breakpoint registration call.

Commit: `feat: add record-only stackplz KPM companion`

---

### Task 10: Operator documentation, profile adaptation guide, and build automation

**Files:**
- Create: `docs/KPM_FORENSICS.md`
- Create: `kpm/README.md`
- Create: `kpm/scripts/verify_artifact.sh`
- Create: `tests/kpm/device_acceptance.md`
- Modify: `docs/BUILD.md`
- Modify: `README.md`
- Modify: `Makefile`

**Interfaces:**
- Produces: `make kpm-host-test`, `make kpm-generate-check`, and documented `make -C kpm KP_DIR=/path/to/KPatch-Next TARGET_COMPILE=aarch64-linux-gnu-` commands.
- Produces: a device-profile adaptation checklist that names required BTF/kallsyms/kernel/KPatch evidence and forbids guessed offsets.

- [x] **Step 1: Write the operator guide around explicit lifecycle**

Document build, manual load with `profile=oneplus-plk110-a16-b4999618-d05`, status/audit, stackplz flags, one-shot/repeat semantics, manual clear, and unload. Show absolute address, `--brk-base`, and `--maps-file` examples. State that stackplz never loads/persists the KPM.

- [x] **Step 2: Document safety/failure interpretation**

Explain `PROFILE_REJECTED`, `PENDING`, `STALE`, `EXITED`, `BUSY`, `QUARANTINED`, loss records, kernel-uaccess watchpoint downgrade, CPU-hotplug restriction, arbitrary-kernel-compromise limitation, and recovery by clear/reboot. Never describe a failure as proof that a hidden task does not exist.

- [x] **Step 3: Document new-device adaptation**

Require exact fingerprint, kernel release/build ID/ACK commit, KPatch ABI/commit, BTF hash, task/cred/perf layouts, debug counts, symbol presence, masked instruction evidence when names are ambiguous, generated-file check, mismatch test, and before/after register snapshots. Include the JSON field map and generator command.

- [x] **Step 4: Add artifact verification script**

The script uses `set -eu`, `readelf`, `nm`, and `objdump`; verifies AArch64 relocatable type, `.kpm.*` sections, direct DBGB/DBGW/MDSCR instructions, absence of perf registration symbols, size limits, and generated-profile freshness. It accepts explicit artifact/KPatch paths and embeds no VM password or device secret.

- [x] **Step 5: Add device acceptance matrix with honest initial state**

Create rows for profile match/mismatch, hidden scheduler task, execute/watch hits, migration, PID reuse, ptrace/perf coexistence, perf-failure independence, slot exhaustion, 100 lifecycle cycles, CPU register restoration, and residual hooks. Mark every physical-device row `NOT TESTED (device unavailable 2026-08-30)`.

- [x] **Step 6: Run documentation/build checks and keep the verified workspace**

Run:

```bash
python tools/gen_kpm_profiles.py --check
make kpm-host-test
make -C kpm/scripts verify ARTIFACT=../stackplz-kpm.kpm
```

Expected: generated files current, host tests PASS, artifact script PASS, and no credential string appears in tracked files.

Commit: `docs: add KPM forensic deployment and adaptation guide`

---

### Task 11: Full verification, source audit, and reproducible handoff evidence

**Files:**
- Create: `artifacts/kpm/host-test-report.txt`
- Create: `artifacts/kpm/cross-build-report.txt`
- Create: `artifacts/kpm/symbol-audit.txt`
- Create: `artifacts/kpm/disassembly-audit.txt`
- Create: `artifacts/kpm/SHA256SUMS`
- Modify: `tests/kpm/device_acceptance.md`

**Interfaces:**
- Produces a reproducible evidence bundle; no physical-device row changes from `NOT TESTED` without actual captured output.

- [x] **Step 1: Run the complete local deterministic suite from a clean tree**

Run:

```bash
python -m unittest discover -s tools/tests -v
python tools/gen_kpm_profiles.py --check
go test ./user/kpm ./user/event ./user/util ./user/module ./cli/cmd -race -count=1
make -C kpm/tests clean test sanitize MODEL_STEPS=100000
```

Expected: all PASS. Capture commands, tool versions, exit statuses, and output in `host-test-report.txt`.

- [x] **Step 2: Cross-build on the authorized VM**

Use `/path/to/KPatch-Next` at the pinned commit and `aarch64-linux-gnu-` for KPM. Place stackplz with its custom sibling `ebpf`/`ebpfmanager` dependencies, use the installed Android NDK, and build ARM64. Record compiler/Go/NDK versions, commits, commands, exit statuses, artifact metadata, and SHA-256 values.

- [x] **Step 3: Perform static source and symbol audits**

Run searches proving direct-backend files contain no perf registration, allocator, sleep, print, user-memory read, unbounded task-list traversal, auto-load, or persistence calls. Audit every `hook_wrap` has reverse rollback/unhook and every raw register write has a conditional restore path. Capture `nm/readelf/objdump` outputs and a direct-register instruction summary.

- [x] **Step 4: Run repository-wide formatting and regression checks**

Run `gofmt` on changed Go, `python -m compileall tools`, `git diff --check`, generator `--check`, all focused tests, and the full build supported by the VM dependency layout. Review `git diff` for unrelated/user changes before staging.

- [x] **Step 5: Update evidence with only verified claims**

Write artifact hashes and host/cross-build results. Keep device runtime as `NOT TESTED`; include the exact command matrix ready for the device's return. Do not use words such as “guaranteed on device” without device evidence.

Commit: `test: verify KPM forensic observer build and invariants`

- [x] **Step 6: Request code review and address findings**

Use `superpowers:requesting-code-review`, check the implementation against this plan/spec and the record-only boundary, then apply only technically verified changes with focused regression tests.

---

## Execution choice

The user explicitly requested autonomous completion and no delegated work. Execute this plan inline with `superpowers:executing-plans`, preserving the test/commit checkpoints above. Do not spawn subagents. Stop only for a genuinely missing authorization or an unrecoverable external dependency; physical-device-only cases remain documented as `NOT TESTED` rather than blocking host/cross-build completion.

Execution override (2026-08-30): the user requested no Git commits. All local
implementation commits were unwound to baseline `62fc9e8`; completed source,
documentation, reports, and build artifacts remain in the current workspace.
