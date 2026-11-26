<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# t2-modemd

Modem control for the Titan 2 (MediaTek MT6878, integrated modem) on mainline Linux. One
daemon owns the modem's AT port (`/dev/wwan0at0`) and its MIPC data port (`/dev/wwan0mipc1`)
and offers radio, SIM, registration, mobile data, voice-call control and SMS to the rest of the
system over a UNIX socket. It is the control counterpart of `tools/ccci-fsd` (NVRAM file
server), `tools/ccci-rpcd` (RPC server) and `tools/speechd` (voice-call audio path): the
service `t2-modem` brings the modem to READY and powers the radio, `t2-modemd` talks to it.

**Not tested on the phone yet.** Everything here was built against observations of our own mainline driver,
clean-room behavioural descriptions of the MIPC, data and voice/IMS protocols
(facts only, no code of the factory firmware), 3GPP TS 27.007 / 27.005 / 23.040 / 23.038, and a fake modem in the
tests. Items not confirmed on the device are marked **(unverified)** are marked the same way below and collected in
[QUESTIONS.md](QUESTIONS.md).

| file | what |
|---|---|
| `src/at.c` | AT engine: command queue with per-command timeout, final result codes, intermediate lines, `> ` prompt for PDUs, URC dispatch by prefix, echo drop, line reassembly from partial reads |
| `src/modem.c` | state machine: port open/reopen, readiness (state file or `-a`), init sequence, radio/SIM/registration/signal tracking, modem loss |
| `src/mipc.c`, `src/mipc.h` | MIPC transport: framing, TLVs, request/confirm by transaction id, indications, timeouts, session start (TEST → OPEN → REGISTER_IND); every id/offset a named constant with a comment |
| `src/data.c` | mobile data: the MIPC backend (DATA_ACT_CALL) and the AT backend (templates or plain 3GPP), then the shared WWAN link / address / route / DNS, teardown; interface indications we did not request go to ims.c, `data_pdn_link/down` give them a link without route/DNS |
| `src/ims.c` | IMS/VoLTE: start-up sequence, the PDN the modem raises itself (EIF_IND + IMS_PDN_IND → link), INTERNAL_EIPSPI_CMD → RSP (SPI), AT notify fallback, registration services, `ims on\|off\|status` |
| `src/call.c` | voice calls: ATD/ATA/+CHLD/+VTS, the +ECPI/+EAIC/+ESPEECH call model, +CLCC reconciliation, speech trigger to t2-speechd |
| `src/sms.c` | SMS PDU mode: +CMT receive with +CNMA, GSM 7-bit (with the extension table) and UCS-2, concatenation, +CMGS send with multipart, +CMGL list |
| `src/ctl.c` | control socket `/run/t2-modemd.sock` (0660), line protocol, `monitor` event stream, client mode |
| `src/vendor.c` | every MediaTek-specific command and URC format in one place (EFUN, ESUO, ECPI, EAIC, ESPEECH, EIMSPDN/EIMSCFG/EIMS, the data-bind hook, the raw-AT deny list) |
| `src/main.c` | options, `/etc/t2-modemd.conf`, logging, the poll() loop |
| `tests/test_modemd.py` | host test: fake modem on a pty, fake MIPC peer on a pty, `ip` stub, t2-speechd stub |
| `t2-modemd.initd`, `t2-modemd.conf` | OpenRC service and the configuration example; installer `tools/netroot/install.d/t2-modemd.sh` |

## Build

C11, libc only, single process, poll() loop, no threads.

    make                    # host build: ./t2-modemd
    make test               # ASan/UBSan build, built-in self-tests (-T), python3 -I tests/test_modemd.py

For the phone (Alpine aarch64, musl, static), as `tools/ccci-rpcd` does; the result is left as
`t2-modemd-aarch64` so the host binary is not overwritten:

    SR=$REPO/out/musl-aarch64/sysroot
    PATH=${CLANG_DIR:-$REPO/toolchains/clang-r574158}/bin:$PATH \
    make BIN=t2-modemd-aarch64 \
         CC="clang --target=aarch64-alpine-linux-musl --sysroot=$SR -fuse-ld=lld --gcc-toolchain=$SR/usr" \
         LDFLAGS=-static

Install: binary to `/usr/local/bin/t2-modemd`, `t2-modemd.initd` to `/etc/init.d/t2-modemd`,
`t2-modemd.conf` to `/etc/t2-modemd.conf` (the netroot installer does this). Needs `iproute2`
(`/sbin/ip`) for the data links.

## Run

    t2-modemd [options] -d /dev/wwan0at0           # daemon
    t2-modemd [-s SOCKET] COMMAND [ARGS]            # client: one command, prints the reply
    t2-modemd -T                                    # self-tests of the MIPC framing and the SMS codec

