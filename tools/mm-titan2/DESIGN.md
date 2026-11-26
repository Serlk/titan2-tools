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
kernel) and the ModemManager tree itself. No proprietary code, no disassembly. Everything not
confirmed on the device is marked **(unverified)** in the code comments and collected in §9.

| path | what |
|---|---|
| `plugin/` | the plugin sources (copied by `apply.sh` into `src/plugins/mtk-ccci/` of a ModemManager tree) |
| `plugin/mm-mtk-ccci-shared.[ch]` | what both modem objects share: MIPC port/session, IMS controllers, radio coordinator, speech path, call approval, LTE start-up contract (§11, §12) |
| `plugin/mm-mtk-ccci-at-bridge.[ch]` | the SIM2 object's AT port over the slot-2 MIPC AT tunnel (§11) |
| `plugin/tests/` | unit test of the MIPC coding; pty tests of the MIPC port, the shared context, the AT bridge and the modem object (`test-modem.c` compiles the modem source in) against a fake modem |
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
Both SIMs at the same time as two modem objects (§11) and an incoming VoLTE call taken while
the phone was asleep have worked on the phone. Not yet confirmed: IPv6, SMS notices (below),
SMS on SIM2, the DSDS rejection (§11.7), the optional LTE start-up groups (§12).

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
- registers the SMS indications 0x4601/0x4602/0x4603/0x4606 per slot (the set Android's
  RIL registers at boot; registration failures are logged, not fatal).
  The modem->AP commands 0x8601/0x8603 are not registered (answer format unknown).

A +CMTI/+CDSI in SYS_AT_IND or one of those indications for the selected slot takes a
5-second kernel wakelock (`/sys/power/wake_lock`, `mm-mtk-ccci-sms`) and scans SM/ME at once;
a notice during a scan gives one follow-up scan. The fallback scan runs every 300 s while
awake and once after a suspend (first MIPC message or timer tick, at most every 30 s).
Notices for the non-selected SIM are logged only. Host tests: 42/42; six plugin suites pass
ASan/UBSan/LeakSanitizer.

### RAT mode / allowed and preferred modes (built, not yet tested on the phone)

From a MIPC capture under Android and the GPL mtk-legacy plugin (`+ERAT`).
`load_supported_modes` takes the mode mask the generic probing finds and offers the combinations of
`mm-modem-helpers-mtk-ccci.c` that fit in it (NR first, LTE first, 2G/3G/4G, 4G/5G, 4G, 3G first,
2G/3G, 3G, 2G). `set_current_modes` sends NW_SET_RAT_REQ (u8 rat, u8 prefer_rat) on the selected
slot; `load_current_modes` reads `AT+ERAT?` on that slot's MIPC AT channel and reports the
combination it set when the pair matches (else the decoded one, else the last pair set). Before
EFUN at cold start and at every enable the plugin sets Android's default 22/128
(NR → LTE → 3G/2G), falling back to LTE first (6/3) when the modem rejects it; a client's choice
is kept for the lifetime of the modem object. Unverified: `+ERAT?` on MT6878, every pair except
22/128, prefer_rat 128's meaning. Only the selected slot is set (Android sets both).

## 1. The modem as ModemManager sees it

The mtk_ccci driver registers the ports in the `wwan` subsystem when the modem FSM reaches READY
(brought there by the `t2-modem` service, which loads the NVRAM/RPC servers;
**leave its `T2_MODEM_RADIO` empty** so that MM owns radio power):

