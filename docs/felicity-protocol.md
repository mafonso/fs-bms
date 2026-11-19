# Felicity Solar BMS BLE protocol

Reference for the Bluetooth LE interface of Felicity Solar LiFePO4 batteries (16S, 48 V packs such as
the FLB48 series), as implemented in `main/features/bms_proto.c` (framing and decoding) and
`main/features/bms.c` (BLE session).

Each pack has its own BLE radio. Packs can be wired in parallel as a bank; every pack then reports its
own measurements plus a copy of the bank's aggregates.

## Discovery

| | |
|---|---|
| Local name | `F07` or `F10` followed by the 18-digit serial number |
| Address | public |
| Manufacturer ID | `0xfffe` |
| Connections | one central at a time |

## GATT

| UUID | Handle¹ | Properties | |
|---|---|---|---|
| `6e6f736a-4643-4d44-8fa9-0fafd005e455` | | | service |
| `49535258-184d-4bd9-bc61-20c647249616` | `0x0011` | write without response | TX: commands |
| `49535458-8341-43f4-a9d4-ec0e34729bb3` | `0x000e` | notify (CCCD `0x000f`) | RX: replies |

¹ Seen with firmware 2.15; discover by UUID.

The BMS sends an ATT Exchange MTU request (MTU 247) right after connecting; the central must answer
it. Commands are 25–37 bytes, so they need an MTU above the default 23.

## Pairing

The BMS accepts a subscription to RX only over an authenticated, encrypted link. Without one,
writing the RX CCCD fails with ATT error `0x05` (Insufficient Authentication).

| | |
|---|---|
| Security Request from the BMS | AuthReq `0x0D`: bonding, MITM, Secure Connections |
| BMS IO capability | DisplayOnly |
| Method | passkey entry: the central (KeyboardOnly) enters the BMS's fixed passkey |
| Default passkey | `123456` |
| Just Works | rejected: SMP Pairing Failed `0x03` (Authentication Requirements) |
| Wrong passkey | SMP Pairing Failed `0x04` (Confirm Value Failed) |

After bonding, later connections re-encrypt with the stored keys. A BLE proxy that cannot bond (such
as an ESPHome Bluetooth proxy) cannot read the BMS.

## Commands

Plain ASCII, written to TX. One command at a time; wait for its reply before sending the next.