| option | |
|---|---|
| `-d DEV` | the AT port. Opening is retried every `reopen_ms` (the port appears after the modem is READY) |
| `-c FILE` | configuration (`/etc/t2-modemd.conf`, key = value). A missing default file is fine; a named one must exist and parse |
| `-o KEY=VALUE` | one configuration value, after `-c` |
| `-S FILE` | modem state file (`/sys/kernel/debug/d180000.mddriver/fsm`, first line `state: ready`); `none` = no state source |
| `-a` | with no state source: ready as soon as the port is open |
| `-s SOCK` | control socket (`/run/t2-modemd.sock`) |
| `-v` | log every AT line, MIPC message, event and `ip` call |

The daemon starts the MIPC session and the init sequence only when the modem is ready (state
file). Readiness is re-checked every `ready_poll_ms`; a modem that stops being ready, an
EIO/ENODEV/EOF on a port, or a write error means **modem lost**: calls are cleared (t2-speechd
`call-stop`), data links and routes are torn down, every queued command fails, the ports are
closed and reopened, and the init sequence runs again when the state file says ready.

### Init sequence

`ATE0`, `AT+CMEE=2`, `ATI`, `AT+CGMR`; stack select (`AT+ESUO=5` when `sim=2`); `AT+EFUN=<mask>`
only when `radio_mask` is set (the `t2-modem` service already does it); `AT+CREG=2`, `AT+CEREG=2`,
`AT+CGREG=2`, `AT+CIREG=2`, `AT+COPS=3,0`, `AT+CLIP=1`, `AT+CCWA=1`, `AT+CRC=1`, `AT+CGEREP=2,1`,
`AT+CGPIAF=1,0,0,0`, `AT+CMGF=0`, `AT+CNMI=2,2,0,1,0` (fallback `1,2,0,0,0`); the per-stack voice
init (`voice_init_cmds`: `AT+CMOD=0`, `ATS0=0`, `AT+CCWA=1`, `AT+CLIP=1`,
`AT+CSSN=1,1`, `AT+CMUT=0`, `AT+ECPI=4294967295`, `AT+EAIC=2`, `AT+EVOCD=1`), `init_cmds`; then
`AT+CPIN?`, `AT+CFUN?`, `AT+CREG?`, `AT+CEREG?`, `AT+CGREG?`, `AT+CIREG?`, `AT+COPS?`, `AT+CSQ`.
Then the start-up of "IMS / VoLTE" (IMS configuration, APN profiles, then the radio). A
`+CME ERROR: SIM busy` (CME 14) on `AT+CPIN?` right after a restart is not a SIM state: the SIM is
read again after 1 s.
Refused optional commands are logged, not fatal; a modem that does not answer at all is retried
every 2 s. `AT+CSQ` is polled every `signal_poll_ms`.

## Commands

Each reply ends with a line `ok` (exit code 0) or `error: …` (exit code 1).

