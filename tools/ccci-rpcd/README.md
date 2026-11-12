<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# t2-ccci-rpcd

A small RPC server for the MediaTek MT6878 modem of the Titan 2, for mainline Linux. The kernel
answers most of the modem's RPC requests itself and passes four of them to user space over
`/dev/ccci_rpc`. `t2-ccci-rpcd` answers those four. The server was written from a
clean-room behavioural description of the protocol (facts only; no code of the factory
firmware was used); the protocol is documented in the source comments.

| op | name | what the server does |
|---|---|---|
| 0x4014 | AMMS_DRDI_CONTROL | INIT/COPY: copies pieces of the `md1drdi` section of the modem image (RF calibration data) into the 64 KiB DRDI window. The modem asserts at start-up without it |
| 0x400F | QUERY_AP_SYS_PROPERTY | looks the name up in the `-p` table; unknown names get the "unset" answer (0, "") |
| 0x4010 | SAR_TABLE_IDX_QUERY | always index 0 |
| 0x4015 | SAVE_MD_CAPID | logs the crystal CAPID/AAC values and replies success. **Nothing is written** |
| others | | rejected with −2 |

## Build

C11, libc only.

    make                    # host build: ./t2-ccci-rpcd
    make test               # ASan/UBSan build + host test (python3 -I)
    make CC="clang --target=aarch64-linux-musl --sysroot=/path/to/musl" LDFLAGS=-static

## Run

    t2-ccci-rpcd [-v] [-m IMAGE [-b]] [-p PROPS] [-w SIZE] [-L] -d /dev/ccci_rpc
    t2-ccci-rpcd [-v] [-m IMAGE [-b]] [-p PROPS] [-w SIZE] -R requests.bin -o replies.bin [-O window.bin]

| option | |
|---|---|
| `-m IMAGE` | the modem image container, opened read-only: on the phone the active slot's partition, e.g. `/dev/disk/by-partlabel/modem_a`; a copy of it in a file works too. `md1drdi` is found by walking the section headers |
| `-b` | `IMAGE` is the bare `md1drdi` data (a copy of the section, offset 0), not a container |
| `-p PROPS` | property table for 0x400F: `name=value` lines, `#` comments, whitespace around name and value dropped, the last line for a name wins, values at most 91 characters. A missing file is fatal; bad lines are logged and skipped |
| `-w SIZE` | size of the DRDI window to map (default 0x10000, the region size on MT6878). The kernel refuses a length larger than the region |
| `-d DEV` | serve the port. Opening is retried for about 8 s |
| `-R`, `-o` | offline replay: each fragment in the file is preceded by its length (u32, little-endian); the replies are written the same way. A zeroed buffer stands in for the window |
| `-O FILE` | with `-R`: write the window to `FILE` at the end |
| `-L` | do not take the `ccci_rpc` wake lock (`/sys/power/wake_lock`) while a request is handled. Without `CONFIG_PM_WAKELOCKS` or write access it is skipped anyway |
| `-v` | log each request and each copied set |

Without `-m`, or when `md1drdi` is not found or the window cannot be mapped, the server still runs,
but every 0x4014 INIT fails (as on the factory system) and the modem will not come up.

The server must be running before the modem sends its first 0x4014 INIT, shortly after the
runtime data exchange. The kernel keeps requests queued while the port is closed.

## DRDI copies

INIT (cmd 1) checks the protocol version (3) and the number of sets (≤ 15) and re-arms the copy.
COPY (cmd 2) validates all sets first and then reads each `{src, dst, len}` set from
`md1drdi + src` into `window + dst`. A COPY with byte 4 = 1 is final: later COPYs are answered
"ok" without reading until the next INIT. Every reply is `{i32 ret, 8-byte status}`; see `src/drdi.c` for the exact
status bytes.

The window is the uncached shared memory the modem reads; the server never clears it.

## Safety notes

- **The modem image is only ever opened `O_RDONLY`.** It must be a block device or a regular
  file. Each COPY reopens it and refuses to read if it is no longer the same device/inode as at
  start-up.
- **No partition is ever written.** The factory server writes 0x4015 CAPID values into the `para`
  partition; this one only logs them and replies success. Nothing else is written anywhere except
  the window and, in replay mode, the `-o`/`-O` files.
- Everything the modem sends is untrusted. Fragment length, channel and slot are checked; broken
  fragments are logged and dropped (a bad slot no longer stops the server, unlike the factory one).
  Each argument length is checked against the received bytes.
- Every COPY set must satisfy `len ≤ 0x10000`, `src + len ≤ md1drdi size` (without 32-bit
  wrap-around) and `dst + len ≤ window size`. A violation fails the whole request before anything
  is copied.
- The port is opened only after `fstat()` shows a character device.
- Run it as a dedicated user with read access to the modem partition and read/write access to
  `/dev/ccci_rpc` only.
- Several points in the protocol description are marked **unverified** (INIT body layout, the meaning of COPY
  byte 4, which status bytes the modem checks, which properties it asks for). Check them against
  a capture of real traffic; see the questions file.

## Choices where the protocol description leaves room

- Reply word 0 is the last request fragment's word 0 with only bit 31 cleared; bit 30 is passed
  back as the factory server does (question 8).
- Alignment filler in replies is zero (question 9).
- A request longer than one slot buffer (one full fragment) is answered with −2 once its last
  fragment arrives, instead of being dropped silently.
- A request whose argument stream is cut short is answered with −2.
- A missing argument 0 counts as a zero-length argument: 0x4014 → {−1, zero status}, 0x400F →
  unset, 0x4015 → {−1, previous status}.
- The INIT range check uses 32-bit wrapping arithmetic exactly as specified (INIT reads
  nothing). The COPY check additionally refuses a wrapping `src + len`.
- The container padding retry starts from the offset where the next header was expected.
- After a modem reset (ENODEV on write) partial requests are dropped; the DRDI state is kept, so
  the modem's next INIT re-arms the copy (question 3).
