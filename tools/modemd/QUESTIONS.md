<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (c) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# t2-modemd: open questions (vendor facts still needed)

Facts the daemon needs but the clean side does not have, or has only as **(unverified)**
statements of the clean-room behavioural descriptions (question numbers refer to their own
open-question lists, which are not published). Each item says what the daemon does meanwhile
and where the answer goes (a configuration key or a named constant).

## A. Mobile data (blocks the first data test)

1. **Does DATA_ACT_CALL_REQ over MIPC alone start the user plane?** (questions
   1–3). The daemon sends the captured TLV set and creates `wwan0-<transIntfId mod 100>`; nothing is
   confirmed back. If the modem needs the REGISTER_IND step or the per-slot RIL start-up, note
   the result code of the CNF. Constants: `MIPC_DATA_ACT_CALL_*`, `MIPC_T_ACT_*` in `src/mipc.h`.
2. **Minimal session**: is `INTERNAL_OPEN_REQ` version 2 accepted, what does the CNF "target
   version" read, is `INTERNAL_REGISTER_IND_REQ` needed (questions 3, 4)? The daemon does
   TEST → OPEN → REGISTER(0x4302, 0x4201, 0x4202, 0x4001) for the selected slot.
3. **Traffic descriptor 0x010D** (672 bytes): needed or not (question 8). Default off
   (`mipc_traffic_descriptor=0`); layout beyond DNN length/string unverified.
4. **Masked CNF TLVs** 0x810F, 0x8121, 0x812F, 0x8131, 0x0128, 0x0129 (question 5): the daemon
   logs every CNF TLV id with `-v` ("mipc: cnf tlv 0x.... len ..."). IPv6 address TLVs of the
   confirm are unknown: IPv6 is taken from `INTERNAL_EIF_IND` records (first 16 bytes, size
   unverified) or from `AT+CGPADDR=<aid>` on the AT port.
5. **EIF_IND record layout** beyond the first 4 bytes (prefix length? gateway?) (question 6).
6. **DEACT cause values** (question 10): `MIPC_DEACT_CAUSE_NORMAL` = 1 ("normal").
7. **AT fallback** (questions 1–7): is `AT+EAPNACT` accepted on UART2 at all, does
   `+EIF:` arrive on this port, does the modem tolerate the spaces in `AT+EIF=%d, "ifst", %s, %d`,
   does `+EAPNACT=1` reuse cid 0 or create a new aid. All in `data_bind_cmds` / `data_deact_cmds`
   (`data_backend=at`). The deactivation reason is 1 ("normal"), not 0.
8. **Accept/refuse of a modem-activated PDN** (`AT+EAPNACT=2|3,<aid>`): not sent
   by the daemon; an unsolicited `+CGEV: ME PDN ACT` is only reported as an event.
9. **Does the modem need `AT+EDALLOW=1` / `AT+EDATASIM`** before data on a stack?
   Not sent; add to `init_cmds` to try.

## B. Voice (A1–A5)

10. **Order/timing of +ECPI / +ESPEECH around a CS call** (A1). The model accepts any order;
    `speech_on=dial` reproduces the factory timing, `speech_on=espeech` the modem-driven variant.
11. **+EAIC approval deadline** and whether `AT+EAIC=2` is required for `+EAIC` (A2). The daemon
    answers immediately from the URC handler; `eaic_auto_accept`.
12. **Is speech on before +ESPEECH: 1 harmless / is +ESPEECH: 1 alone enough** (A3): the two
    `speech_on` policies; result decides the default.
13. **Dual SIM on one AT port** (A5): do URCs of the non-selected stack arrive, how
    are they told apart. The daemon assumes everything on the port belongs to the selected stack
    (`sim 1|2`).
14. **+ECPI 131/132** (held/active, which is which), 7, 135–137; `dir` polarity (C11). The daemon
    only reports them and re-polls `AT+CLCC`.
15. **AT+CHLD=6 vs ATH vs AT+CHUP** for "hang up all": the daemon tries `AT+CHLD=6`, then `ATH`,
    then `AT+CHUP`. One call: `AT+CHLD=1<id>`.