| command | what |
|---|---|
| `status` | modem/port state, model, radio, stack, SIM, registration (`reg ims: registered (voice,sms, ext 0x5)`), the `ims status` lines, signal, data, MIPC session, calls, SMS and AT counters |
| `net` | re-reads CREG/CEREG/CGREG/CIREG/COPS/CSQ and prints operator, RAT, registration (CS/EPS/GPRS/IMS), signal |
| `scan` | `AT+COPS=?` (up to `scan_timeout_ms`), one line per network |
| `radio on [mask]` / `radio off` | on: `AT+CFUN=1` for mask 1, `AT+EFUN=<mask>` for 2/3 (default: `radio_mask` or 1); off: `AT+CFUN=4` |
| `sim 1\|2` | selects the protocol stack on the port (`AT+ESUO=4`/`5`), re-runs the voice init, re-reads SIM and registration. Refused while data is up or a call exists. **Limitation**: one stack at a time on this port; URCs of the other stack are **(unverified)** |
| `pin <PIN>` | `AT+CPIN="…"`, only when the SIM asks for a PIN (never where a PUK is asked); the PIN is not logged |
| `data up [apn] [cid]` | see below |
| `data down` | routes and link removed, DNS file removed, context deactivated (if we activated it) |
| `data status` | aid/APN/link/transIntfId/address/DNS/bind status and the link's rx/tx counters (the DL/UL path) |
| `ims on` | IMS support on: runs the start-up sequence now (modem ready and radio on) or as soon as it is; reply = each command and its answer, `ims: on, setup done (…)` |
| `ims off` | `AT+EIMSPDN= "onoff",0`, the IMS links removed (nothing sent over MIPC), `AT+EIMS=0` only if we sent `AT+EIMS=1`; internet data is not touched |
| `apn profiles` | the `APN_ADD_PROFILE_LIST_REQ` (internet/mms/ims/sos) again over MIPC; error when the session is not ready or the CNF result is not 0 |
| `ims config` | the `IMS_SET_CONFIG_REQ` feature-switch form (the `ims_cfg` flags) again over MIPC; error when the session is not ready or the CNF result is not 0 |
| `ims status` | on/off, setup result, `+EIMS`/`+EIMCFLAG` state, each IMS PDN (type, aid, link, address, transIntfId, MTU), counters (EIF, IND, SPI, AT notify, reject, oos), last reject |
| `call dial <number> [--clir\|--no-clir] [--emergency]` | `ATD<number>;` with `#31#` / `*31#` CLIR prefixes, `ATDE<number>;` for emergency; number ≤ 40 characters |
| `call answer` | `ATA` |
| `call hangup [id]` | `AT+CHLD=1<id>`; without id and several calls `AT+CHLD=6` (then `ATH`, then `AT+CHUP` on error) |
| `call hold` / `resume` / `swap` | `AT+CHLD=2` |
| `call dtmf <digits>` | one `AT+VTS=<d>` per digit |
| `call list` | `AT+CLCC` reconciled into the model, then the list |
| `call device earpiece\|speaker`, `call volume 0..6`, `call mute on\|off` | passed through to t2-speechd |
| `sms send <number> <text>` | PDU mode; 7-bit when the text fits the GSM alphabet (extension table included), else UCS-2; split with a concatenation header (up to 8 parts) |
| `sms list` | `AT+CMGL=4`, every stored PDU decoded |
| `monitor` | keeps the connection; a snapshot (`modem`, `sim`, `reg …`, `signal`, `mipc`, `ims on\|off`, `ims pdn up …`, `data`, `call`, `audio` lines in the event formats), then the event stream |
| `at <command>` | raw AT passthrough for bring-up: logged at normal level, refused for the deny list, `allow_raw_at=0` disables it |
| `help` | the command list |

### Events (`monitor`)

`modem ready` / `modem lost <why>`; `sim ready|pin|puk|pin2|puk2|locked|other <code>`;
`reg cs|eps|gprs <stat> [<rat>]`, `reg ims registered|not-registered`, `reg operator <name> <rat>`;
`signal <dBm> dBm rssi=<n> ber=<n>` / `signal unknown`; `mipc open|ready|lost <why>`,
`mipc ind 0x<id> len <n>`, `urc-mipc <line>` (AT URC via the MIPC tunnel); `data up aid=<a>
link=<if> addr=<a>`, `data down <why>`, `data pdn <text>` (+CGEV), `data if <tid> ifup|ifdown|…
<v4> <v6>`; `call <id> dialing|alerting|active|held|incoming|waiting <number>`, `call <id> ended
<number> [cause <n>]`, `call ring <number>`, `call <id> rejected <number>`, `call <id> ringback
in-band`, `call speech 0|1`, `call codec <n>`, `call result NO CARRIER|BUSY|…`, `audio on|off
(<why>)`, `audio error call-start|call-stop`; `sms <from> <text>` (newlines as `\n`),
`sms-part <from> <i>/<n>`, `sms-bad <hexlen>`, `sms-report …`; `ims on|off`, `ims setup ok`,
`ims setup failed <why>`, `ims services <voice,text,sms,video|none> ext=0x<hex>`, `ims pdn init`,
`ims pdn up|change link=<if> transIntfId=<t> addr=<a>`, `ims pdn failed transIntfId=<t>`,
`ims pdn ind aid=<a> action=<0|1> type=ims|<n> iface=<id>`, `ims pdn down link=<if> aid=<a>
type=<t> <why>`, `ims spi index=<i> spi=0x<hex>` / `ims spi index=<i> refused`, `ims pdn notify
aid=<a> action=<n> type=<t>` + `ims pdn confirm aid=<a> result=0` (AT fallback), `ims pdn reject
cause=<c> type=<t>`, `ims pdn oos <fields>`, `ims <line>` (`+EIMS`, `+EIMCFLAG`, `+EIMSPDIS` and
other IMS URCs, forwarded); `urc <line>` (anything unknown).

## Mobile data

Two backends (`data_backend`), the rest is shared.