| kernel name | type attribute | plugin use |
|---|---|---|
| `wwan0at0` | `AT` | primary AT port (CCCI UART2): 3GPP control, voice, SMS |
| `wwan0mipc1` | `MIPC` | the MIPC data channel Android uses → `MMPortMipc` |
| `wwan0mipc2` | `MIPC` | ignored (never opened); in the dual-modem mode the only port of a second device `mtk-ccci-sim2`, the anchor of the SIM2 modem object (§11) |
| `wwan0mipc0`, `wwan0mipc3..9` | `MIPC` | ignored |
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
- **create_modem** requires the AT port and creates `MMBroadbandModemMtkCcci`; for the
  device that holds only the SIM2 anchor (`ID_MM_MTK_CCCI_SIM_ANCHOR=2`) it creates the SIM2
  object instead (`mm_broadband_modem_mtk_ccci_new_sim2`, §11), unless
  `ID_MM_MTK_CCCI_DUAL_MODEM=0`.

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
| `modem_power_up` | through the shared radio coordinator (§11.4): `AT+EFUN=<mask>` with mask = configured (udev `ID_MM_MTK_CCCI_RADIO_MASK`, default 3) minus the stacks in low power; SIM busy retried 30 × 1 s; `AT+CFUN=1` fallback for mask 1 |
| `modem_power_down` | the coordinator: `AT+EFUN=<remaining mask>` while another stack stays on, `AT+CFUN=4` when none does (observed start-up state; `AT+EFUN=0` untried) |
| `load_power_state` | generic `AT+CFUN?` |
| `enabling_started` voice setup | `AT+ESUO=4` (SIM1) or `=5` (SIM2) to select the protocol stack, then the per-stack voice init: `AT+CMOD=0`, `ATS0=0`, `AT+CCWA=1`, `AT+CLIP=1`, `AT+CSSN=1,1`, `AT+CMUT=0`, `AT+ECPI=4294967295`, `AT+EAIC=2`, `AT+EVOCD=1` (all optional, errors logged). Run before radio-on even when the power transition hook would be skipped |
| `load_sim_slots` / `set_primary_sim_slot` | dual-modem mode: not supported (one generic SIM per object, no SimSlots). Single-object mode: two active SIM objects; primary uses the generic AT SIM, secondary uses slot-addressed SYS_AT. Switching writes `/var/lib/ModemManager/mtk-ccci-sim-slot` and returns success → ModemManager reprobes the modem → the new stack is selected at power-up. Same slot → `EXISTS` (no reprobe). Old bearers retain their original slot until teardown |
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
| 1, 3, 4, 5, 129, 134..137 | ignored (Android's RIL does not forward them either) |

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
(`AT+EAIC=1,<id>,<seq>,21`) is used only in the DSDS case (the other SIM has a call, §11.7), with
the same decision as the MIPC approval; otherwise the user rejects the ringing call through the
Call object like with any other modem. The approval deadline is unverified (QUESTIONS B11); the answer
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
   0x0123 (0 or absent on the phone → 1500, what Android sets), EPS bearer 0x012C (logged). Every TLV id of the confirm is logged at debug level for the
   open questions.
   `INTERNAL_EIF_IND` (0x4302) arriving while connecting is recorded **per transIntfId**; the
   address of the record whose transIntfId equals the confirm's is used when the confirm carries
   none. Indications for other interfaces (the modem brings the IMS PDN up by itself; it appeared 0.2 s after the internet PDN in a trace under Android) are ignored by the
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
   address/32, **gateway = own address** (what Android's RIL hands to the framework;
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
`call-stop`, reply ends with `ok`). The rule (Android's timing): speech on when a call
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
| bearer | gateway = own address and prefix /32 as the static config NetworkManager receives (Android got the same) | trace under Android |
| voice | `+ECPI` field order and `dir` polarity; which of 131/132 is held; `+EAIC` deadline; `+ESPEECH` timing relative to speech-on | QUESTIONS B10–B14 |
| voice | the in-call hook fires at DIALING (after `ATD` returned OK) — Android's timing; in-band ring-back audibility | trace under Android |
| speechd | `call-start --rate 48000`: the rate t2-speechd assumes | t2-speechd README |
| deploy | the cross-built `.so` relies on matching the package's build configuration (checked) and on `libmm-glib.so.0` ABI 0.11 (same commit) | §7 |
| dual | MM creates a modem for a device whose only port is ignored (the anchor): expected from the code (`mm-plugin.c`, `mm-port-probe.c`) | §11.1 |
| dual | `AT+EFUN=2` alone (SIM2 radio without SIM1); whether `AT+CFUN=4` on the SIM1 stack also switches SIM2 off; `AT+CFUN?` on a slot's tunnel channel reports that stack's radio | §11.4 |
| dual | `AT+EFUN`/`AT+CFUN` through SYS_AT slot 1 (only used when wwan0at0 is not open) | §11.4 |
| dual | SMS submit through the tunnel as one string `AT+CMGS=<n>\r<pdu>\x1a`; multi-line SYS_AT replies (+CMGL) are one string | §11.3 |
| dual | IMS configuration of one stack with the other stack's radio on and without a radio cycle | §11.4 |
| dual | DSDS: one speech path for both stacks | §11.5 |
| dual | DSDS: whether the modem asks for approval (CALL_APPROVE_INCOMING_CMD or `+EAIC`) at all for the other SIM's incoming call during a call, or rejects/busies it itself, or only signals waiting | §11.7 |
| dual | DSDS rejection with cause 21: what the caller hears, and whether Android's RIL sends 21 or another cause in this case | §11.7 |
| lte | the meaning of the start-up requests of groups 2 and 3 and of `prefer_rat` 128; the EARFCN/PCI offsets of the registration block (inferred) | §12 |

## 10. Host build and tests

Built: ModemManager `fcc9d19f` + plugin, host (x86_64, QMI disabled because the host's libqmi is
older than the tree needs; irrelevant for the plugin), all six plugin sources warning-free under
ModemManager's strict flags; cross-built for aarch64/musl (plugin `.so` + helper). Tests passed:
`test-plugin-mtk-ccci` (6 checks incl. the +CLCC normalisation), `test-plugin-mtk-ccci-port-mipc`
(1 scenario, ~50 assertions), `test-udev-rules` (our rules file through ModemManager's parser);
ModemManager's whole suite 38/38.

## 11. Two SIM stacks as two modem objects (dual-modem mode)

### 11.1 Why this shape

The modem runs DSDS: `AT+EFUN=3` powers both stacks, both SIMs register and IMS registers on
both (phone, 29.10). ModemManager has one SIM stack per modem object: one 3GPP registration, one
signal, one Messaging, one Voice interface. The single-object design could only switch between
the stacks (PrimarySimSlot → reprobe → `AT+ESUO`). Two objects give each SIM its own registration,
signal, SMS and calls through unchanged generic ModemManager code, and every client (mmcli,
NetworkManager, WirePlumber's modem-manager module, the front end) handles several modems already.

MM's grouping is per *device* (udev `ID_MM_PHYSDEV_UID`): one device = one modem object = one
plugin `create_modem` with that device's probed ports. So SIM2 needs a device of its own:

- **Anchor.** The udev rule gives `wwan0mipc2` `ID_MM_PHYSDEV_UID=mtk-ccci-sim2` and
  `ID_MM_MTK_CCCI_SIM_ANCHOR=2`; it keeps `ID_MM_PORT_IGNORE=1`, so MM never probes or opens it
  (`mm-port-probe.c`: an ignored port is not probed). MM creates the device and asks our plugin
  (udev tag) for a modem; `create_modem` sees the anchor and no AT port.
- **AT port.** MM supports AT ports on unix sockets: subsystem `virtual` →
  `base_modem_create_virtual_port` → `MMPortSerialAt` with `MM_PORT_SUBSYS_UNIX`, which connects
  to the socket address named by the port (`abstract:` prefix = abstract socket) on every open
  (`mm-port-serial.c`). The SIM2 constructor creates the bridge (§11.3), builds an
  `MMKernelDeviceGeneric` for `virtual`/`abstract:mm-mtk-ccci-sim2-at-<pid>-<n>` exactly as
  `mm-plugin.c` does for virtual devices, and grabs it as the PRIMARY AT port before
  `mm_base_modem_organize_ports`. Generic code then runs per stack: `+CPIN`, `+CIMI`, `+CRSM`,
  `+CSQ`/`+CESQ`, `+CREG/+CEREG` (URCs included, §11.3), `+COPS`, SMS in PDU mode, `+CUSD`. All
  symbols used (`mm_kernel_device_generic_new_with_rules`, `mm_kernel_event_properties_*`,
  `mm_port_serial_at_command`, `mm_log_object_get_type` …) are exported by Alpine's
  ModemManager/libmm-glib (checked with llvm-nm against the sysroot: 111/111 `mm_*`).
- **A pty instead of a socket** was the alternative; it needs a tty device name under /dev and
  gains nothing (MM's port treats both the same; the socket form needs no tty configuration).

### 11.2 One MIPC session for both objects (`mm-mtk-ccci-shared.c`)

Android's RIL drives both slots over one MIPC channel (`ttyCMIPC1`, header slot bit).
A second channel (`wwan0mipc2`) as a separate
session was not used: that it is an independent session is unverified, and one opener per CCCI
port means SIM1's reprobe would otherwise have to coordinate two ports anyway.

`MMMtkCcciShared` (GObject, MMLogObject "mtk-ccci-shared/wwan0mipc1") is found by the MIPC port
name in a process-wide registry; each modem object holds a reference (SIM1 from
`create_wwan_port`, SIM2 from its constructor; whichever comes first creates it). It owns:

- the **MMPortMipc** of `wwan0mipc1` (SIM1 also grabs it as its port; SIM2 only references it).
  The port's open count already allowed several openers; the session start is now single-flight
  (`mm_port_mipc_open_session` joins a start in flight: one TEST/OPEN/REGISTER on the wire). A
  reprobe of either object leaves the session to the other; the last close ends it (so
  `mmcli --inhibit-device=mtk-ccci` does not free `wwan0mipc1` while the SIM2 object exists:
  inhibit both devices to hand the ports to another program). A forced close (port lost)
  reprobes both objects and forgets the radio state;
- the two **IMS controllers** (owner = the shared object, so their tasks keep it alive); setup is
  single-flight per stack (both objects and the coordinator may ask);
- the **incoming-call approval** responder (CALL_APPROVE_INCOMING_CMD → RSP): answered once per
  command, also when the SIM's object does not exist (yet); allow, except the DSDS case (§11.7);
- the **radio coordinator** (§11.4) and the **speech path count** (§11.5).

Each object filters MIPC indications by the slot bits it serves (`served`: SIM1 0x01, SIM2 0x02,
single-object mode 0x03): calls (CALL_STATUS_IND, index namespace 1..7 / 101..107 as before), SMS
notices, RAT indications, voice init per slot, `+CLCC` and `AT+CHLD=6` per slot.

### 11.3 The AT bridge (`mm-mtk-ccci-at-bridge.c`)

Listens on the abstract socket; accepts only its own process (SO_PEERCRED pid; the abstract
namespace is visible to the network namespace). Per command line: `AT…<CR>` → SYS_AT_REQ slot 2,
CNF text reframed as `\r\n<line>\r\n…\r\n<final>\r\n` (the CNF result gives OK/ERROR when the text
has no final code); V.250 line settings (`ATZ`, `ATE`, `ATV`, `ATX`, `AT&C`, `AT&F`, `AT`) are
answered locally (`ATZ` at enabling would otherwise reset the slot channel's `+EURCRPT`, `+CNMI`,
`+CREG` settings); `AT+CMGS=`/`AT+CMGW=` get the `> ` prompt locally and the PDU up to Ctrl-Z is
sent with the command as one tunnel string `AT+CMGS=<n>\r<pdu>\x1a` (**unverified** that the
tunnel takes it). Replies for a client that went away are dropped (epoch). Timeouts: 30 s, `+COPS=?`
180 s, `+CMGS`/`+CMSS` 120 s, `ATD`/`+CUSD`/`+COPS=` 60 s.

URCs: the SIM2 object pushes the slot's SYS_AT_IND text into the bridge; only lines that always
have a handler on the port are written (MMBroadbandModem's setup_ports installs `+CREG` family,
`+CIEV`, `+CMTI`, `+CUSD`; the plugin `+EIND/+EFUN/+ESUO`), anything else would be taken into the
next command's reply. Calls, IMS, `+CDSI` and the SMS indications are handled from MIPC directly.

SIM2's initialization: no `+ESUO` (that selects the stack of the physical UART only), the MIPC
session, then `+CMEE=1` and `+EURCRPT=2` on its channel, `+CFUN?`, cold-start path as SIM1.

### 11.4 Radio power: one EFUN mask

`AT+EFUN` takes a SIM mask, so the stacks' radios are one setting. The coordinator keeps
configured (udev radio mask, from the SIM1/single object), low-power (stacks a client put into
low power), and on/known (from `AT+CFUN?` of each object on its own stack, the coordinator's own
read-only `AT+CFUN?` on a slot's tunnel channel, and the commands it sent). Requests run one at a
time: power up of an object clears its low-power bit and sends `AT+EFUN=<configured & ~low>`;
low power sets the bit and sends `AT+EFUN=<rest>`, or `AT+CFUN=4` when nothing is left. Commands go
to the SIM1 object's `wwan0at0` (the proven path) when that port is registered and open; the SIM2
object waits up to 20 s for it, then uses SYS_AT slot 1 (**unverified** for radio power).

Cold start (an object's `AT+CFUN?` says off): RAT default of its slot, then
`radio_prepare` — IMS setup of every stack about to come on (the requester's failure is fatal,
the other's logged), the voice init on its AT port, then power up. With SIM1 first this is the
old single-object order: IMS SIM1, IMS SIM2, voice init, `AT+EFUN=3`.

The IMS controllers' radio-off step (CFUN=4 before a cold IMS configuration when the stack is not
IMS-registered) goes through a hook: single-object mode as before (CFUN=4); dual-modem mode only
when this stack is on and every other configured stack is known off — never with the other SIM's
radio on or unknown (configure with the radio on instead; whether IMS then comes up without a
radio cycle is **unverified**).

### 11.5 Voice, SMS, data

- **Speech:** one speech path. The first object entering in-call sends `call-start`, the last one
  leaving sends `call-stop`. DSDS has one active call at a time: the other SIM's incoming call
  during a call is rejected at the approval step (§11.7), as Android's RIL does; whether the modem
  asks for that approval at all during a call is **unverified**.
- **SMS:** per object — SIM2's generic messaging (CPMS/CMGF/CNMI, CMGL/CMGR, CMGS) runs on its
  tunnel channel, its notices (`+CMTI` via the bridge, SMS indications, the fallback scan) are its
  own. SIM1 unchanged.
- **Data:** SIM1 only by default. The SIM2 object refuses bearers unless
  `ID_MM_MTK_CCCI_SIM2_DATA=1` on the anchor. A NetworkManager GSM profile must be bound to SIM1
  (`gsm.sim-id` = SIM1 ICCID), otherwise NM may try it on SIM2 first.
- **SIM slots:** each object has one generic SIM; no SimSlots/PrimarySimSlot;
  `/var/lib/ModemManager/mtk-ccci-sim-slot` is ignored in this mode.
- **Telling the objects apart:** `Modem.Device` is `mtk-ccci` (SIM1) / `mtk-ccci-sim2` (SIM2);
  the SIM2 object's primary port is `abstract:mm-mtk-ccci-sim2-at-…`, its ignored port `wwan0mipc2`.

### 11.6 Single-object mode

`ID_MM_MTK_CCCI_DUAL_MODEM=0` (a 79-mm rule for the modem's wwan ports): the SIM1 object serves
both slots as before (PrimarySimSlot, state file, ESUO), the anchor device gets no modem object
(one log line); additionally resetting `ID_MM_PHYSDEV_UID=mtk-ccci` on `wwan0mipc2` restores the
old port layout exactly. Radio power goes through the coordinator in this mode too, with the same
commands as before (EFUN=mask, CFUN=4, the IMS CFUN=4 step unchanged).

### 11.7 DSDS: an incoming call while the other SIM has a call

Android's RIL rejects an incoming call by itself when another SIM already has a call (DSDS);
the plugin does the same at the approval step. The policy lives in
the shared context (`mm_mtk_ccci_shared_incoming_decide`) and answers both channels the modem may
ask on: CALL_APPROVE_INCOMING_CMD 0x8901 (MIPC, answered by the context) and `+EAIC` on the SIM1
object's `wwan0at0` (answered by that object with `AT+EAIC=0|1,<id>,<seq>[,21]`).

- **Calls present.** Each modem object registers itself as the voice client of the slots it serves
  (SIM1 0x01, SIM2 0x02, single-object mode both; weak reference). "Does slot N have a call" is
  answered from that object's own MM call list: calls of the slot (index 1..7 / 101..107, or the
  call's own slot before an index exists) in `dialing`, `ringing-out`, `active`, `held`,
  `ringing-in` or `waiting` (`mm_call_mtk_ccci_count_present`). No separate counter: the call list is
  what +ECPI, CALL_STATUS_IND and the +CLCC reconciliation keep current.
- **Decision.** Other slot with at least one such call: RSP 0x8902 `0x0100` u8 0, `0x0102` call id,
  `0x0103` seq, `0x0101` u32 21 (in this order), log
  `SIM<n> incoming call <id> rejected: SIM<m> has a call (DSDS)`. Otherwise `0x0100` = 1 as before.
  The same (slot, id, seq) within 30 s (the other channel, a repeat) gets the same answer and is
  shown once. Same-SIM call waiting is not touched (a slot's own calls never reject it).
- **Front end (MM D-Bus).** On the rejected SIM's modem object a Call object is created
  (`mm_call_mtk_ccci_report_rejected`): `Direction` incoming, `Number` from the 0x8101 TLV (`""` when
  absent; `+EAIC`'s number on the AT path), `State` terminated, `StateReason` refused-or-busy
  (`MM_CALL_STATE_REASON_REFUSED_OR_BUSY` = 5), set before export. The Voice interface emits
  `CallAdded(path)` and updates `Calls`; no `StateChanged` goes out on D-Bus (the state is final
  before the object is exported). It stays in the call list until a client calls
  `Voice.DeleteCall` (MM never removes ended calls itself), like any ended call. It never rings,
  never enters the in-call state (no `call-start` to t2-speechd: the in-call check runs on state
  changes of listed calls only), and the call reports (index lookups), the generic matching and the
  +CLCC reconciliation skip terminated calls; a later report of the released call id is ignored, a
  new call with the same id is a new object. No modem object for the SIM (not created yet, or its
  voice interface not set up): reject and log only.
- **Switch.** `ID_MM_MTK_CCCI_DSDS_BUSY=0` on `wwan0at0` (read by the SIM1/single object, one setting
  for both SIMs): approve always (the behaviour before). Default 1.
- **Wire content:** only the documented RSP TLVs. Cause 21 is the documented value for a reject
  with reason "call rejected"; which cause Android's RIL sends in the DSDS case is not known
  (unverified, §9).
- **Tests:** `test-plugin-mtk-ccci-dual-modem` `dsds-approval` (RSP bytes, approve when idle, the
  repeat, no client, property 0, weak client, single-object mode), `test-plugin-mtk-ccci-dual-sim`
  `dsds-rejected-call` (the Call object, the call counts; reports and +CLCC leave it alone), and the
  new `test-plugin-mtk-ccci-modem` (the modem source compiled into the test): the DSDS flow end to
  end between two real modem objects, and the enabling IMS steps below.

**Callbacks with a shared-context source (the crash of the first dual-modem build).** Every async result of an object
the shared context owns has the context as its source object: the IMS controllers
(`mm_mtk_ccci_ims_new (shared, …)`; owner now typed `gpointer` and documented in the header) and the
radio coordinator (`radio_prepare`, `radio_set`). Audit of all plugin callbacks:
`ims_setup_ready`/`secondary_ims_ready` (fixed); `cold_prepare_ready`/`radio_set_ready`
(typed as the context, take the modem from their own task: correct);
`mm_broadband_modem_mtk_ccci_prepare_data` → the bearer's `data_profiles_ready`, typed as the modem
but receiving the context (harmless only because the finish ignores it): fixed, the modem wraps the
controller's result in a task of its own. MIPC port users (`MMPortMipc *` first parameter), the
SIM/SMS helpers and the AT bridge are typed correctly. `test-plugin-mtk-ccci-modem` drives the
enabling IMS step of the SIM1 and SIM2 objects (dual mode), the single-object secondary-stack path,
the cold-start radio prepare and the data profile step, and checks each result's source object;
with the old `ims_setup_ready` put back it fails as the phone did (`g_hash_table_size` on a garbage
pointer, checked).

## 12. LTE start-up contract and diagnostics

Before radio-on the plugin can send the start-up requests the modem receives under Android, each
group behind its own udev switch on `wwan0at0` (`77-mm-mtk-ccci.rules` lists them; overrides belong
in a later `79-mm` rule). Every request, its payload and its confirm (result and all TLVs) is
logged as a `rat-trace` line of the slot; a refusal never stops the start-up.

| switch | default | what is sent |
|---|---|---|
| `ID_MM_MTK_CCCI_DATA_ALLOW` | 1 | group 1: `DATA_SET_DATA_ALLOW_REQ` on the data SIM's slot (`ID_MM_MTK_CCCI_DATA_SIM`, default 1) after every radio-on of that slot (three sends: at once, +0.46 s, +1.10 s), when the data SIM changes, and once per MIPC session when the radio was already on; independent of any bearer |
| `ID_MM_MTK_CCCI_RADIO_CONFIG` | 0 | group 2: `NW_SET_RADIO_CAPABILITY` (both slots), `SYS_MULTI_SIM_CONFIG` and `SYS_SET_DSBP` (slot 1) before radio-on |
| `ID_MM_MTK_CCCI_PREFER_SBP` | 0 | group 3: `CALL_DATA_PREFER_SET` (both) and the `AT+ESBP`/`AT+EIAREG`/`AT+ECCAUSE` commands before radio-on |
| `ID_MM_MTK_CCCI_REG_CAUSE` | 0 | `AT+CREG=3`, `+CGREG=3`, `+CEREG=3` (reject causes) where `AT+CEREG=?` offers level 3 |
| `ID_MM_MTK_CCCI_START_PROFILES` | `before-radio` | the IMS/internet profile list before radio-on, `after-registration` or `off` |
| `ID_MM_MTK_CCCI_IMS_MIPC_CONFIG`, `_CIREG`, `_EIMSPDN`, `_EIMSCFG`, `_EIMSRCS`, `_START_RAT` | 1 | each IMS start-up step and the start-up RAT setting; 0 leaves that step out |
| `ID_MM_MTK_CCCI_RADIO_OFF` | `cfun` | radio off with `AT+CFUN=4`, or `mipc` (`NW_SET_RADIO_STATE_REQ` 0 per configured slot) |
| `ID_MM_MTK_CCCI_NW_DIAG` | 0 | read-only network diagnostics: the network indications registered and traced on both slots, `NW_GET_PS_REQ` every second from each radio-on until the first registration result |
| `ID_MM_MTK_CCCI_IA` (`_SIM1`, `_SIM2`) | 0 | initial-attach experiment: 0 = A (profile list only), 1 = B (also `APN_SET_IA_REQ` with the SIM's internet profile once the list is confirmed) |

**RAT start-up pair.** At cold start and at every enable the slot gets 22/128 (NR first) unless a
client chose other modes; when the modem rejects it, one retry with the LTE-first pair 6/4 (the pair
6/3 used before was rejected by the modem, result 2), never the rejected pair again. The mode is
read back with `AT+ERAT?` and decoded without the cache of what was set.

**Registration diagnostics.** The registration URCs of a slot arrive on its MIPC AT channel (not on
`wwan0at0`); the plugin parses them (level 2 and 3, `+COPS?` AcT), applies them to the 3GPP state of
the object's own stack and refreshes `AccessTechnologies`. A burst of registration URCs of one slot
triggers one read-only snapshot on that slot (`+CREG?`, `+CGREG?`, `+CEREG?`, `+COPS?`, `+ERAT?`,
`AT+CEER` after a denial). Each `rat-trace` line carries both clocks (`mono`, `boot`), so it can be
matched with the kernel log across suspend.

**Observed on the phone.** A cold LTE attach failure on both SIMs (both stayed on 3G, SIM2 without
IMS) was not a start-up request problem: the modem had written a bad record into its own NVRAM
working copy. `t2-modem-nvcheck` in titan2-tools (`tools/netroot`) checks that copy before the modem
starts. With a fresh working copy both SIMs attach to LTE and register IMS with the default switches.
