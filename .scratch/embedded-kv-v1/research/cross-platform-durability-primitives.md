# Cross-platform durability primitives

Research date: 2026-08-08

## Decision summary

V1 should define durability through a narrow `durable_file` abstraction over local, seekable regular files. A commit writes recovery data and newly reachable database data with exact positioned I/O, executes a stable-storage barrier, publishes a self-validating commit record, and executes a second stable-storage barrier. The recovery format must tolerate short writes, torn multi-sector writes, reordered writes before a completed barrier, and an interrupted barrier; recovery selects only a complete authenticated/checksummed generation.

The platform barrier is:

- Windows: `FlushFileBuffers` on every data-bearing handle participating in the barrier. If writable memory mapping is ever used, `FlushViewOfFile` must precede `FlushFileBuffers`; Microsoft explicitly says the former neither flushes file metadata nor waits for the hardware cache ([FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers), [FlushViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-flushviewoffile)).
- Linux: `fsync` on every participating file. Linux documents that it flushes file data, associated metadata, and the disk cache and waits for device completion; a namespace change additionally requires `fsync` on the containing directory ([fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html)).
- macOS: `fcntl(fd, F_FULLFSYNC)` on every participating file. Apple's `fsync` documentation warns that `fsync` alone may leave data in a drive cache and recommends `F_FULLFSYNC` for databases needing write ordering; `F_FULLFSYNC` asks the drive to flush buffered data to permanent storage ([fsync(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fsync.2.html), [fcntl(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)).

A successful v1 commit may therefore promise survival of a later process or OS crash, and power loss when the local filesystem/device stack truthfully honors its documented flush operation. It cannot truthfully guarantee survival on hardware, firmware, hypervisors, remote filesystems, or removable media that acknowledge flushes without making data stable. Every barrier error is a commit failure with an indeterminate physical tail; the handle becomes recovery-required and no further writes are allowed until close/reopen and validation.

## Required portable abstraction

The storage layer should expose capabilities, not raw native calls:

```text
durable_file
  create_new(path) / open_existing(path)
  read_exact_at(offset, bytes)
  write_all_at(offset, bytes)
  resize(length)
  stable_storage_barrier()
  close()

mapped_region (optional optimization)
  map_read_only(...)
  flush_range(...)        // only if writable mappings are later admitted

namespace_ops (administrative, not a commit primitive)
  replace_same_volume(...)
  sync_parent_if_supported(...)

space_ops (optional optimization)
  preallocate(...)
  punch_hole(...)
```