**mipc** (default; the path the factory software uses): after the modem is
ready the MIPC port is opened and the session started: `INTERNAL_TEST_REQ` (0x0305, slot 0xFF)
every 100 ms until its CNF, `INTERNAL_OPEN_REQ` (0x0301: version 2, client name, flag 1),
`INTERNAL_REGISTER_IND_REQ` (0x0307) for 0x4302, 0x4201, 0x4202, 0x4001 and 0x4A03 on the
selected slot, `INTERNAL_REGISTER_CMD_REQ` (0x030B, slot 0xFF) for 0x8305 (`mipc ready`). `data up` sends `DATA_ACT_CALL_REQ` (0x0201, slot bit of the stack) with the
TLV set of the captured request (APN 0x0101, type 0x0102 = 1, PDP type 0x0103/0x0104, auth 0x0105 = 0, empty
user/password 0x8106/0x8107, 0x0108 = 0x7F, bearer mask 0x0109 = 0x7FFFFFFF, 0x010A = 0,
0x010F = 0; the 672-byte traffic descriptor 0x010D with `mipc_traffic_descriptor=1`), waits for
the CNF (result 0x0000, aid 0x0100, IPv4 0x0103/0x8104, DNS 0x010D/0x810E and 0x0112/0x8113, MTU
0x0123, transIntfId 0x012A) and uses the address of the `INTERNAL_EIF_IND` (0x4302, cmd 0 = up,
records 0x8105) that arrives before it when the CNF has none. **Link id = transIntfId mod 100.**
Nothing is confirmed back to the modem. `DATA_DEACT_CALL_IND` (0x4202) for our aid and
`INTERNAL_EIF_IND` cmd 1 (down) for our interface take the link down; `data down` sends
`DATA_DEACT_CALL_REQ` (0x0203: aid 0x0101, cause 0x0102 = 1). Framing/ids are from the protocol description and
one captured activation; whether the request alone starts the user plane, whether REGISTER_IND is
required, the masked CNF TLVs and the IPv6 TLVs are **(unverified)** (QUESTIONS A1–A6). With `-v`
every CNF TLV id is logged for the questions file.

**at** (fallback; **nothing of it has been tried on this phone**): the templates
of `data_bind_cmds` run in order with `{apn}`, `{aid}`, `{tid}`, `{linkid}` substituted; the aid is
captured from `+CGEV: ME PDN ACT <aid>` (as a response line or a URC), the transIntfId from
`+EPDN: <aid>,"new",<rat>,<tid>`, addresses from `+EPDN "addr"`/`"update"`, `+CGCONTRDP`,
`+CGPADDR` or the `+EIF: <tid>,ifup,…` URC; `@linkup` marks where the link is created, the
templates after it (`AT+EIF=<tid>, "ifst", "up", 1`, advisory) run afterwards. Default:
`AT+EAPNACT=1,"{apn}","default",0; AT+EPDN={aid},"ifst",20; AT+CGCONTRDP={aid}; AT+EPDN={aid},"addr";
@linkup; AT+EIF={tid}, "ifst", "up", 1`; deactivation `AT+EAPNACT=0,{aid},1` (reason 1 = normal
per the protocol description). With an **empty** `data_bind_cmds` the plain 3GPP route is used: the lowest
context of `AT+CGDCONT?` (the LTE default bearer, cid 0 on this modem) or the given cid/APN,
`AT+CGACT?`/`AT+CGCONTRDP=<cid>` to see whether it is active, `AT+CGACT=1,<cid>` if not,
`AT+CGPADDR=<cid>`; a context we did not activate is never deactivated. The code hook for a
vendor bind command is `vendor_data_bind()` in `src/vendor.c` (empty).

**Shared**: `ip link add dev <wwan_dev>-<N> parentdev <wwan_dev> type wwan linkid <N>` (skipped
when the link exists), `ip addr flush`, `ip addr add <v4>/32`, `ip -6 addr add <v6>/64` (if a
global IPv6 address is known), `ip link set … mtu <mtu>` (modem MTU, else `data_mtu`, 0 = leave),
`ip link set … up`, `ip route replace default dev … metric <route_metric>` (and `-6`). `ip` is
executed with fork/exec and argv arrays (no shell), path `ip_cmd`. DNS goes to `resolv_file`
(`/run/t2-modemd/resolv.conf`), and to `/etc/resolv.conf` only with `manage_resolv=1` (a copy via
rename; it is not restored on down). `data up` reports the backend, contexts, aid/tid, address,
gateway, DNS, link, metric, the UL/DL path and the bind status; `data status` adds the link's
rx/tx counters. Without an APN argument `data_apn` is used, else the attach context's APN read
from `AT+CGDCONT?`.

## Voice calls

The call model follows the MediaTek call-progress reports: `+ECPI: <id>,<type>,…` is the primary source (130
created → dialing, 0 setup → incoming/waiting, 2 alerting (`is_ibt` = in-band ring-back event),
6 connected → active, 133 disconnected with cause → ended, 7 all released; 131/132 held/active
**(unverified which)** only re-poll), `+EAIC: <id>,"<num>",<type>,<mode>,<seq>` is answered at
once with `AT+EAIC=0,<id>,<seq>` (`eaic_auto_accept=1`, the call then rings) or
`AT+EAIC=1,<id>,<seq>,21` (reject), `RING`/`+CRING`/`+CLIP`/`+CCWA` fill in the number,
`+ESPEECH: <on>,…` (first field only) reports the modem's speech path, `+EVOCD` the codec,
`+EVADSREP`/`+CIREPH` are forwarded. `AT+CLCC` is polled after every event, every `call_poll_ms`
while calls exist and a few seconds after, and reconciles the model (calls appearing or
disappearing there are reported too). Call-mode 20..25 marks IMS calls (same commands).

