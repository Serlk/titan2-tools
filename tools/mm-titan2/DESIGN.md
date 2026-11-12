<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# ModemManager plugin "mtk-ccci" for the Titan 2 modem: design and status

Modem control for the Titan 2 (MediaTek MT6878, integrated modem) on mainline Linux is moving
from our interim daemon `t2-modemd` (`tools/modemd`) to ModemManager. This directory holds the
first iteration: an in-tree ModemManager plugin, the scripts that put it into a ModemManager
source tree and build it for the phone, and a small helper for the voice audio path.

Clean room: the plugin is written from `tools/modemd` (our own GPL daemon), clean-room
behavioural descriptions of the MIPC, data and voice/IMS protocols (facts only),
`tools/speechd/README.md`, our kernel driver (`drivers/net/wwan/mtk_ccci/` of the titan2-linux
kernel) and the ModemManager tree itself. No vendor code, no disassembly. Everything not
confirmed on the device is marked **(unverified)** in the code comments and collected in §9.

| path | what |
|---|---|
| `plugin/` | the plugin sources (copied by `apply.sh` into `src/plugins/mtk-ccci/` of a ModemManager tree) |
| `plugin/tests/` | unit test of the MIPC coding, pty test of the MIPC port against a fake modem |
| `apply.sh` | copies the plugin into a ModemManager tree and patches its meson build (idempotent) |
| `mm-mtk-ccci.patch` | the same as one patch against ModemManager `fcc9d19f` (what Alpine 3.24 ships) |
| `build-aarch64.sh` | cross build of the plugin `.so` and the helper for Alpine 3.24 aarch64 |
| `build-on-phone.sh` | the same build on the phone itself (Alpine's `abuild`, produces apk packages) |
| `speech-trigger/` | `t2-mm-speech-trigger`: D-Bus → t2-speechd bridge (alternative to the in-plugin hook) |
| (none) | `build-on-phone.sh` takes Alpine's 3.24-stable `community/modemmanager` packaging from aports (`APORTS=` or a fresh clone) and adds `mm-mtk-ccci.patch` |
| `INSTALL.md` | what to do on the phone |

## Status

Tested on the phone: modem start/stop without t2-modemd, both SIMs (ICCID/IMSI read, both
active), IMS registration on both SIMs, outgoing and incoming VoLTE calls with two-way audio,
IPv4 mobile data with repeated disconnect/reconnect, warm ModemManager restart and a
disable/low/enable cycle. Incoming SMS reached ModemManager through the storage scan.
Not yet confirmed: IPv6, SMS sending, SMS notices (below), the RAT-mode settings, suspend.

### SMS readiness and recovery scan

Messaging waits for readiness before generic initialization caches CPMS support: fresh CPMS
test/query and CMGF=0, at most 30 attempts with one-second retry delays for transient
CMS/SIM-busy errors. Missing/locked SIMs remain classified by generic MM. Creating an SMS with
UNKNOWN default storage reproduces an upstream SIGABRT locally.

A recovery reader scans the selected stack's supported SM/ME storages, using generic MM
storage locks, PDU parsing, SMS list and duplicate checks. Scans do not delete SMS, overlap,
run before PDU/storage/list readiness, or continue across disable/reprobe epochs. Native MIPC
SMS delivery, status reports and simultaneous SMS reception on both SIMs are not implemented.

### SMS notices (built, not yet tested on the phone)

A stored SMS raised no URC on `wwan0at0`: this modem's URCs come as SYS_AT_IND on the MIPC AT
channel. Messaging enable now (after the generic +CNMI on `wwan0at0`, whose result stays the
result):

- sends `AT+CNMI=2,1,2,1,0` through SYS_AT_REQ for slot 1 and slot 2 (best effort);
- registers the SMS indications 0x4601/0x4602/0x4603/0x4606 per slot (the set the factory
  firmware's RIL registers at boot; registration failures are logged, not fatal).
  The modem->AP commands 0x8601/0x8603 are not registered (answer format unknown).

A +CMTI/+CDSI in SYS_AT_IND or one of those indications for the selected slot takes a
5-second kernel wakelock (`/sys/power/wake_lock`, `mm-mtk-ccci-sms`) and scans SM/ME at once;
a notice during a scan gives one follow-up scan. The fallback scan runs every 300 s while
awake and once after a suspend (first MIPC message or timer tick, at most every 30 s).
Notices for the non-selected SIM are logged only. Host tests: 42/42; six plugin suites pass
ASan/UBSan/LeakSanitizer.

### RAT mode / allowed and preferred modes (built, not yet tested on the phone)

From a factory-firmware MIPC capture and the GPL mtk-legacy plugin (`+ERAT`).
`load_supported_modes` takes the mode mask the generic probing finds and offers the combinations of
`mm-modem-helpers-mtk-ccci.c` that fit in it (NR first, LTE first, 2G/3G/4G, 4G/5G, 4G, 3G first,
2G/3G, 3G, 2G). `set_current_modes` sends NW_SET_RAT_REQ (u8 rat, u8 prefer_rat) on the selected
slot; `load_current_modes` reads `AT+ERAT?` on that slot's MIPC AT channel and reports the
combination it set when the pair matches (else the decoded one, else the last pair set). Before
EFUN at cold start and at every enable the plugin sets the factory default 22/128
(NR → LTE → 3G/2G), falling back to LTE first (6/3) when the modem rejects it; a client's choice
is kept for the lifetime of the modem object. Unverified: `+ERAT?` on MT6878, every pair except
22/128, prefer_rat 128's meaning. Only the selected slot is set (the factory firmware sets both).

## 1. The modem as ModemManager sees it

The mtk_ccci driver registers the ports in the `wwan` subsystem when the modem FSM reaches READY
(brought there by the `t2-modem` service, which loads the NVRAM/RPC servers;
**leave its `T2_MODEM_RADIO` empty** so that MM owns radio power):

| kernel name | type attribute | plugin use |
|---|---|---|
| `wwan0at0` | `AT` | primary AT port (CCCI UART2): 3GPP control, voice, SMS |
| `wwan0mipc1` | `MIPC` | the MIPC data channel the factory software uses → `MMPortMipc` |
| `wwan0mipc0`, `wwan0mipc2..9` | `MIPC` | ignored |
| `wwan0-N` (net) | — | data links, created by the bearer (`ip link add ... type wwan linkid N`); the driver registers its `wwan_ops` with `WWAN_NO_DEFAULT_LINK`, so **no netdev exists before a connection** |

ModemManager handles the `wwan` subsystem natively (udev subsystem list from the plugins,
`MM_PORT_SUBSYS_WWAN`, the `type` attribute as a probing hint, `create_wwan_port` class hook). The
kernel's `MIPC` type is unknown to it, so without help it would AT-probe the MIPC ports; the
plugin prevents that.

ModemManager's own device grouping (physdev = the first `platform` ancestor, `d180000.mddriver`)
already puts all these ports in one device; the udev rules file sets `ID_MM_PHYSDEV_UID=mtk-ccci`
too, like the qcom-soc plugin does.

## 2. Plugin: `mm-plugin-mtk-ccci.c`

- Allowed subsystems `wwan`, `net`; allowed udev tag `ID_MM_MTK_CCCI` (set by
  `77-mm-mtk-ccci.rules` on every wwan/net device whose driver chain contains `mtk-ccci-md`).
- **Custom init** (`MM_PLUGIN_CUSTOM_INIT`, runs before AT probing with the port opened as AT):
  for a port whose kernel name contains `mipc` it calls `mm_port_probe_set_result_at (FALSE)` and
  `set_result_qcdm (FALSE)`, so the probe ends with no type and nothing is ever written to a MIPC
  channel. The AT port is probed normally (`AT`, `ATI`...).
- **grab_port**: `wwan*mipc1` (or whatever carries `ID_MM_MTK_CCCI_MIPC_DATA`) is grabbed as
  group USED, type UNKNOWN; the other MIPC ports as IGNORED; the AT port with the PRIMARY flag.
- **create_modem** requires the AT port and creates `MMBroadbandModemMtkCcci`.

Port organization: `mm_base_modem_organize_ports` ignores UNKNOWN-typed ports, finds the AT
primary, and needs *a* data port: the modem is created with `DATA_TTY_SUPPORTED` and
`DATA_NET_SUPPORTED` both TRUE, so the AT port stands in as the data port (never used for data;
the bearer returns the link netdev as the connection's data port, exactly as the MBIM/QMI
multiplexed bearers do).

## 3. Modem: `mm-broadband-modem-mtk-ccci.c` (MMBroadbandModem subclass)

Generic MMBroadbandModem does 3GPP registration (`+CREG/+CEREG/+CGREG`), signal quality (`+CSQ`),
operator, SIM (`+CPIN`, `+CIMI`, `+CRSM`), SMS in PDU mode (`+CMGF=0`, `+CNMI`, `+CMGS`, `+CMT`),
the generic AT voice calls (`ATD<n>;`, `ATA`, `+CHLD=1x`/`+CHUP`, `+VTS`, `RING`/`+CLIP`/`+CCWA`,
`+CLCC` polling). The subclass adds the MediaTek behaviour (`tools/modemd/src/vendor.c`):

| hook | what it does |
|---|---|
| `create_wwan_port` | name contains `mipc`, type UNKNOWN → `MMPortMipc` (kept in `priv->mipc`); else parent |
| `setup_ports` | reads the configuration from the AT port's udev properties (radio mask, SIM slot, speechd socket, WWAN device name), installs the vendor URC handlers disabled |
| `modem_power_up` | `AT+EFUN=<mask>` (udev `ID_MM_MTK_CCCI_RADIO_MASK`, default 3 = both radios, observed); on error falls back to `AT+CFUN=1`; errors not fatal (as generic) |
| `modem_power_down` | `AT+CFUN=4` (observed start-up state; `AT+EFUN=0` untried) |
| `load_power_state` | generic `AT+CFUN?` |
| `enabling_started` voice setup | `AT+ESUO=4` (SIM1) or `=5` (SIM2) to select the protocol stack, then the per-stack voice init: `AT+CMOD=0`, `ATS0=0`, `AT+CCWA=1`, `AT+CLIP=1`, `AT+CSSN=1,1`, `AT+CMUT=0`, `AT+ECPI=4294967295`, `AT+EAIC=2`, `AT+EVOCD=1` (all optional, errors logged). Run before radio-on even when the power transition hook would be skipped |
| `load_sim_slots` / `set_primary_sim_slot` | two active SIM objects; primary uses the generic AT SIM, secondary uses slot-addressed SYS_AT. Switching writes `/var/lib/ModemManager/mtk-ccci-sim-slot` and returns success → ModemManager reprobes the modem → the new stack is selected at power-up. Same slot → `EXISTS` (no reprobe). Old bearers retain their original slot until teardown |
| `create_bearer` | `MMBearerMtkCcci` |
| 3GPP `setup/cleanup_unsolicited_events` | chain to parent, add vendor handlers and forward IMS registration/interface events to the IMS controller |
| Voice `setup/cleanup/enable_unsolicited_events` | chain to parent (RING, +CLIP, +CCWA, +CRC), add `+ECPI`, `+EAIC`, `+ESPEECH`, `+EVOCD`; enable sends `+ECPI=4294967295`, `+EAIC=2`, `+EVOCD=1` again |
| Voice `create_call` | `MMCallMtkCcci` uses slot-addressed SYS_AT; "dialing→ringing" and "ringing→active" reports are supported and incoming timeout is disabled |
| Voice `hangup_all` | `AT+CHLD=6`, parent's `+CHUP` as fallback |
| Voice `setup/cleanup_in_call_audio_channel` | the **t2-speechd trigger**, §6 |
| `enabling_started` | parent, then opens the MIPC port and runs the session start; session/IMS failure prevents enabling |
| `disabling_stopped` | closes the MIPC port (no `INTERNAL_CLOSE_REQ`; the next session starts with TEST/OPEN as after a client restart) |

### 3.1 The +ECPI call model

`+ECPI: <id>,<type>,<is_ibt>,<is_tch>,<dir>,<call_mode>,"<number>",<toa>,"<pau>"[,<cause>]` is
turned into `MMCallInfo` reports (`mm_iface_modem_voice_report_call`), which ModemManager
matches against its Call objects by index / state:

| type | report |
|---|---|
| 130 created | index=id, outgoing, DIALING |
| 2 alerting | outgoing, RINGING_OUT |
| 0 setup | incoming, RINGING_IN, number from field 7 |
| 6 connected | ACTIVE |
| 133 disconnected (cause logged) | TERMINATED |
| 7 all released, 131/132 held/active (which is which is unverified) | full `+CLCC` reload |
| 1, 3, 4, 5, 129, 134..137 | ignored (the factory firmware's RIL does not forward them either) |

`+CLCC` polling by ModemManager while calls are being established remains active as a second
source, through the plugin's own `load_call_list`: the modem reports IMS (VoLTE) calls with
`<mode>` 20..25 (observed on the phone: `+CLCC: 1,0,2,20,0,"+7...",145` for a dialing call), which
ModemManager's generic parser would skip and then terminate the call; the mode is rewritten to 0
(`mm-modem-helpers-mtk-ccci.c`, unit tested) before `mm_3gpp_parse_clcc_response`. +ECPI reports
are not filtered by call mode (20..25 is logged only).

### 3.2 +EAIC approval

`+EAIC: <id>,"<number>",<type>,<mode>,<seq>[,...]` is answered at once with
`AT+EAIC=0,<id>,<seq>` (let the call proceed: the modem then sends RING, +CLIP, +ECPI 0), as
`tools/modemd/src/call.c` does with `eaic_auto_accept=1`. Rejecting at this stage
(`AT+EAIC=1,<id>,<seq>,21`) is not exposed: the user rejects the ringing call through the Call
object like with any other modem. The approval deadline is unverified (QUESTIONS B11); the answer
goes out from the URC handler, i.e. within milliseconds.

## 4. MIPC port: `mm-port-mipc.c`, `mm-mipc-proto.c`

`MMMipc*` in `mm-mipc-proto.[ch]` is the GLib port of `tools/modemd/src/mipc.c`: 16-byte header
(magic `0x24541984`, slot bit mask, msg id, txid, length), TLVs padded to 8 bytes, array
descriptors, privacy bit masked in lookups, result TLV 0x0000 (absent → 2), every id used by the
plugin as a named constant with a comment. Unit test: `tests/test-mipc-proto.c` (the
worked frame example, same checks as t2-modemd's `-T` self-test).

`MMPortMipc` is an `MMPort` subclass with its own nonblocking file descriptor, frame
write queue, and bounded table of pending transactions. A separate response path allows a
modem command to be answered while an AP request waits for its confirm. Outgoing frames are
never interleaved, including partial writes. Responses match txid, expected confirm id and
slot; unmatched/late confirms are discarded. IND/CMD signals preserve txid and byte 8.
Request validity uses an explicit table: IMS_SET_CONFIG_REQ is the even id 0x0A00.

Session: TEST (up to 10 attempts, 1 s each), OPEN version 2, REGISTER_IND on the selected
SIM bit for EIF/data/tunnel/IMS_PDN, REGISTER_CMD on slot 0xff for EIPSPI. OPEN and required
registrations must succeed. Port loss fails pending tasks and requests an MM reprobe.

The fake-modem pty tests exercise fragmentation, late/mismatched confirms, reversed concurrent
responses, SPI CMD/RSP while waiting for another confirm, timeout, cancellation before sending,
queued request closure and refused OPEN. IMS-controller tests mock AT/rtnetlink boundaries
while retaining the real MIPC transport.

AT tunnel text preserves CR/LF/TAB for protocol parsing; other control bytes are
replaced. Generic strings used in logs remain single-line. Pending transactions
and their timers are detached before completing a GTask: a synchronous completion
callback may close the port, including during a timeout, without completing the
same task twice.

The current local tests also cover multiline IMS responses/URCs, warm IMS setup,
stopping setup during a pending query, SIM2 identifiers and CME errors, and bearer
rollback after cancellation or link failure. Bearer tests use the real MIPC port
and a fake peer on a pty; modem accessors and rtnetlink calls are mocked. Existing
links and IMS links are not deleted by a failed connection attempt.

## 5. Bearer: `mm-bearer-mtk-ccci.c` (MMBaseBearer subclass) and `mm-wwan-link.c`

Not an `MMBroadbandBearer`: there is no AT dial and no pre-existing net port. Connect steps:

1. **check**: the modem has a MIPC port with a ready session.
2. **APN**: from the bearer properties; if empty, the lowest context of `AT+CGDCONT?` (the LTE
   default bearer, cid 0 on this modem, observed). If CID 0 is absent, report that
   an explicit APN is required; a remaining CID 1=ims is not an internet default.
   Before activation, synchronize the selected IMS controller's APN profile list with
   the connection's actual APN and wait for a successful APN_ADD_PROFILE_LIST_CNF.
   Keep the IMS/sos service profiles and the other SIM's profiles. Refusal or cancellation
   prevents DATA_ACT_CALL; no radio restart is used to recover this configuration.
3. **activate**: `DATA_ACT_CALL_REQ` (0x0201, slot bit of the stack, 60 s) with the TLV set of
   the captured request: APN 0x0101, type 0x0102 = 1 (default), PDP type 0x0103 and
   roaming PDP type 0x0104 (1 IP / 2 IPv6 / 3 IPv4v6 from the bearer's ip-type, IPv4 when
   unset), auth 0x0105 = 0 (**unverified coding**), user 0x8106 / password 0x8107 (empty strings
   when unset), 0x0108 = 0x7F, bearer mask 0x0109 = 0x7FFFFFFF, 0x010A = 0, 0x010F = 0. The
   672-byte traffic descriptor 0x010D is not sent (t2-modemd's default too).
   Confirm: result 0 required; aid 0x0100, transIntfId 0x012A (else direct interface id
   0x0125, else the one interface indication seen during the request, with a warning),
   `link id = transIntfId mod 100`, IPv4 address 0x8104 (first), DNS 0x810E / 0x8113, MTU
   0x0123 (0 or absent on the phone → 1500, what the factory software sets), EPS bearer 0x012C (logged). Every TLV id of the confirm is logged at debug level for the
   open questions.
   `INTERNAL_EIF_IND` (0x4302) arriving while connecting is recorded **per transIntfId**; the
   address of the record whose transIntfId equals the confirm's is used when the confirm carries
   none. Indications for other interfaces (the modem brings the IMS PDN up by itself; it appeared 0.2 s after the internet PDN in a factory-firmware trace) are ignored by the
   bearer at every stage.
4. **IP fallback**: no address or no DNS → `AT+CGCONTRDP=<aid>` on the AT port (observed format
   for cid 0).
5. **link add**: rtnetlink `RTM_NEWLINK` with `IFLA_LINKINFO{kind "wwan", data{IFLA_WWAN_LINK_ID
   N}}` and `IFLA_PARENT_DEV_NAME` = the wwan device name (from the AT port's wwan class device,
   else "wwan0"), ifname `<wwan device>-N`; an existing link is refused (EEXIST), so its
   ownership is not taken over. After a new EXCL creation, a stale MM link-port with that
   name is released before waiting for udev; its cached ifindex belongs to the deleted netdev.
   ModemManager has
   no generic "add wwan link" helper (MBIM/QMI use libmbim/libqmi for it), hence
   `mm-wwan-link.c` (synchronous, one request + ack).
6. **wait for the netdev**: `mm_base_modem_wait_link_port ("net", ifname, 5 s)` — the udev event
   for `wwan0-N` is routed to our device (same physdev / `ID_MM_MTK_CCCI` tag; the udev rules file
   tags `net` devices of the modem too), grabbed as a link port.
7. **link up**: `mm_port_net_link_setup (up, MTU)`.
8. **result**: data port = the link `MMPortNet`, multiplexed; IPv4 config method STATIC,
   address/32, **gateway = own address** (what the factory software hands to Android;
   NetworkManager adds the default route through it), DNS list, MTU. IPv6: STATIC/64 when the
   modem gave a global address, else DHCP (SLAAC by the kernel inside the PDN, unverified) when
   IPv6 was requested, else none.

Disconnect: `DATA_DEACT_CALL_REQ` (0x0203: aid 0x0101, cause 0x0102 = 1 "normal", unverified
coding; 60 s; failures logged), link down, `RTM_DELLINK`. Modem-side end: `DATA_DEACT_CALL_IND`
(0x4202) for our aid or `INTERNAL_EIF_IND` command 1 for our transIntfId → connection reported
lost, link deleted; MIPC port closed → the same. No connection-status polling (the modem reports
losses); statistics from `/sys/class/net/<link>/statistics`.

A failed connection deletes the link it created; the PDN may stay active in the modem (a second
activation of the same APN is unverified, questions 13).

## 6. Voice audio path (t2-speechd)

The voice samples never pass through the AP; `t2-speechd` (`tools/speechd`) switches the AP audio
path and the modem's speech engine on command over `/run/t2-speechd.sock` (`call-start --rate N`,
`call-stop`, reply ends with `ok`). The rule (the factory firmware's timing): speech on when a call
is dialing/ringing-out/held/active, off when none is; a ringing incoming call is not yet "in call".
ModemManager has exactly this notion: the voice interface calls
`setup_in_call_audio_channel` when the first call enters one of these states and
`cleanup_in_call_audio_channel` when the last one leaves them. Two implementations, use one:

1. **In-plugin (default)**: the hooks connect to the socket (udev
   `ID_MM_MTK_CCCI_SPEECHD_SOCKET`, default `/run/t2-speechd.sock`, empty = off) and send
   `call-start --rate 48000` / `call-stop`; a failure is logged and never fails the call. No
   audio port/format is reported on the Call object.
2. **`speech-trigger/t2-mm-speech-trigger`** (ModemManager-independent, libmm-glib): watches the
   Call objects of every modem over D-Bus, applies the same rule, talks to the same socket;
   handles ModemManager restarts; `call-stop` on SIGTERM. For use with an unmodified ModemManager
   or when the in-plugin hook is disabled.

`+ESPEECH: <on>` is logged ("modem speech path on/off") and not acted upon (both policies of
t2-modemd exist for a reason, QUESTIONS B10–B12; the ModemManager state machine reproduces the
"dial" policy). `+EVOCD` codec is logged.

## 7. Build and deployment

- Host: `apply.sh` into `src/ModemManager` (checked out at `fcc9d19f`, Alpine 3.24's commit), then
  `meson setup build -Dplugin_mtk_ccci=enabled ...; ninja -C build`. The plugin is a shared module
  `libmm-plugin-mtk-ccci.so` (ModemManager plugin ABI 5.0, same source commit as the phone's
  package) plus a udev rules file; nothing in the daemon changes. Tests:
  `meson test -C build test-plugin-mtk-ccci test-plugin-mtk-ccci-port-mipc test-udev-rules`.
- Phone (Alpine 3.24 aarch64, `modemmanager-1.25.95_git20260527-r1`): `build-aarch64.sh`
  cross-builds the `.so` and the helper with the project's clang against a sysroot populated with
  Alpine's own packages (same `WITH_QMI/MBIM/QRTR/SUSPEND_RESUME/UDEV/POLKIT` configuration as the
  package: checked in `config.h` and in the package's binary), or `build-on-phone.sh` rebuilds the
  whole package with the plugin inside using Alpine's `abuild`. INSTALL.md has the steps.

Service order on the phone (OpenRC): `t2-ccci-fsd`, `t2-ccci-rpcd` and `t2-modem` (modem to READY,
radio left off); `dbus`, `polkit`; then `modemmanager` and `networkmanager` (and
`t2-speechd`). ModemManager starts before the modem is READY: the wwan ports simply appear later
(udev events) and the modem is probed then. **`t2-modemd` must not run** at the same time: both
would open `wwan0at0`/`wwan0mipc1` (one opener per CCCI port).

## 8. IMS controller: mm-mtk-ccci-ims.[ch]

Stack selection (`ESUO`) and MIPC session setup run in `initialization_started` before SIM
reads. On a cold CFUN=4 modem, both SIMs are inaccessible until EFUN. Initialization therefore
configures IMS for both stacks, powers the radio with EFUN, waits for CPIN, and only then
allows generic SIM discovery. `enabling_started` also requests IMS/voice setup; the IMS setup
is idempotent while configured, avoiding an extra CFUN=4 after cold discovery.

IMS setup: CIREG?; CFUN=4 unless already IMS-registered; IMS_SET_CONFIG key and flags;
CGDCONT?; APN profile list (internet if known, ims, sos); CIREG=2; EIMSPDN onoff (spaced form,
plain fallback); EIMSCFG; EIMSRCS through SYS_AT. Essential setup failures stop enabling.
SIM-busy radio startup is retried; no claim of successful power-up on an error.

The controller joins EIF and positive IMS_PDN labels by transIntfId. Only labelled IMS PDNs
get links; unrelated internet indications do not. Link setup has no routes/DNS. Address can
arrive in a direct/array EIF record or in a +EIF URC on AT/the MIPC tunnel. EIPSPI replies
preserve the request header and do not reuse session SPIs (bounded allocation table).
CIREG is polled at 5 s until registered, then 60 s; a missing internet APN is queried and
the profile list refreshed after attach. Port loss/disable disposes of owned IMS links.

`t2-modemd` remains a rollback/reference implementation, not a required initializer.
Cold modem stop/start, both IMS registrations and outgoing calls on both SIMs are confirmed.
Warm MM restart and one disable/low/enable cycle pass IPv4 data checks on SIM1.
IPv6, SMS and the rest of the feature matrix still need separate phone tests.
Existing same-name links are deliberately not adopted without
ownership verification; stale-link recovery after a hard crash remains open.

## 9. Unverified items and risks (in code order)

| where | item | source |
|---|---|---|
| probe | the kernel `type` attribute of the MIPC ports is "MIPC" (Linux ≥ 6.9); ModemManager ignores it and relies on the plugin's custom init — if `wwan0mipc1` were ever AT-probed the modem would receive `AT\r` on the MIPC channel (harmless but noisy) | kernel driver |
| power | `AT+EFUN=3` when the radios are already on (the `t2-modem` service did it) is expected to be harmless; `AT+EFUN=0` untried | QUESTIONS C17 |
| stack | URCs of the non-selected stack on the single AT port: unknown; everything on the port is attributed to the selected stack | QUESTIONS B13 |
| SIM slots | both SIMs are real active objects; secondary PIN/network operations are unsupported, and switching primary reprobes the whole modem (data drops) | implementation / phone observations |
| MIPC | minimal session (TEST/OPEN/REGISTER) accepted; version 2; registration required; result codes ≠ 0 | QUESTIONS A1, A2 |
| bearer | auth type coding (0x0105), 0x0108/0x010A meaning, deactivation cause coding, masked CNF TLVs, EIF record layout beyond 4 bytes, IPv6 TLVs, repeated activation of the same APN | QUESTIONS A3–A6, questions 5–10, 13 |
| bearer | gateway = own address and prefix /32 as the static config NetworkManager receives (the factory Android got the same) | factory-firmware trace |
| voice | `+ECPI` field order and `dir` polarity; which of 131/132 is held; `+EAIC` deadline; `+ESPEECH` timing relative to speech-on | QUESTIONS B10–B14 |
| voice | the in-call hook fires at DIALING (after `ATD` returned OK) — the factory firmware's timing; in-band ring-back audibility | factory-firmware trace |
| speechd | `call-start --rate 48000`: the rate t2-speechd assumes | t2-speechd README |
| deploy | the cross-built `.so` relies on matching the package's build configuration (checked) and on `libmm-glib.so.0` ABI 0.11 (same commit) | §7 |

## 10. Host build and tests

Built: ModemManager `fcc9d19f` + plugin, host (x86_64, QMI disabled because the host's libqmi is
older than the tree needs; irrelevant for the plugin), all six plugin sources warning-free under
ModemManager's strict flags; cross-built for aarch64/musl (plugin `.so` + helper). Tests passed:
`test-plugin-mtk-ccci` (6 checks incl. the +CLCC normalisation), `test-plugin-mtk-ccci-port-mipc`
(1 scenario, ~50 assertions), `test-udev-rules` (our rules file through ModemManager's parser);
ModemManager's whole suite 38/38.
