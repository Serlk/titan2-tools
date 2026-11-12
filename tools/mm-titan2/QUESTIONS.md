<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) 2026 serlk <22211586+Serlk@users.noreply.github.com> -->
# mtk-ccci plugin: open protocol questions

Open questions about the modem's behaviour that the plugin cannot answer by itself. Answers
should be observable wire facts (captures, logs), not code or pseudocode.

1. **IMS reconciliation after a userspace restart.** Which documented request enumerates
   active IMS PDNs together with transIntfId/address, or asks the modem to replay their
   indications? A preserved registration alone does not prove the new client owns its links.
   Current implementation receives fresh EIF/IMS_PDN events and does not guess fixed link ids.
2. **EIPSPI lifecycle.** Does this firmware require AP-kernel SPI reservations, and what
   event frees an allocation? Confirm negative result coding, retransmitted CMD semantics,
   and whether command header byte 8=0x11 can be scoped to one SIM. Current code echoes the
   complete available routing byte/txid and never reuses session allocations.
3. **IPv6 EIF record.** Establish record lengths, prefix/gateway fields and interface-id
   semantics for this device; indicate where the IPv6 DNS and host-SLAAC identifier arrive.
   Do not infer the raw-IP IPv6 setup from one IPv4 trace.
4. **Authentication.** Establish the mapping between allowed-auth settings and DATA_ACT_CALL
   TLV 0x0105, independently of the APN profile record's byte 254. Current data activation
   preserves the previously exercised value 0; PAP/CHAP are not claimed supported.
5. **IMS feature flags and APN policy on SIM2/other carriers.** Which values are modem
   requirements and which are carrier policy? Current feature flags are 1,0,0,0,1,1, the
   successful SIM1 setting; the profile list uses captured record fields marked unverified.
6. **Activation cancellation/timeout.** Can the AP enumerate or cancel a DATA_ACT_CALL whose
   CNF was lost, without deactivating a modem-initiated/default/IMS context? The new bearer
   drains a CNF after user cancellation and rolls back by its returned aid; an unreceived
   confirm leaves ownership unknown.
7. **Data after a primary-SIM reprobe (resolved for the reproduced APN failure).** A warm
   switch back to SIM1 produced DATA_ACT_CALL result 1316568 despite CGATT=1 and IMS registration.
   Teardown had removed CID 0; discovery incorrectly chose remaining CID 1=ims for the
   default profile. Explicit APN restoration recovered data without resetting the radio.
   The plugin now discovers only CID 0 and confirms a profile-list update using the actual
   connection APN before activation. Two round trips, warm restart and one radio low/enable
   cycle passed IPv4 WWAN checks. The general numeric meaning of 1316568 remains unknown;
   it must not be mapped globally to "wrong APN". EDALLOW/EDATASIM were not needed.
8. **SMS notifications on this MIPC build.** With CNMI=2,1,2,1,0 and CMGF=0, an unread
   SMS-DELIVER was present in SM while MM's list remained empty and no physical CMTI
   appeared in the captured log. Establish native SMS indication IDs/registration and
   their relationship to UART2/SYS_AT routing. Do not guess payloads from message names.
   The offline candidate uses standard stored-message scans as recovery, not native
   MIPC SMS support. Direct CMT, acknowledgement and second-SIM messaging remain open.
9. **Cold SMS readiness and abort confirmation.** On the phone, CPMS and CMGF/CNMI failed
   during startup, leaving SMS default storage UNKNOWN. Generic create_sms asserts on
   that state: this reproduces SIGABRT locally, and known storage avoids the abort.
   The recorded phone process died with signal 6 before any create-SMS log, but its
   stderr was /dev/null and core limit zero. Capture stderr/stack on further testing
   before claiming this was the exact original abort or that all call/SMS crashes are fixed.