**Speech trigger** (both `speech_on` policies are implemented because A1–A3 of the
voice questions are open): `dial` (default) sends t2-speechd `call-start --rate <call_rate>` right
after `ATD`/`ATDE` is answered `OK` and after `ATA` (factory timing); `espeech` sends it only on
`+ESPEECH: 1` (modem-driven; whether in-band ring-back is then audible is **(unverified)**).
`call-stop` goes out when the last call is gone: `+ECPI …,133` for the last id, `+ECPI …,7`,
`+ESPEECH: 0`, a hang-up answered `OK` with an empty `+CLCC`, modem loss, or SIGTERM. An empty
`+CLCC` right after a dial does not stop the audio (the modem may list the call only after
`+ECPI …,130`): 5 s grace, and only tracked calls that disappear count. `device`/`volume`/`mute`
are passed through to t2-speechd. t2-speechd protocol: `tools/speechd/README.md`.

## IMS / VoLTE

The IMS stack (SIP, registration, media) runs in the modem, and on this phone the modem also
**raises the IMS PDN by itself** (the boot timeline of the factory firmware). The daemon configures IMS, gives the PDN its Linux link and answers the
modem's IPsec SPI requests. It never activates a PDN for IMS and confirms nothing over MIPC.
VoLTE calls use the same commands and call model as CS calls (call-mode 20..25 in
`+ECPI`/`+EAIC`); the speech trigger is identical.

**Start-up** runs on every (re)connected modem once it is ready and (with `data_backend=mipc`)
the MIPC session is ready; after 20 s without a session it runs without the MIPC steps. It runs
**before the radio goes on**: the modem keeps interface 101 for IMS and raises the IMS PDN only
when the IMS configuration and an APN profile of type IMS are in place before the radio is on
("Found on mainline 2026-10-30"). Order:
1. With `ims=1`: MIPC `IMS_SET_CONFIG_REQ` 0x0A00, key/value form, on the stack's slot:
   `0x0100` u32 1, `0x0101` `"mtk_ct_volte_support"` + NUL (21 bytes), `0x0102` u8 0x32 (meaning
   unverified). The daemon waits for CNF 0x0A01 (result `0x0000`). Off with `ims_mipc_key=0`.
2. With `ims=1`: `IMS_SET_CONFIG_REQ`, feature-switch form: `0x0100` u32 0, `0x0102` 6 bytes from
   `ims_cfg` (default `1,0,0,0,1,1`: VoLTE, SMS over IMS, emergency IMS, the SIM1 values).
   `ims config` sends this form again on demand; factory re-sent it 0.2 s before the IMS PDN.
3. With `apn_profiles=1` (default, also with `ims=0`): `APN_ADD_PROFILE_LIST_REQ` 0x0113 on the
   stack's slot, then wait for CNF 0x0114 with result 0. The payload is:
   - `0x0100` u8 = the count;
   - the array descriptor `0x8101` (len 0, body `u32 0, u32 0, u16 first, u16 count`);
   - one element per profile, with ids counting down from 0xFFFF (0xFFFD..0xFFFF for
     three, 0xFFFC..0xFFFF for four). Each element is a 284-byte record (padded to 288):
     id 0xFFFFFFFF, APN char[100] at offset 12, bearer mask 0x7FFDFFFF at 244, type mask at
     248, PDP/roaming PDP at 252/253, auth at 254, enabled 1 at 256, the rest 0.

   The profiles:

   | profile | APN | type | PDP | auth | included |
   |---|---|---|---|---|---|
   | internet | `data_apn`, else the attach context of `AT+CGDCONT?` | 0x29 | `data_ip_type` (IP = 1) | 1 | if an APN is known |
   | mms | `mms_apn` | 0x04 | 1 | 1 | only if `mms_apn` is set |
   | ims | `ims_apn` (`ims`) | 0x02 | 3 | 0 | unless `ims_apn` is empty |
   | sos | `sos_apn` (`sos`) | 0x100 | 3 | 0 | unless `sos_apn` is empty |

   `apn profiles` re-sends the list on demand. If the internet profile had to be left out (no
   `data_apn`, and no attach context yet because the radio is still off), the list goes out
   without it. It is then re-sent every 5 s, once registered, until `AT+CGDCONT?` gives the
   attach APN; this stops after 60 tries. Event: `apn profiles sent with internet`.
   Setting `data_apn` avoids the second list.
