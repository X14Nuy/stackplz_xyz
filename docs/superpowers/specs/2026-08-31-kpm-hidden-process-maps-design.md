# KPM hidden-process maps design

Date: 2026-08-31

Status: approved implementation specification

## Goal

When a target remains scheduler-visible but `/proc/<pid>` is hidden or filtered,
stackplz must obtain an initial, coherent maps snapshot through the already-bound
KPM identity. The snapshot must feed the same user-space maps cache used by the
ordinary procfs path so that library-relative breakpoints, user-stack
symbolization, syscall tracing, uprobes, perf breakpoints, and KPM-direct
breakpoints keep their existing behavior.

Ordinary stackplz defaults remain unchanged. Failure of the optional maps
capability must not reject an otherwise valid KPM profile or disable tracing
features that do not require mappings.

## Exact formatter and portability boundary

The preferred backend calls the target kernel's two-argument
`show_map_vma(struct seq_file *, struct vm_area_struct *)`. The current PLK110
BTF proves the two-argument ABI and the layouts recorded below. The matching
Android 6.12 implementation formats permissions, offsets, device/inode data,
file paths, anonymous names, heap, stack, and special mappings.

`show_map_vma` is a local text symbol on the current kernel. Its address is not
hard-coded. Every device profile contains its symbol name and maps ABI data.
The KPM resolver tries the KernelPatch symbol facility exposed through
`kallsyms_lookup_name`; a future profile may carry a build-relative fallback
only when it is tied to an exact kernel/BTF identity and a reviewed signature.
Raw runtime absolute addresses are never accepted from the control protocol.

Maps symbols are an optional capability set. Missing or invalid maps symbols
set `maps_supported=0` while the core profile remains `ready`.

## Profile data

The generated C and Go profiles gain a `maps` section with:

- capability bounds: maximum snapshot bytes and maximum control chunk bytes;
- `seq_file` size and offsets for `buf`, `size`, `from`, `count`,
  `pad_until`, and `private`;
- `proc_maps_private` size and offsets for `task`, `mm`, and `iter`;
- `vma_iterator`/`ma_state` size and offsets for `tree`, `index`, `last`,
  `node`, and `status`;
- `mm_struct.mm_mt`, `vm_area_struct.vm_start`, and
  `vm_area_struct.vm_end` offsets;
- the profiled `MAS_START` pointer value, `ma_start` enum value, and
  `show_map_vma` argument count.

The symbol table gains:

```text
show_map_vma find_vma mas_walk get_task_mm mmput
mmap_read_lock_killable mmap_read_unlock
rust_helper_get_task_struct rust_helper_put_task_struct
vmalloc_noprof vfree
```

Generator validation proves every offset and field width is within its
containing structure, snapshot/chunk limits are bounded, the formatter ABI is
two arguments, and all names are non-empty. Generated-file checks remain
deterministic.

## Task lifetime

The scheduler observer is the only trusted point at which the hidden task is
known to be live. When a pending binding first transitions to bound, the KPM
takes one `task_struct` reference with the profiled helper and records the
binding generation. The hot path performs no allocation, VMA walk, path
lookup, or sleeping operation.

The retained task reference is released by asynchronous `clear`. A second bind
is rejected while a retained task belongs to the previous generation; the
operator must clear first. This prevents pointer replacement and reference
leaks. A task exit may make a later maps request return `-ESRCH`, but it cannot
turn the saved pointer into a use-after-free.

## Snapshot execution

The command `maps` submits `SPZ_ASYNC_MAPS`. It never renders inside KPatch
`ctl0`, because this device invokes control while holding an RCU read-side
section and the formatter may sleep during mmap locking and path lookup.

The unbound workqueue worker performs the following transaction:

1. Verify maps support, a bound identity, a matching retained task generation,
   and no concurrently armed maps writer.
2. Call `get_task_mm(task)`. Return `-ESRCH` if the task no longer owns an MM.
3. Allocate a bounded vmalloc buffer only for this explicit snapshot request.
4. Acquire `mmap_read_lock_killable(mm)` for a coherent address-space view.
5. Starting at address zero, call `find_vma(mm, cursor)`. For each result,
   initialize a real profiled `vma_iterator` over `mm->mm_mt`, position it with
   `mas_walk`, verify it resolves to the same VMA, then call `show_map_vma` with
   a profiled in-memory `seq_file` and `proc_maps_private`.