| Command | Reply |
|---|---|
| `wifilocalMonitor:get dev real infor` | live data |
| `wifilocalMonitor:get dev basice infor` | device information (`basice` is the protocol's spelling) |
| `wifilocalMonitor:get Date` | `dateTime`, `timeZMin` |

## Framing

Each reply is one JSON object split over several notifications. A notification that starts with `{`
starts a frame. Append notifications until one ends with `}` and the buffer parses as JSON; a `}`
that closes a nested object can also end a notification.

Every reply has `"CommVer": 1`. Values are integers, sometimes encoded as JSON strings, within
±`0xFFFFFFFF`.

| Value | Meaning |
|---|---|
| `65535`, `-1` | empty slot (the other pack's position in a per-pack array, or an absent version field) |
| `32767` | temperature probe not fitted |
| `null` | unused |

Right after a connection the BMS can answer with a well-formed frame whose values are all still zero
(0 V, 0 %, 0 °C). Discard frames whose pack voltage is outside 10–70 V and ask again.

## Common fields

Present in every reply.

| Field | Meaning |
|---|---|
| `CommVer` | protocol version, `1` |
| `wifiSN` | `F` + serial number |
| `DevSN` | serial number (18 digits) |
| `modID` | the pack's index within the bank (`1`, `2`, …) |
| `Type`, `SubType` | model codes (e.g. `112`, `7300`) |

## Live data: `get dev real infor`

Example: `test/fixtures/real.json` (serial numbers zeroed). Arrays indexed `[row][0]` hold this pack's
value; `[row][1]` is the other pack's slot and reads `65535` or `-1`.

### This pack

| Value | Field | Scale | Example |
|---|---|---|---|
| Pack voltage | `BattList[0][0]` | mV | `52230` → 52.23 V |
| Current | `BattList[1][0]` | 0.1 A, signed; negative = discharging | `-201` → −20.1 A |
| State of charge | `BatsocList[0][0]` | 0.01 % | `6000` → 60 % |
| Cell voltages | `BatcelList[0][0..15]` | mV | `3271`, `3260`, … |
| Highest / lowest cell | `BMaxMin` | `[[max mV, min mV], [max index, min index]]`, 0-based | `[[3271,3255],[0,11]]` |
| Temperatures | `BtemList[0][0..7]` | 0.1 °C, signed; 4 probes fitted, `32767` = none | `250` → 25.0 °C |
| Charge voltage limit | `BLVolCu[0][0]` | 0.1 V | `576` → 57.6 V (3.6 V/cell) |
| Discharge voltage limit | `BLVolCu[0][1]` | 0.1 V | `480` → 48.0 V (3.0 V/cell) |
| Charge current limit | `BLVolCu[1][0]` | 0.1 A; drops to 0 when full | `1500` → 150 A |
| Discharge current limit | `BLVolCu[1][1]` | 0.1 A | `1500` → 150 A |
| Warning word | `BBwarn` | bit field, 0 = none | `4` at top of charge when one cell runs ahead |
| Fault word | `BBfault` | bit field, 0 = none | |
| BMS clock | `date` | `YYYYMMDDhhmmss`, local time | |

`BatcelList[1]` (the other pack's cells) is all `65535`.

### Bank

Every pack in a parallel bank reports the same values for these fields. With a single pack they
equal the pack's own values.

| Value | Field | Scale |
|---|---|---|
| Bank voltage | `Batt[0][0]` | mV |
| Bank current | `Batt[1][0]` | 0.1 A, sum of all packs |
| Bank state of charge | `Batsoc[0][0]` | 0.01 % |
| Bank voltage limits | `LVolCur[0][0]`, `[0][1]` | 0.1 V (charge, discharge) |
| Bank current limits | `LVolCur[1][0]`, `[1][1]` | 0.1 A, sum of all packs (charge, discharge) |
| Bank warning word | `Bwarn` | OR of all packs' `BBwarn` |
| Bank fault word | `Bfault` | OR of all packs' `BBfault` |

The field names do not indicate the scope: `BattList` is per pack and `Batt` is the bank, while
`BLVolCu` is per pack and `LVolCur` is the bank.

### Partly decoded

| Field | Example | Meaning |
|---|---|---|
| `BatsocList[0][1]`, `Batsoc[0][1]` | `1000` | likely state of health, 0.1 % |
| `BatsocList[0][2]` | `300000` | capacity in mAh; does not always match the nameplate (a 314 Ah pack reports `350000`). `Batsoc[0][2]` is the bank total |
| `BTemp[0]` | `[250,240]` | highest and lowest pack temperature, 0.1 °C |
| `BTemp[1]` | `[256,257]` | unknown; takes a few discrete values (`256`, `257`, `259`, `512`, `513`) |
| `Templist` | `[[250,240],[0,1],[65535,65535],[65535,65535]]` | four two-value groups; group 0 is the highest/lowest probe temperature |
| `Estate`, `Bstate` | `5056` | bank and pack state bit fields |
| `workM` | `0` | work mode |
| `HtCuSt` | `0` | heater current state (`0` = not heating) |
| `EMSpara`, `BMSpara` | `[[1,2]]` | configuration pair; the second value may be the heater request current (A) |

## Device information: `get dev basice infor`

Example: `test/fixtures/basic.txt`.

| Field | Meaning | Example |
|---|---|---|
| `version` | Wi-Fi/BLE module firmware ("Version" in the FSolar app) | `"2.15"` |
| `M1SwVer` | BMS firmware ("Master Version") | `524` |
| `M2SwVer` | bootloader firmware ("IAP Version") | `5` |
| `DSwVer` | display firmware, `65535` = no display | `65535` |
| `DHwVer`, `CtHwVer`, `PwHwVer`, `BDsVer` | hardware/board versions, `65535` = absent | |
| `COM`, `iotType`, `RD`, `RDT` | unknown | `3`, `3`, `1`, `18` |

## Credits

The command set and core field layout were first published in
[aiobmsble](https://github.com/patman15/aiobmsble) (`aiobmsble/bms/felicity_bms.py`), which
`bms_proto.c` is ported from. The per-pack/bank split, the limits and the app version names follow
[esphome-felicity-bms](https://github.com/andrein/esphome-felicity-bms) (`PROTOCOL.md`). Both are
Apache-2.0.