4. With `ims=1`: `AT+CIREG=2`; `AT+EIMSPDN= "onoff",1` (the form the factory firmware's RIL sent; the AT port
   accepts it; the plain form only after a refusal); `AT+EIMSCFG=<ims_cfg>` on the AT port
   (harmless, kept); `AT+EIMSRCS=0,0` through the MIPC AT tunnel (`SYS_AT_REQ` 0x0003, TLV
   `0x8100`; CNF 0x0004 with result and text; success = result 0 and no `ERROR`
   **(unverified)**; the plain AT port does not know the command); `AT+EIMS?`, and `AT+EIMS=1`
   only when it answers `+EIMS: 0`.
5. **Radio** (`radio_on_at_start=1`, default): once per connection, the radio goes on as
   `radio on` does: `AT+EFUN=<radio_mask>`, or `AT+CFUN=1` without a mask. It is skipped when
   the radio is already on and no mask is set. It is also skipped when `radio off` was given in
   this daemon run. Without this step, a modem restarted by the kernel stays at `+CFUN: 4` and
   never registers. With `radio_on_at_start=1`, `radio_mask` is no longer sent during init; it
   is sent here, after the set-up. With `radio_off_before_setup=1` (default 0), a radio that is
   already on before the set-up (the first boot, when the t2-modem service powered it) is
   switched off first (`AT+CFUN=4`).

The start-up runs again after modem loss, after a new MIPC session (steps 1–4), after
`sim 1|2` and after `ims on` (steps 1–4; the radio is already on then).
A refused config request (non-zero CNF result or no CNF), `AT+EIMSCFG`, tunnel command or
`AT+EIMS=1` is noted in the setup status. If
both onoff forms are refused, the setup fails; if the modem does not answer, it is retried
after 5 s. The MIPC session also registers `IMS_PDN_IND` 0x4A03 (per slot) and
`INTERNAL_EIPSPI_CMD` 0x8305 (`INTERNAL_REGISTER_CMD_REQ` 0x030B, slot 0xFF).

**The IMS PDN**: nothing in this flow is sent to the modem except the 0x8306 answers.
- **Up.** An `INTERNAL_EIF_IND` (0x4302) with command 0 for a transIntfId the daemon did not
  request is the IMS (or emergency) PDN coming up. The daemon creates
  `wwan0-<transIntfId mod 100>` (factory: 101 → `wwan0-1`) and sets on it:
  - the address of the indication's IPv4 record (`/32`; IPv6 `/64` if a record is given),
  - the MTU of the indication, else `data_mtu`,
  - `up`.

  The IPv4 record `0x8105` is read as a plain TLV or as an array element (descriptor plus
  element). On mainline the indication had no usable address, and the address came only
  in the URC `+EIF: <tid>, ifup, <type>, <ipv4>, <x>` (layout observed once), on the AT port and
  as the tunnel's `SYS_AT_IND` text. That URC's address is applied to the IMS link, either at
  once or when the EIF_IND comes later. With `-v` every EIF_IND TLV id is logged.

  No default route and no DNS file are set up: the IMS traffic is the modem's own.
- **Repeated indication.** Another up (or command 4/5) for the same interface configures the
  link again.
- **Telling its own interface apart.** While `data up` waits for its `DATA_ACT_CALL_CNF`, every
  EIF_IND is held. After the confirm, the one with the confirm's transIntfId is the internet
  PDN; any other one goes to IMS.
- **Labelling.** `IMS_PDN_IND` 0x4A03 has two forms:
  - The full form (aid, action, type 2 = ims, interface id) labels the PDN with its aid and
    type (shown in `ims status`). If it comes before its EIF_IND, it is remembered and the link
    comes with the EIF_IND.
  - The short form (`0x0100` = 1) is the bookkeeping reset (= `+EIMSPDN: "init"`). The labels
    are dropped; the links stay until their EIF_IND down **(unverified)**.
- **Down.** `IMS_PDN_IND` action 0 for the PDN, or `INTERNAL_EIF_IND` command 1 for its
  interface, removes the link.
- **IPsec SPI.** `INTERNAL_EIPSPI_CMD` 0x8305 (header byte 8 = 0x11, its own txid) is answered
  with `INTERNAL_EIPSPI_RSP` 0x8306, with the same txid and header byte:
  - `0x0100` u32: the request index, echoed;
  - `0x0101` u8: 0;
  - `0x8102` u32: an SPI chosen uniformly at random in [`0x0106`, `0x0107`] of the request
    (0x100..0x0fffffff when absent), never one handed out earlier in this MIPC session.

  The table holds the last 256 SPIs. A range with nothing left is answered with result 1
  **(unverified value)**. The request's address records and protocol are logged with `-v`.

