<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# t2-ccci-fsd

A small file server for the MediaTek MT6878 modem of the Titan 2, for mainline Linux. The modem
has no flash access of its own: it reads and writes its NVRAM files (IMEI, calibration,
settings) through a file server on the AP, over the CCCI character device `/dev/ccci_fs`.
`t2-ccci-fsd` answers those requests from a sandbox directory and never touches a flash
partition. The server was written from a clean-room behavioural description of the protocol
(facts only; no code of the factory firmware was used); the frame layout, operations and
return codes are documented in the source comments (`src/fsd.h`, `src/ops.c`).

## Build

C11, libc only.

    make                    # host build: ./t2-ccci-fsd
    make test               # ASan/UBSan build + host test (python3 -I)
    make CC="clang --target=aarch64-linux-musl --sysroot=/path/to/musl" LDFLAGS=-static

## Run

    t2-ccci-fsd [-v] -r ROOT -d /dev/ccci_fs [-p]
    t2-ccci-fsd [-v] -r ROOT -R requests.bin -o replies.bin [-p]

`-d` serves the port. It must be running before the modem's second handshake. `-R`/`-o` replays
a capture offline: each fragment in the file is preceded by its length (u32, little-endian), and
the replies are written the same way. `-p` refuses every write to X: and Y:. `-v` logs each
request and copies the journal to stderr.

## Sandbox layout

    ROOT/ro/<dir>/...   read-only copy of the phone's files, opened only O_RDONLY
    ROOT/rw/<dir>/...   everything the modem creates or changes (made at start-up)
    ROOT/journal.log    copy-ups, creates, writes (offset, length, CRC-32), deletes,
                        renames, the decoded path of every compound request and
                        Move, refused and unimplemented requests

| drive | `<dir>` | writes |
|---|---|---|
| Z: | nvdata/md | yes |
| V: | nvdata/md_cmn | yes |
| X: | protect_f/md | yes, journaled as PROTECT; refused with `-p` |
| Y: | protect_s/md | same as X: |
| S: | nvcfg | yes |
| U: | mdlpm | yes |
| W: | firmware | no (−30) |
| R: | etc_md | no (−30) |
| T: | mdota (absent on this phone) | no (−30); every lookup −9 while absent |
| Q: | vendor_de_md (absent by default) | same as T: |
| others | — | −19 (never −4) |

A file in `rw/` hides the one in `ro/`. The first write to an `ro/` file copies it up to `rw/`.
Deleting an `ro/` file leaves a whiteout `.t2fsd.wh.<name>` in `rw/`. Names starting with
`.t2fsd.` are reserved and the modem cannot use them.

## Operations

| op | name | implementation |
|---|---|---|
| 0x1001 | Open (path, flags) | handle 1..128; create/truncate open read/write even with 0x100 |
| 0x1011 | Open variant (path, flags, 8 bytes) | handle or error, then the 8 request bytes echoed (zeros without them) |
| 0x1002 | Seek (handle, offset, whence) | new position; beyond the end allowed, negative −18 |
| 0x1003 | Read (handle, length) | ret, count, data; at most 0x40AC bytes |
| 0x1004 | Write (handle, data, length) | ret, count (−1 on error) |
| 0x1005 | Close (handle) | syncs a file opened with 0x10000, 0x20000 or 0x01000000 |
| 0x1006, 0x1017 | CloseAll, Shutdown | close every handle, no sync |
| 0x1009 | GetFileSize (handle) | ret, size (0 on error) |
| 0x1010 | GetAttributes (path) | 0x10 directory, 0x01 a file the modem cannot write; no archive bit |
| 0x1025 | GetFileDetail (path) | ret, 24 bytes: mtime, atime, ctime (u64 seconds; zeros on error) |
| 0x1012 | FindFirst (pattern path, u8 required attributes, u8 excluded attributes, u32 max name length) | handle (from the Open table) or −6/error, 0x34-byte record, name |
| 0x1013 | FindNext (handle, u32 max name length) | 0 or −6 (end; the handle stays open) or −10, record, name |
| 0x1014 | FindClose (handle) | closes a search or any other handle |
| 0x101A | GetDrive (u32 type, serial, alt mask) | 0x5A (`Z`) for type 0x04, otherwise −2 |
| 0x1007, 0x100B, 0x100F | CreateDir, Rename (files), Delete | |
| 0x100C | Move | a copy: the source is never removed, an existing destination is overwritten; 0x10000 first deletes the destination (a directory with everything below it) |
| 0x1022 | combined read | open, size, seek, read, close in one request; the handle is always closed; at most 0x40A0 bytes |
| 0x1024 | combined write | open (or a given handle), seek, write, close; journaled like Write |
| 0x1026 | combined read, extended | as 0x1022, may keep the handle open or use a given one; at most 0x4098 bytes |
| 0x1015, 0x1016, 0x1019, 0x101C | no-ops | 0, 1, 0, 0 |
| 0x101D–0x1021 | OTP, Restore | always −16 |
| 0x1023 | binary region access | −2 |
| everything else | RemoveDir, GetFolderSize, Count, GetDiskInfo, XDelete, GetClusterSize, unknown | −18, journaled with the argument lengths |

Each operation takes the arguments it uses by position; extra arguments are ignored and
journaled as `EXTRAARGS`. A missing argument, or an integer argument shorter than 4 bytes, is −2
in the operation's full reply shape and journaled as `BADPARAM`. Only the first 4 bytes of an
integer argument count. A handle that is not open, or a directory handle (except for Close), is −10.

Every operation reports an unknown drive letter as −19. Open, GetAttributes, GetFileDetail,
FindFirst, the compound operations and Move report a missing file or directory as −9 and a `..`
component as −16; other host errors are −18.

Find: the pattern is the last path component; `?` matches one character, `*` any run, everything
else only itself, case-sensitively (`*.*` needs a dot). FindFirst takes a snapshot of the merged
directory: the `rw/` names in host order, then the `ro/` names that `rw/` neither has nor hides with
a whiteout. Internal `.t2fsd.*` names, symlinks and special files are never listed; attributes,
size and times come from the visible copy. FindFirst skips `.` and `..`, FindNext does not. The
record's DOS date-times follow the factory server, quirks included: local time, raw seconds in
bits 0–4, the day of the month in the month bits, the access time in the creation slot. At most
4096 entries per directory are listed.
The compound operations reply with an 8-byte status (done mask, error) instead of a return code.
The handles they leave open are ordinary handles for Read, Write, Seek and Close.

## Safety notes

- Everything the modem sends is untrusted. Every length is checked, and broken fragments are
  logged and dropped.
- Paths must be ASCII DOS paths. The server refuses `.` and `..` components, empty components,
  `<>:"|?*`, control characters and the reserved names. Every lookup is an `openat2()` below
  the layer directory with `RESOLVE_BENEATH | RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS |
  RESOLVE_NO_MAGICLINKS`, so a symlink, a mount point or `..` cannot lead out of the sandbox.
  Only regular files and directories are served. Without `openat2()` (Linux < 5.6) the daemon
  refuses to start.
- OTP operations and Restore are always refused (−16).
- The port is opened only after `fstat()` shows a character device, so a block device is never
  opened. Nothing is ever copied back to the phone.
- Run it as a dedicated user that owns `ROOT` and `/dev/ccci_fs` and has no access to block
  devices. It warns when run as root.
- Several layouts in the protocol description are marked **unverified**. Check them against a capture of real
  traffic before using the daemon with a real modem.
