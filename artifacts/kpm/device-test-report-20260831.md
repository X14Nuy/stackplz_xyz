# stackplz / KPM device test report (2026-08-31)

## Device and artifacts

- Device: OnePlus PLK110, Android 16, Linux `6.12.23-android16-5-gb2a876903b49-ab14541642-4k`
- Native BTF: `/sys/kernel/btf/vmlinux`, SHA-256 `75b509d7abaed46f57cf26c3780122b5abfae889cde4a22edf1812bba3ce3320`
- Final tested stackplz SHA-256: `13016228a580302e548a61d3895b1b28fc9fc1978eed0418d1147ad81be7aed3`
- Final tested KPM SHA-256: `786e3d5030e4f2021e80c39f2a3ef28aa84f8bbf2980946f72d4c306052e7b18`
- Test target: native arm64 `write_loop`, repeatedly calling libc `write` and `usleep`

All eBPF tests used the device-native BTF (`--btf`). The earlier zero-event result was invalid: the test stopped before the syscall programs finished loading, and `--nocheck` without `--btf` incorrectly selected the embedded Android 12 / 5.10 BTF.

## Results

| Scenario | Result | Evidence |
|---|---:|---|
| Original stackplz, visible target, syscall | PASS | 5,446 target lines in 3 s; correct PID/TID |
| Original stackplz, visible target, uprobe | PASS | 2,494 target lines in 2 s; correct PID/TID |
| Rebuilt stackplz, visible syscall | PASS | 4,388 target lines in 2 s |
| Rebuilt stackplz, visible uprobe | PASS | 1,242 target lines in 1 s; registers and symbolized backtrace |
| Rebuilt stackplz, visible perf execute breakpoint | PASS | 247 target/backtrace lines in 1 s |
| Hidden target sanity check | PASS | bind-mounted empty directory on `/proc/<pid>`; `ps -p` returned only its header; `task-source=proc` failed |
| Hidden target, explicit KPM syscall | PASS | 4,278 target lines in 2 s; correct PID/TID and syscall return value |
| Hidden target, explicit KPM uprobe | PASS (capture) | 507 target lines in 1 s; correct registers |
| Hidden target, KPM + original perf breakpoint | PASS | 1,382 target lines in 1 s using a pre-hide maps snapshot |
| Hidden target, KPM-direct breakpoint | PASS | 168 hits in 2 s; full x0-x30/SP/PC/PSTATE |
| Hidden target, `task-source=auto` syscall | PASS | 4,532 target lines in 2 s after proc lookup became unavailable |
| Hidden target, `task-source=auto` uprobe | PASS (capture) | 1,750 target lines in 1 s |
| Hidden target, `task-source=auto` perf breakpoint | PASS | 1,302 breakpoint events in 1 s using a pre-hide maps snapshot |
| Consecutive KPM-direct sessions | PASS | 90 then 93 hits; zero `stale` and zero poll errors |
| KPM audit and cleanup | PASS | `request_status=0`; final state `ready`, binding `none`, configured/enabled `0` |
| Hidden target, KPM `show_map_vma` snapshot | PASS | `ps` only returned its header and `/proc/<pid>/maps` was unreadable; KPM returned 8,595 bytes and the chunk-reassembled data was byte-identical to the pre-hide baseline |
| Final rebuilt stackplz, visible syscall | PASS | 4,086 target lines in 2 s |
| Final rebuilt stackplz, visible uprobe + stack | PASS | 1,161 calls in 1 s; `libc.so` and `write_loop` frames symbolized |
| Final hidden target, automatic KPM syscall | PASS | 3,792 target events in 2 s without `--maps-file` |
| Final hidden target, KPM uprobe + stack | PASS | 1,055 calls in 1 s; 2,110 `libc.so` frames and zero maps errors, without `--maps-file` |
| Final hidden target, perf breakpoint | PASS | 1,569 breakpoint event records in 1 s; resolved `libc.so+0xef700`, without `--maps-file` or `--brk-base` |
| Final hidden target, KPM-direct breakpoint | PASS | 238 KPM hits in 1 s, without `--maps-file` or `--brk-base` |

## Fixes validated during testing

1. `CLEAR` now resets the KPM event ring only after handlers have quiesced and debug slots are restored. This prevents a new KPM-direct client starting at sequence zero from receiving `ESTALE` after an earlier session.
2. KPM-direct shutdown no longer records the expected `CommandContext` process kill as a poll failure.
3. Idle task PID/TGID zero and signed per-CPU offset behavior have dedicated host regression tests.

## Hidden-process maps implementation

The remaining gap is now closed for the tested kernel profile. After scheduler
binding, KPM retains the live task and asynchronously renders the bound
`mm_struct` with the kernel's `show_map_vma`. The bounded snapshot is exported
through versioned, offset-checked chunks with a snapshot id, total length and
CRC32. The Go client reconstructs and validates the snapshot, then injects it
into stackplz's existing maps cache before uprobe/stack/breakpoint preparation.

All formatter symbols and structure offsets are profile data. If a future
kernel cannot resolve the optional maps symbols, KPM remains usable for its
core task binding and direct-breakpoint functionality and reports
`maps_supported=0`; it does not guess an ABI.

The final device test hid `/proc/<pid>` before requesting the snapshot. KPM
returned 8,595 bytes in six chunks; the reconstructed file had SHA-256
`150c6d35ccc93fb15576bbb6a48c916a882b27483d68d87c414c6b4744f30ed4`
and was byte-identical to the maps captured immediately before concealment.

At high event rates, the original perf/stack event processor can also print `workerQueue is not empty` while shutting down. Event capture remained valid and the final process/module cleanup completed, but this pre-existing close race should be tracked separately from KPM binding.

## Final device state

- KPM intentionally remains loaded because hot unload previously blocked.
- Module state: `ready`, `maps_supported=1`, maps state `empty`, no binding, no configured or enabled breakpoint.
- Hidden proc mounts: 0.
- Test `write_loop` / stackplz processes: 0.
- Kernel `BUG`/oops/hung-task/deadlock/KASAN/use-after-free signatures after the final run: 0.
