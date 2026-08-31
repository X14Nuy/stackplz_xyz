# KPM physical-device acceptance

Target profile: `oneplus-plk110-a16-b4999618-d05`

Device availability: **PLK110 tested 2026-08-31**. The focused stackplz/KPM
capability and hidden-maps results are archived in the
[device test report](../../artifacts/kpm/device-test-report-20260831.md).

This checklist must be run on the explicitly authorized PLK110 build before
the KPM path is described as directly usable. Host tests, sanitizers, an ARM64
cross-build, and artifact inspection do not substitute for these tests.

Host/cross-build evidence captured without changing any device status:

- [host test report](../../artifacts/kpm/host-test-report.txt)
- [cross-build report](../../artifacts/kpm/cross-build-report.txt)
- [source/symbol audit](../../artifacts/kpm/symbol-audit.txt)
- [ELF/disassembly audit](../../artifacts/kpm/disassembly-audit.txt)
- [artifact hashes](../../artifacts/kpm/SHA256SUMS)

## Evidence capture

Preserve the following before changing device state:

- full Android build fingerprint, security patch level, `uname -a`, kernel
  build ID, boot ID, page size, online/possible CPU sets;
- KPatch `kpver`, `kver`, installed KPatch version and loader hash;
- KPM SHA-256 and output from `kpm info stackplz-kpm`;
- raw ID_AA64DFR0_EL1 and the current BRP/WRP values, controls, and owner
  bookkeeping from the trusted test harness;
- serial timestamps, `status`, `audit`, stackplz output, kernel log, and the
  return code of every load/control/unload operation.

Use one operator and serialize load/control/unload. Do not CPU-hotplug while a
breakpoint is armed.

## Acceptance matrix

| ID | Test | Pass evidence | Status |
| --- | --- | --- | --- |
| D01 | Exact-profile load | Correct artifact loads only with the exact profile; `status` reports `state=ready` and the exact profile ID | PASS (exact profile; focused device run 2026-08-31) |
| D02 | Profile mismatch | Altered profile ID/kernel release/page size/CPU count/DFR0/missing symbol fails closed with no installed hooks or debug writes | NOT TESTED (device unavailable 2026-08-30) |
| D03 | Visible task bind | A controlled process becomes `binding=bound`; PID/TGID/UID/comm/start times match an independent trusted source | PASS (focused device run 2026-08-31) |
| D04 | Procfs/VFS-hidden task | A test-only VFS concealment fixture hides the controlled process from `ps` and `/proc`, while scheduler observation still binds the same independently known PID | PASS (focused device run 2026-08-31) |
| D04a | Hidden-task VMA snapshot | After binding and VFS concealment, KPM renders the live task's VMAs and the chunked result matches a pre-hide `/proc/<pid>/maps` baseline byte-for-byte | PASS (8,595 bytes; focused device run 2026-08-31) |
| D05 | Non-running limit | A stopped/non-scheduled fixture produces no false “not present” claim; timeout is reported only as missing observation | NOT TESTED (device unavailable 2026-08-30) |
| D06 | PID reuse | Exit and reuse of the same number cannot inherit the prior identity; start-time mismatch becomes stale/exited and requires clear/rebind | NOT TESTED (device unavailable 2026-08-30) |
| D07 | Execute once | An aligned EL0 instruction breakpoint records exactly one owned hit with correct registers and remains disabled afterward | NOT TESTED (device unavailable 2026-08-30) |
| D08 | Execute repeat | Controlled repeated execution records ordered hits, preserves pre-existing SS state, and re-arms only after the owned single step | NOT TESTED (device unavailable 2026-08-30) |
| D09 | Read/write watchpoints | Lengths 1/2/4/8 inside one aligned 8-byte block work for r/w/rw; invalid/cross-block requests are rejected without writes | NOT TESTED (device unavailable 2026-08-30) |
| D10 | Kernel-uaccess downgrade | A repeat watchpoint triggered by kernel access records `KERNEL_UACCESS` and becomes one-shot instead of single-stepping kernel code | NOT TESTED (device unavailable 2026-08-30) |
| D11 | CPU migration | The controlled task migrates across allowed CPUs; old CPU state restores and the new CPU arms without ownership leakage | NOT TESTED (device unavailable 2026-08-30) |
| D12 | Existing debug clients | Ordinary ptrace/perf breakpoint state is neither overwritten nor disabled; unavailable or ambiguous slots return busy | NOT TESTED (device unavailable 2026-08-30) |
| D13 | Interference/quarantine | A test harness changes an owned slot after programming; KPM emits integrity evidence, quarantines, and does not blindly restore over the change | NOT TESTED (device unavailable 2026-08-30) |
| D14 | perf-hook independence | With a test-only perf registration fault fixture, `kpm-direct` still records its owned direct-register breakpoint; the original `perf` backend fails independently | NOT TESTED (device unavailable 2026-08-30) |
| D15 | Read-only audit | `audit` reports occupied/inconsistent hardware and perf bookkeeping without registering, changing, or closing any third-party event | NOT TESTED (device unavailable 2026-08-30) |
| D16 | Ring pressure/loss | Controlled event pressure preserves monotonic committed sequences and emits a counted loss record rather than silently claiming completeness | NOT TESTED (device unavailable 2026-08-30) |
| D17 | stackplz cancellation | SIGINT/SIGTERM and startup failures run disable then clear; all cleanup errors are surfaced | NOT TESTED (device unavailable 2026-08-30) |
| D18 | Clean unload loop | At least 100 serialized load/bind/enable/hit/disable/clear/unload cycles leave original registers/owners intact and show no crash/UAF/leaked module | NOT TESTED (device unavailable 2026-08-30) |
| D19 | Busy unload | Unload during live binding/debug/async work returns busy and leaves a controllable or inert resident image; it never frees active code | NOT TESTED (device unavailable 2026-08-30) |
| D20 | Hook quiescence stress | Scheduler/exit/debug exceptions run concurrently with repeated cleanup; task-RCU quiescence and per-CPU barriers prevent callbacks into freed code | NOT TESTED (device unavailable 2026-08-30) |
| D21 | CPU topology guard | Offline topology-change test is rejected before arming or detected as stale; no live CPU hotplug is attempted while armed | NOT TESTED (device unavailable 2026-08-30) |
| D22 | Reboot recovery | Deliberately induced cleanup refusal is recoverable by reboot; report distinguishes recovery from proof of successful hot unload | NOT TESTED (device unavailable 2026-08-30) |
| D23 | Residual-hook audit | After a successful unload, exact hook slots contain no stackplz callbacks and repeated scheduler/exception traffic produces no KPM event or callback | NOT TESTED (device unavailable 2026-08-30) |

## Completion rule

Do not replace a status with PASS unless the raw command, return code, device
identity, before/after debug state, and logs are archived together. Any crash,
unexplained register change, lost third-party owner, profile mismatch that
continues running, or unload that frees a non-quiescent module is a release
blocker.

“No event observed” is never a PASS for hidden-task absence. The only accepted
claim is that a specific controlled task was or was not observed during the
recorded window under the documented scheduler and kernel assumptions.