6. Read `vm_end` through the profiled offset and require strict forward
   progress. Overflow, a full seq buffer, malformed ranges, iterator mismatch,
   or signal interruption fails the whole snapshot rather than publishing
   partial maps.
7. Release the mmap lock and MM reference, calculate IEEE CRC32, and atomically
   publish the completed immutable buffer under the async request ID.

Only completed buffers are readable. A failed refresh leaves the previous
completed snapshot intact. `clear` blocks new reads, waits for an in-flight
bounded reader, frees the snapshot, releases the task reference, then resets
the binding and event ring. No maps allocation occurs in scheduler or debug
exception handlers.

## Control protocol

The version-1 grammar adds:

```text
maps
maps-read snapshot=<decimal> offset=<decimal>
```

`maps` returns an async `request=<id>`. Once its status is `done`, the same ID
is the immutable snapshot ID. `maps-read` returns:

```text
snapshot=<id> offset=<n> total=<bytes> crc32=<decimal> eof=0|1 data=<hex>
```

Each decoded chunk is at most the profile limit and each text response stays
under `SPZ_CONTROL_RESPONSE_MAX`. Reads require the exact snapshot ID and
offset; stale snapshots, offsets beyond the end, inconsistent metadata, odd
hex, oversized chunks, and CRC mismatches are rejected.

Status includes `maps_supported`, `maps_state`, `maps_snapshot`, and
`maps_size`. These fields are diagnostic and do not change the existing
profile-ready decision.

## Go and stackplz integration

`user/kpm.Client.SnapshotMaps(ctx)` submits `maps`, waits for the async request,
reads all chunks, validates stable metadata and CRC32, and returns the raw maps
bytes.

`event.LoadMapsContent(pid, content)` atomically replaces the existing maps
cache after confirming at least one valid mapping. `LoadMapsFile` delegates to
the same behavior.

During target preparation:

- proc targets retain the current procfs enrichment path;
- a KPM target with no explicit `--maps-file` requests one KPM snapshot before
  the preparation binding is cleared;
- successful content is injected into the maps cache and its file-backed
  directories are added to library search paths;
- a failed optional snapshot is logged and syscall/explicit-address tracing
  may continue;
- an operation that actually needs maps, such as unresolved `--brk-lib`, fails
  with the underlying maps error unless `--maps-file` or `--brk-base` was
  supplied.

The initial KPM snapshot is later augmented by the existing mmap2-event path.
No temporary unhide, procfs hook bypass, target memory modification, or maps
file injection into the target namespace is used.

## Failure isolation and resource limits

- Maximum snapshot size for the first profile is 2 MiB.
- Maximum decoded chunk size is 1536 bytes, leaving response headroom after
  hexadecimal encoding and metadata.
- Maps capability validation and snapshot failures never change
  `module->ready`, hook installation, debug-slot ownership, or event polling.
- `clear` remains the only normal session boundary. Hot unload is not part of
  this change; replacement of the currently loaded KPM uses a device reboot.
- Unsupported future layouts report `maps_supported=0`; they are adapted by
  adding a new exact profile rather than guessing offsets.

## Verification

Host tests cover profile generation, command parsing, task-reference lifetime,
snapshot publication/read bounds, Go chunk assembly/CRC validation, maps cache
replacement, and CLI fallback behavior. Sanitizer and race suites remain
mandatory.

On the authorized phone, acceptance requires:

1. Compare a KPM snapshot with `/proc/<pid>/maps` while the fixture is visible,
   allowing only mappings that legitimately change between sequential reads.
2. Hide the same fixture from procfs and prove normal `ps`/maps access fails.
3. Without `--maps-file` or `--brk-base`, prove KPM maps contains the fixture
   library and drives `--brk-lib` resolution.
4. Re-run syscall eBPF, uprobe plus user-stack symbolization, perf breakpoint,
   and KPM-direct breakpoint tests and require non-zero target events.
5. Run consecutive sessions and `clear`, verify no stale snapshot, task
   reference, binding, configured breakpoint, event-ring residue, kernel error,
   or device instability remains.