**AT fallback** **(unverified: never observed)**. `+EIMSPDN: "notify",<aid>,<0|1>,…` on the AT
port is answered with `AT+EIMSPDN="confirm",<aid>,0`, and nothing else is done (no
activation). `+EIMSPDN: "init"` is reported. `+EIMSPDN: "reject",<cause>,<type>` and
`+EIMSPDNOOS: …` are events (`ims status` shows the last reject). `+EIMSPDIS` is only
forwarded. `+EIMSPDN:` / `+EIMS:` lines that arrive inside the answer of their own command are
handled as URCs.

**Registration**: `+CIREGU: <reg_info>,<ext_info>` and the `AT+CIREG?` answer set
`reg ims registered|not-registered`. On mainline no `+CIREGU` reached the AT port, so while
IMS is on the daemon polls `AT+CIREG?`:
- every `ims_reg_poll_ms` (5 s) after an IMS PDN came up, until registered;
- every `ims_reg_idle_poll_ms` (60 s) otherwise;
- also with `net` and at init. The hex `<ext_info>` is decoded per 27.007 §8.71 (1 voice,
2 text, 4 SMS, 8 video) into `ims services …` and the `reg ims:` status line.

**Coexistence with internet data**:
- The IMS PDNs have their own state (transIntfId, link, aid/type label). `data up/down` never
  touches them, and `ims on/off` never touches internet.
- An unrequested interface whose linkid equals the internet link is refused.
- `ims off` sends `AT+EIMSPDN= "onoff",0`, removes our IMS links (nothing over MIPC), and sends
  `AT+EIMS=0` only if we sent `AT+EIMS=1`. While IMS is off, unrequested interfaces get no link.
- Modem loss, loss of the MIPC session, SIGTERM (unless `keep_data_on_exit=1`) and `sim 1|2`
  remove the IMS links. The SPI table is reset with the MIPC session.
- `ims_cfg` is the same for both stacks; factory used `0,0,0,0,1,1` on SIM2.

## SMS

PDU mode (`AT+CMGF=0`). Incoming `+CMT: ,<len>` + PDU: acknowledged with `sms_ack_cmd`
(`AT+CNMA`, submitted before decoding), SMS-DELIVER decoded (SMSC skipped, originating address
incl. alphanumeric, DCS → GSM 7-bit / 8-bit / UCS-2 (surrogates handled), UDH with concatenation
IE 0x00/0x08), parts of a multipart message are held per (sender, ref, total) in 8 slots and
delivered when complete (`sms-part` events meanwhile; incomplete after 10 minutes → delivered with
`[missing part]`). `+CMTI` → `AT+CMGR=<idx>` (deleted only with `sms_delete_read=1`). Sending
builds SMS-SUBMIT PDUs (SMSC 00, no validity period, DA international when the number starts
with `+`), 160/153 septets or 70/67 UCS-2 characters per part, escape sequences never split,
`AT+CMGS=<len>` → `> ` → PDU + Ctrl-Z, `+CMGS: <mr>` reported per part. Malformed PDUs are
dropped with an `sms-bad` event. The codec round-trips are in the `-T` self-test.

## Robustness and safety

- Every line from the modem and every MIPC byte is untrusted: bounded buffers everywhere (lines
  > 1023 bytes dropped, unterminated input > 8 KiB dropped, control characters replaced), TLV
  lengths checked against the payload, frames with a bad magic or an impossible length dropped,
  PDUs validated field by field. The sanitizer build runs the whole test suite.
- Command timeouts: `cmd_timeout_ms` (5 s), `call_timeout_ms` (30 s, ATD/ATA/raw), `sms_timeout_ms`,
  `scan_timeout_ms`, `mipc_timeout_ms`. After a timeout the next command waits `settle_ms` and
  stray late final results are logged.
- SIGTERM: calls hung up and `call-stop` sent, data taken down (unless `keep_data_on_exit=1`),
  ports and socket closed. Modem loss: see above.
- The daemon never sends commands that change IMEI, NV/calibration, SIM or carrier lock state or
  factory modes. The raw `at` passthrough is logged at normal level and refuses the deny list
  (`raw_at_deny`, 3GPP lock/PIN/SIM-access commands: `+CLCK`, `+CPWD`, `+CSIM`, `+CRSM`, `+CPIN`,
  `+CCHO`, `+CCHC`, `+CGLA`, `+CRLA`, `+CFUN=1,1`); vendor NV/IMEI write commands are unknown on the
  clean side and should be added when named (QUESTIONS C18). `allow_raw_at=0` disables it.
