# Felicity Solar BMS BLE protocol

Reference for the Bluetooth LE interface of Felicity Solar LiFePO4 battery BMSes (16-cell, 48 V packs
such as the FLB48 series), as implemented in `main/features/bms_proto.c` (framing and decoding) and
`main/features/bms.c` (BLE session).

## Discovery

| | |
|---|---|
| Local name | `F07` or `F10` followed by the 18-digit serial number |
| Address | public |
| Manufacturer ID | `0xfffe` |
| Connectable | yes, one central at a time |

## GATT

| UUID | Handle | Properties | |
|---|---|---|---|
| `6e6f736a-4643-4d44-8fa9-0fafd005e455` | | | service |
| `49535258-184d-4bd9-bc61-20c647249616` | `0x0011` | write without response | TX: commands |
| `49535458-8341-43f4-a9d4-ec0e34729bb3` | `0x000e` | notify (CCCD `0x000f`) | RX: replies |

Handles are from BMS firmware 2.15 and may differ on other versions; discover them by UUID.

The BMS sends an ATT Exchange MTU request (MTU 247) right after connecting. The central must answer
it. Commands are 25–37 bytes, so they need an MTU above the default 23.

## Pairing

The BMS requires an authenticated, encrypted link before it accepts a subscription to RX. Without
it, writing the RX CCCD fails with ATT error `0x05` (Insufficient Authentication).

| | |
|---|---|
| Security Request from the BMS | AuthReq `0x0D`: bonding, MITM, Secure Connections |
| BMS IO capability | DisplayOnly |
| Method | Passkey entry: the central enters the BMS's fixed 6-digit passkey |
| Just Works | rejected with SMP Pairing Failed `0x03` (Authentication Requirements) |
| Wrong passkey | SMP Pairing Failed `0x04` (Confirm Value Failed) |

The passkey is not printed on the battery's labels. After bonding, later connections only need to
re-encrypt with the stored keys.

## Commands

Plain ASCII, written to TX:

| Command | Reply |
|---|---|
| `wifilocalMonitor:get dev real infor` | live data |
| `wifilocalMonitor:get dev basice infor` | device information (`basice` is the protocol's spelling) |
| `wifilocalMonitor:get Date` | `dateTime`, `timeZMin` |

## Framing

Each reply is one JSON object split over several notifications. A notification that starts with `{`
starts a frame. Append notifications until one ends with `}` and the buffer parses as JSON; a `}`
that closes a nested object can also end a notification.

Every reply has `"CommVer": 1`.

Values are integers, sometimes encoded as JSON strings, within ±`0xFFFFFFFF`. Arrays are padded
with `65535` (`0xFFFF`) in unused slots.

## Live data

Example reply: `test/fixtures/real.json` (serial numbers zeroed).

| Value | Field | Scale | Example |
|---|---|---|---|
| Pack voltage | `BattList[0][0]` | mV | `52230` → 52.23 V |
| Current | `BattList[1][0]` | 0.1 A, signed; negative while discharging | `-201` → −20.1 A |
| State of charge | `BatsocList[0][0]` | 0.01 % | `6000` → 60 % |
| Full capacity | `BatsocList[0][2]` | mAh | `300000` → 300 Ah |
| Remaining charge | `BatsocList[0][0] × BatsocList[0][2] / 10⁷` | Ah | 180 Ah |
| Cell voltages | `BatcelList[0][0..15]` | mV | 16 cells, `3271`, `3260`, … |
| Temperatures | `BtemList[0][0..7]` | 0.1 °C, signed; `32767` = no sensor | 4 sensors, `250` → 25.0 °C |
| Warning flags | `Bwarn` | bit field, 0 = none | |
| Fault flags | `Bfault` | bit field, 0 = none | |
| Highest / lowest cell | `BMaxMin` | `[[max mV, min mV], [max index, min index]]`, 0-based | `[[3271,3255],[0,11]]` |
| BMS clock | `date` | `YYYYMMDDhhmmss`, local time | |
| Serial number | `DevSN`, `wifiSN` (`F` + serial) | | |

`BatcelList[1]` and the unused slots of `BattList` are all `65535`.

### Other fields

These fields are present, but their meaning is not documented. The interpretations below are
inferred from the values and are unverified.

| Field | Example | Likely meaning |
|---|---|---|
| `Batt` | `[[52200],[-203],[null]]` | voltage (mV) and current (0.1 A), rounded |
| `Batsoc` | `[[6000,1000,300000]]` | same as `BatsocList`; `[0][1]` may be state of health in 0.1 % |
| `LVolCur`, `BLVolCu` | `[[576,480],[1500,1500]]` | charge voltage limit and discharge cut-off (0.1 V); charge and discharge current limits (0.1 A) |
| `BTemp` | `[[250,240],[256,257]]` | highest and lowest temperature (0.1 °C); second row unknown |
| `Templist` | `[[250,240],[0,1],[65535,65535],[65535,65535]]` | highest and lowest temperature, and their sensor indexes |
| `Estate`, `Bstate` | `5056` | state bit field |
| `EMSpara`, `BMSpara` | `[[1,2]]` | unknown |
| `BBfault`, `BBwarn` | `0` | secondary fault and warning flags |
| `workM`, `HtCuSt` | `0` | operating mode; heater state |
| `modID`, `Type`, `SubType` | `1`, `112`, `7300` | module ID, device type and subtype |

## Device information

Example reply: `test/fixtures/basic.txt`.

| Field | Meaning | Example |
|---|---|---|
| `DevSN` | serial number | 18 digits |
| `version` | software version | `"2.15"` |
| `M1SwVer`, `M2SwVer` | firmware versions | `524`, `5` |
| `Type`, `SubType` | device type | `112`, `7300` |

Also present: `wifiSN`, `COM`, `iotType`, `modID`, `DSwVer`, `DHwVer`, `CtHwVer`, `PwHwVer`,
`BDsVer`, `RD`, `RDT`.

## Credits

The command set and the core field layout were first published in
[aiobmsble](https://github.com/patman15/aiobmsble) (`aiobmsble/bms/felicity_bms.py`, Apache-2.0),
which `bms_proto.c` is ported from.