`read_exact_at` and `write_all_at` loop until the requested range is transferred or an error/EOF is reported. POSIX `pread`/`pwrite` do not move the shared file offset and may successfully transfer fewer bytes than requested; Linux also violates the POSIX offset rule for `pwrite` on an `O_APPEND` descriptor, so database files must never be opened with `O_APPEND` ([Linux pread/pwrite(2)](https://man7.org/linux/man-pages/man2/pread.2.html), [POSIX pwrite](https://pubs.opengroup.org/onlinepubs/9699919799/functions/write.html)). Windows uses explicit `OVERLAPPED.Offset/OffsetHigh`; completion of `WriteFile` is not a persistence barrier, and Microsoft says only a single-sector write is atomic while multi-sector writes are not guaranteed atomic ([WriteFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile)).

The database should use buffered positioned I/O for its write path and explicit barriers. Unbuffered/direct I/O has platform- and device-alignment constraints and does not eliminate the need to flush metadata; Microsoft recommends physical-sector alignment for unbuffered I/O and separately documents metadata flushing ([Windows file buffering](https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering), [Windows file caching](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching)). Direct I/O may be added later as a measured optimization, not as a semantic requirement.

## Memory mapping

V1 should permit read-only mappings as a cache/access optimization but should not use writable mappings as its canonical commit path. Linux says `msync(MS_SYNC)` writes modified mapped pages and that without `msync` there is no guarantee changes are written before unmapping ([msync(2)](https://man7.org/linux/man-pages/man2/msync.2.html)). macOS likewise documents synchronous mapped-page writeback with `msync`, while its stronger device-cache guarantee remains `F_FULLFSYNC` ([macOS msync(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/msync.2.html), [macOS fcntl(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)). Windows explicitly requires `FlushViewOfFile` followed by `FlushFileBuffers` for dirty pages, metadata, and hardware-cache completion ([FlushViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-flushviewoffile)). A portable writable-map path thus needs both `flush_range` and the same file barrier; it offers no simpler durability contract.

## Creation, replacement, and directory durability

Creation should use exclusive-create semantics (`O_CREAT|O_EXCL` on POSIX and `CREATE_NEW` on Windows), fully initialize and barrier the file, then close it before reporting success. On Linux, `fsync(file)` does not necessarily persist the directory entry, so creation/rename must also `fsync` the parent directory ([fsync(2)](https://man7.org/linux/man-pages/man2/fsync.2.html)). The reviewed Apple and Microsoft documentation does not provide an equivalent universal parent-directory durability primitive with the same explicit guarantee. Consequently:

- normal database commits must never depend on file creation, deletion, rename, or replacement;
- the durable-commit promise begins for an already-open, successfully validated database file;
- creation and administrative replacement should use the strongest platform sequence available, then close and reopen/validate the result, but the specification must document the narrower namespace-crash guarantee where a platform does not expose one;
- sidecars may participate in recovery only while the database is open, but a clean close must checkpoint all committed state into the main file, barrier it, and only then remove dispensable sidecars.

POSIX/Linux `rename` atomically changes visible names on the same mounted filesystem, but atomic visibility is not the same as crash durability and cross-filesystem rename fails with `EXDEV` ([rename(2)](https://man7.org/linux/man-pages/man2/renameat2.2.html), [POSIX rename](https://pubs.opengroup.org/onlinepubs/9799919799/functions/rename.html)). Windows `MoveFileEx` may degrade cross-volume movement to copy-plus-delete; `MOVEFILE_WRITE_THROUGH` specifically guarantees flushing that copy operation. `ReplaceFile` has no supported write-through flag and documents intermediate failure outcomes involving both names. Neither should be the transaction commit point ([MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw), [ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)).

## Allocation and sparse files

Allocation is an optional capacity/performance hint, never part of correctness:

- Linux `fallocate` is filesystem-dependent, but successful ordinary allocation guarantees subsequent writes in that range do not fail for lack of disk space; hole punching and other modes are not universally supported ([fallocate(2)](https://man7.org/linux/man-pages/man2/fallocate.2.html)). `posix_fallocate` is the more portable POSIX-shaped operation.
- macOS exposes `F_PREALLOCATE`, including all-or-nothing and contiguous-allocation requests ([fcntl(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)).
- Windows sparse support must be detected and enabled explicitly; unwritten sparse ranges read as zeros and physical space is allocated as data is written ([Sparse Files](https://learn.microsoft.com/en-us/windows/win32/fileio/sparse-files), [FSCTL_SET_SPARSE](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_sparse)).

The engine must remain correct when preallocation or hole punching is unsupported. It must handle allocation/write/barrier failures, and it must not treat logical file length, a sparse hole, or successful preallocation as evidence that committed bytes are stable.

## Locking

Multi-process operation is outside v1, so locking is defensive detection rather than a correctness mechanism. The platform layer should attempt a process-lifetime exclusive open/lock and return `busy` when another cooperative opener is detected:

- Windows can deny conflicting access using `CreateFile` sharing rules and/or a byte-range lock. Windows byte-range locks cause conflicting handle I/O to fail, but are ignored by memory-mapped access ([Locking byte ranges](https://learn.microsoft.com/en-us/windows/win32/fileio/locking-and-unlocking-byte-ranges-in-files)).
- Linux/macOS may use `flock` or `fcntl`; these are advisory for local files, released with the owning open-file state/process, and remote filesystem semantics vary. Linux explicitly documents advisory behavior and divergent NFS/SMB semantics ([flock(2)](https://man7.org/linux/man-pages/man2/flock.2.html)); Apple documents advisory `fcntl` locking and recommends `flock` when last-close semantics matter ([fcntl(2)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/fcntl.2.html)).

The format and recovery algorithm must therefore remain safe under process crashes, but the library does not promise safety if unsupported simultaneous processes bypass or defeat the lock.

## Crash model the specification can test

The recovery design should assume interruption at every write and barrier boundary and must establish these observable outcomes:

1. If `commit()` returns success and the documented storage assumptions hold, reopening yields that commit or a later successful commit.
2. If interruption or I/O failure occurs before success is returned, reopening yields either the previous committed generation or the new generation, never a fabricated mixture; authentication/checksums and generation metadata decide validity.
3. A partial/torn commit record, reordered unbarriered data, stale sidecar, or extra physical tail is ignored or reported as corruption according to the recovery specification.
4. `close()` is not a durability primitive. It checkpoints, barriers, and only then disposes of recovery sidecars; failure leaves recovery material intact and reports an error.
5. No correctness argument relies on filesystem journaling. Linux's ext4 documentation, for example, says default journaling protects metadata consistency but does not guarantee file-data consistency after a crash ([ext4 journal](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html)).

## Consequences for later tickets

- The backend/recovery design needs at least two ordered stable-storage phases around publication of a self-validating root/commit record.
- The file format must include redundant generation metadata and integrity checks and must not assume atomic page-sized or multi-sector writes.
- Writable memory mapping, sparse layout, preallocation, and whole-file atomic replacement remain optional platform/backend optimizations.
- Supported-storage documentation must distinguish local regular files from remote/synchronized/virtual filesystems and state the truthful-device assumption.
- Fault injection must cover short reads/writes, ENOSPC, delayed writeback error, barrier failure, torn records, process kill, OS crash simulation, and clean-close sidecar removal ordering on every supported platform.