- `pin` refuses to send a PIN when the SIM asks for a PUK or is ready.
- The daemon writes only to the two WWAN ports, its socket and files in `/run/t2-modemd`,
  `/etc/resolv.conf` when asked, and the network configuration through `ip`.
- A context the modem activated on its own (the LTE default bearer) is never deactivated by the
  3GPP route; the MIPC/AT activation paths deactivate only what they activated.

## Choices where the facts leave room

- Readiness: the state file, else `-a`; there is no AT-side readiness check beyond the first
  command answering.
- `NO CARRIER`/`BUSY`/`NO ANSWER`/`NO DIALTONE` are final results only while `ATD`/`ATA` is
  pending, URCs (call events) otherwise. `+CREG:`-style lines answer a pending `AT+CREG?` and are
  URCs otherwise.
- Radio off = `AT+CFUN=4` (observed start-up state); `AT+EFUN=0` is untried (QUESTIONS C17).
- Hang-up: `AT+CHLD=1<id>` for one call (also when only one exists), `AT+CHLD=6` for all.
- `+CGEV … DEACT` for our aid, `NW/ME DETACH`, `+EIF ifdown`, `DATA_DEACT_CALL_IND` and
  `INTERNAL_EIF_IND` down take the link down; `ifchg`/`ipchg` are reported, not applied.
- IPv6: a global address from the modem is set with /64 and a default route; link-local or
  interface-id-only values are ignored (RA inside the PDN is the kernel's business).
- The MIPC txid starts at 0 and counts up per request; 0xFFFF is skipped.
- `scan` parses `+COPS: (…),(…)` leniently and stops at the trailing format lists.

## Tests

`make test` builds with ASan/UBSan, runs the built-in self-tests (`-T`: MIPC framing/TLV against
the protocol description's worked example incl. array descriptors and the privacy bit; SMS codec round trips for
7-bit, extension characters, UCS-2, surrogate pairs, multipart) and `tests/test_modemd.py`
(python3 -I, standard library only): the fake modem on a pty scripts responses and URCs
(fragmented reads, URC inside a response, echo, timeouts, CME/ERROR, garbage, oversized lines),
init and events, radio/SIM/PIN/stack switching/scan, data on both backends (`ip` argv recorded,
resolv.conf, teardown by command/URC/indication/loss; the MIPC peer checks the session start,
every request TLV, fragmented frames, shuffled CNF TLVs with array descriptors, non-zero results,
bad frames), the call flows (dial/answer/hangup/DTMF/CLIR/emergency, ECPI/EAIC/ESPEECH, both
speech policies, call-start/call-stop timing against the stub, modem loss and SIGTERM with a call
up), SMS (receive, concatenation, send single/multipart/UCS-2, CMTI, list), modem loss and
recovery, configuration validation; IMS: the start-up sequence and its variants (cannot query,
`+EIMS: 0`, plain onoff form after a refusal, setup failure, `ims=0`), `"init"` inside the onoff
answer, the registrations of 0x4A03/0x8305, the modem's own IMS PDN (EIF_IND 101 + IMS_PDN_IND
full form → link without route/DNS, no DATA_ACT_CALL, no confirm), EIPSPI CMD → RSP (txid and
header byte, echoed index, distinct SPIs in range, no reuse in a one-value range), `+CIREGU`
services, `net` and the snapshot, an unrequested interface during `data up` told apart from the
internet one, teardown by IMS_PDN_IND action 0 and by EIF_IND down, IMS_PDN_IND before its
EIF_IND, the short form, the AT notify fallback (confirm 0 only), reject/oos, `ims off`/`on`,
coexistence with internet data both ways, modem loss and SIGTERM; the MIPC IMS configuration
(both `IMS_SET_CONFIG_REQ` forms byte-exact against the protocol description, slot, order key → flags → tunnel,
the start-up waiting for the MIPC session, `ims config` re-send, a non-zero CNF result,
`ims_mipc_key=0`, `AT+EIMSRCS=0,0` through `SYS_AT_REQ` and its error); the APN profile list
byte-exact for three (888 bytes) and four profiles with mms (1176 bytes, elements 0xFFFC..),
`apn profiles` and a non-zero CNF result, the order IMS_SET_CONFIG < APN list < onoff < tunnel <
radio on, the radio restored after a modem loss, `radio off` respected across a loss,
`radio_mask` 3 after the set-up, SIM busy retried; the IMS address from the `+EIF` URC (after
and before an address-less EIF_IND, on the AT port and through the tunnel), the array-form
record, the `AT+CIREG?` poll (fast until registered, then slow), the APN list re-sent with the
internet profile once registered. 386 checks.

## Open questions

[QUESTIONS.md](QUESTIONS.md): the vendor facts still needed, what the daemon does meanwhile, and
which key or constant takes the answer.