16. **IMS/VoLTE** (`src/ims.c`). Settled by the factory-firmware traces: the modem
    raises the IMS PDN itself, the AP gives it a link and answers 0x8305. Still **(unverified)**:
    a. **Does IMS register on mainline now?** 2026-10-30 runs: AT only → no IMS PDN and
       interface 101 to internet; with the MIPC IMS configuration done before the radio
       (after a modem restart) 101 stayed free but still no IMS PDN, because the APN profile of
       type IMS was missing. The daemon now does: IMS_SET_CONFIG (both forms), then
       APN_ADD_PROFILE_LIST (internet 0x29, ims 0x02, sos 0x100, mms 0x04 with `mms_apn`), then
       `AT+CIREG=2`, `AT+EIMSPDN= "onoff",1`, `AT+EIMSCFG`, `AT+EIMSRCS=0,0` through the tunnel,
       and then the radio. Expected: the modem raises the IMS PDN (EIF_IND 101 + IMS_PDN_IND +
       two 0x8305) after attach or after `data up`, then `+CIREGU: 1,5`. Open:
       - At the first boot the t2-modem service powers the radio before the daemon. Either use
         `radio_off_before_setup=1`, or let the daemon power it (drop the service's EFUN).
       - The APN record fields marked unverified in the protocol description: id 0xFFFFFFFF, the user and
         password offsets, the auth value 1 for internet, the type bits of 0x29, the "enabled"
         flag.
       - Whether the internet profile needs the operator's exact APN. The daemon uses
         `data_apn`, else `AT+CGDCONT?`; with no APN known, the profile is left out.
       - Whether `APN_SET_IA_REQ` 0x0101 (factory sent it 1.7 ms later) is also needed.
       - Whether the IMS PDN waits for the internet DATA_ACT_CALL; factory sent the list 30 ms
         before it.
       - The meaning of the IMS_SET_CONFIG key and of its value 0x32 (`ims_mipc_key=0` if it is
         harmful).
       - Whether `onoff` must go through the tunnel.
       - The tunnel's success test (result 0, no `ERROR` in the text).
       - `IMS_STATE_IND` 0x4A01 is not registered.
    a1. **Phone result 2026-10-30** (radio off before the set-up): the IMS PDN came up by itself
       (transIntfId 202, IMS_PDN_IND aid 1 type 2 interface 202), `+CIREG: 2,1,5` about a minute
       later, internet next to it (301); no 0x8305 came. Still open:
       - Why the 0x4302 record gave no IPv4. The daemon now also reads an array-form record and
         falls back to the `+EIF: <tid>, ifup, <type>, <ipv4>, <x>` URC (layout observed once:
         what are `<type>` 1 and the last field 0?). The `-v` log of the EIF_IND TLV ids will
         tell.
       - Why `+CIREGU` does not reach the AT port. Perhaps it goes to the tunnel or needs
         `AT+EURCRPT`. The daemon polls `AT+CIREG?`.
       - Whether the modem needs the internet profile in the same list as ims. The daemon
         re-sends the whole list once the attach APN is known; `data_apn` avoids that.
    a2. **Radio after a restart**: `radio_on_at_start` sends `AT+EFUN=<radio_mask>` (or
       `AT+CFUN=1`) once per connection after the set-up; whether `+EFUN: 0` after a restart
       needs anything else is unknown. `+CME ERROR: SIM busy` (CME 14 or text "busy") is
       retried every second.
    b. **`AT+EIMS?` answers `+CME ERROR: unknown`**: `AT+EIMS=1` is not sent; if IMS does not
       start, try `ims_eims=always` and whether `+EIMS: 1` / `+EIMCFLAG: 1` appear (B15).
    c. **The two onoff forms**: the spaced `AT+EIMSPDN= "onoff",1` (observed, accepted on the AT
       port) is sent first, the plain one only after a refusal.
    d. **EIPSPI**: echoing the command's header byte (0x11) in the RSP; the meaning of `0x0101`
       and of the two address records (`0x8102` remote/any?, `0x8103` own address); the failure
       result value (1) for an exhausted or inverted range; whether the SPIs must also be
       reserved in the kernel's IPsec stack (factory reserves them; the daemon does not, as the
       modem does the IPsec itself — unverified); `0x8303`/`0x8307`/ARP commands are not
       registered and not answered.
    e. **IMS_PDN_IND**: the type value of an emergency PDN (only 2 = ims seen); the short form
       keeps existing links (only their labels are dropped); the 20-byte `0x8100` and `0x0105`
       are not used.
    f. **Unrequested interfaces**: every one is taken for IMS/emergency (only 101 seen); one
       that arrives while our `data up` waits is held until the confirm and attributed by its
       transIntfId. The IPv6 record size (`0x8107`) is assumed to start with 16 address bytes.
    g. **`ims off`** removes only our links; whether `"onoff",0` makes the modem release the PDN
       (an EIF_IND down) is unknown.
    h. **AT fallback**: `+EIMSPDN: "notify"` on the AT port is answered `"confirm",<aid>,0`
       (also for action 0) — never observed.
    i. **`<ext_info>` bit 2**: 27.007 says RTP text, the vendor description says video; the daemon
       follows 27.007.
    j. **`+EIMSPDIS` answer** (B8): not sent, only forwarded; factory P-CSCF comes with the PDN.
    k. **SIM2**: one `ims_cfg` for both stacks (factory: `0,0,0,0,1,1` on SIM2); the start-up waits
       for `AT+CFUN?` = 1.

## C. Radio, SIM, misc

17. **`AT+EFUN=0`**: is it a valid "radio off"? The daemon uses the 3GPP `AT+CFUN=4` (the observed
    start-up state) for `radio off`.
18. **Vendor commands that write IMEI / NV / calibration / lock state**: unknown on the clean
    side. The raw-AT deny list (`raw_at_deny`) holds the 3GPP lock/PIN/SIM-access commands only;
    the dirty side should name the vendor ones to add.
19. **Does `+CPIN:` come unsolicited** after `AT+EFUN`? Handled if it does.
20. **`AT+CNMA` form in PDU mode** (`AT+CNMA` vs `AT+CNMA=0`): `sms_ack_cmd`.
21. **Late responses after a command timeout**: the daemon waits `settle_ms` (200) before the
    next command and logs stray final results; whether the modem ever answers late is unknown.
