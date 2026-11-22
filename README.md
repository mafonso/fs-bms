# fs-bms

ESP32-S3/ESP32-C3 firmware that reads a **Felicity Solar LiFePO4 battery's BMS** over Bluetooth LE
and publishes it over Wi-Fi as a **Prometheus `/metrics`** endpoint. It also has a live web page, a
JSON API and a USB console. It updates itself from this repo's GitHub Releases.

- **BLE**: every 30 s (configurable) it connects to the BMS, reads the live data and disconnects.
  The BMS accepts one connection at a time, so the FSolar app can connect between polls.
- **Pairing**: the BMS requires passkey pairing. The firmware uses Felicity's default passkey,
  `123456`; set another with `bms pin` (stored in NVS). Protocol details:
  [docs/felicity-protocol.md](docs/felicity-protocol.md).
- **Home page** (`http://<board>/`): SoC, power, voltage, current, the 16 cells (lowest/highest
  outlined), temperatures, warnings, and 2-hour SoC/power/cell-delta charts. Board stats are under
  Settings → System.
- **Exposed**: per-cell voltages, cell min/max/delta, pack voltage/current/power, SoC, SoH, remaining
  and full Ah, temperatures, charge/discharge limits, warning and fault words (pack and bank), BLE
  RSSI, poll health.

## Requirements

- One of these boards, within BLE range of the battery:
  - ESP32-S3 with 16 MB flash and 8 MB PSRAM (N16R8): the default build.
  - ESP32-C3 with 4 MB flash (no PSRAM): same features, except the home page charts, which need PSRAM
    for their history; fewer simultaneous browser connections (8). Wi-Fi transmit power is capped
    at 10 dBm, because many small C3 boards can't join an access point at full power.
- ESP-IDF v6.1
- A Felicity Solar LiFePO4 battery with BLE (advertises as `F07…` or `F10…`)

In a parallel bank every pack has its own BLE radio. One board reads one pack (pin it with
`bms addr`); each pack also reports the bank's totals and flags, exported as the `bms_bank_*` metrics.

## Build and flash over USB

```sh
. ~/esp/esp-idf/export.sh
cp sdkconfig.secrets.example sdkconfig.secrets   # Wi-Fi password, git-ignored
idf.py build flash monitor
```

That builds for the ESP32-S3. For an ESP32-C3, use a separate build directory:

```sh
idf.py -B build-c3 -D SDKCONFIG=build-c3/sdkconfig set-target esp32c3
idf.py -B build-c3 -D SDKCONFIG=build-c3/sdkconfig build flash monitor
```

A blank board reboots continuously and can drop off USB before flashing starts: hold BOOT (GPIO0 on
the S3, GPIO9 on the C3) while plugging it in for the first flash.

## Flash a release (no build)

Each release has a complete image per chip, `fs-bms-esp32s3-factory.bin` and
`fs-bms-esp32c3-factory.bin` (bootloader, partition table and app). Connect the board's USB port
(hold BOOT while plugging in a blank board), then either:

- **Browser** (Chrome or Edge): open <https://espressif.github.io/esptool-js/>, Connect, choose the
  board's port, add the file at address `0x0`, Program.
- **esptool** (`pip install esptool`):
  ```sh
  esptool write-flash 0x0 fs-bms-esp32c3-factory.bin   # macOS port: /dev/cu.usbmodem*, add --port if several
  ```

This erases nothing else: a board that already ran fs-bms keeps its Wi-Fi, passkey and BLE bond. Then
unplug and replug it (or press RESET), and set it up as below. Later versions arrive by OTA.

The Wi-Fi password in `sdkconfig.secrets` only seeds NVS on first boot. Release builds don't
contain it, and OTA keeps what's in NVS, including a passkey set with `bms pin`.

## First setup

**From a phone.** With no Wi-Fi network saved, the board opens a setup hotspot named after its
hostname (`fs-bms-xxxxxx`). Join it; the setup page opens by itself, or browse to
`http://192.168.4.1/`.

1. Settings → Network: pick your Wi-Fi network. The board joins it, shows its new address, and turns
   the hotspot off a minute later.
2. Settings → Security: set an admin password.
3. Settings → Battery: **Find batteries**, choose yours (or leave **Automatic**), and
   Save.

The hotspot also comes back if the saved network is unreachable for 3 minutes
(`CONFIG_APP_SETUP_AP_AFTER_S`). It is open unless `CONFIG_APP_SETUP_AP_PASSWORD` is set, so set an
admin password.

**From the USB console.**

```
bms> wifi MyNetwork mypassword
bms> name fs-bms             # DHCP/mDNS hostname
bms> passwd s3cret           # web admin password (protects Settings, OTA, POST /api/bms)
bms> bms scan                # list batteries in range
bms> bms addr <address>      # optional: pin one; otherwise the first found is used
bms> bms pin <passkey>       # only if the battery doesn't use the default 123456
bms> bms                     # status + last sample
```

| Command | |
|---|---|
| `bms` / `bms status` | state, last error, poll counts, last sample |
| `bms raw` | the last raw JSON replies (live data and device info), as the pack sent them |
| `bms scan` | search for 8 s and list the batteries in range, strongest first |
| `bms poll` | poll now |
| `bms interval <s>` | poll interval, 10–3600 s (default 30) |
| `bms addr <AA:BB:CC:DD:EE:FF>` / `bms addr -` | pin one pack / take the first named `F07*`/`F10*` |
| `bms pin <passkey>` / `bms pin -` | BLE pairing passkey / back to the default `123456` |
| `bms pause <min>` | stop polling, e.g. to use the FSolar app; `bms pause 0` resumes. Not kept across reboots |
| `bms off` / `bms on` | turn polling off or on, kept across reboots (also Settings → Battery) |
| `ap [on\|off]` | show or switch the setup hotspot |
| `factory-reset yes` | erase all settings and reboot (see below) |

A battery that is connected to the FSolar app doesn't advertise, so it won't show up in a search.

The pack remembers a limited number of paired devices (phones included). When it is full, a new board
pairs but every poll fails with `subscribe failed (273)` (ATT "Insufficient Resources"). Turn polling
off (`bms off`) on boards that don't need the pack, so they don't use a pairing; freeing one on the
pack means resetting its Bluetooth.

## Factory reset

Settings → System, `factory-reset yes` on the console, or hold the BOOT button for 10 s. It erases
the Wi-Fi network, hostname, admin password, BMS settings (passkey, address, interval) and the BLE
pairing, then reboots into the setup hotspot. The firmware stays.

The first three successful polls after boot log the raw JSON at INFO on the serial console. The web
log viewer cuts lines at 200 chars, so use `bms raw` or `GET /api/bms/raw` for the whole frame.

## Prometheus

`GET /metrics` (no auth). A Prometheus scrape job, for example:

```yaml
- job_name: fs-bms
  scrape_interval: 30s
  static_configs:
    - targets: ["fs-bms.local:80"]   # or the board's IP
```

| Metric | |
|---|---|
| `bms_up` | 1 if the last poll succeeded |
| `bms_polls_total{result="ok"\|"error"}` | counter |
| `bms_sample_age_seconds` | age of the last good sample |
| `bms_info{name,address,serial,model,version,master_version,iap_version}` | always 1 |
| `bms_voltage_volts`, `bms_current_amperes`, `bms_power_watts` | current > 0 while charging |
| `bms_state_of_charge_percent`, `bms_state_of_health_percent` | SoH is inferred |
| `bms_remaining_capacity_amp_hours`, `bms_full_capacity_amp_hours` | capacity as the BMS reports it; may differ from the nameplate |
| `bms_cell_voltage_volts{cell="1".."N"}`, `bms_cell_count` | |
| `bms_cell_voltage_min_volts`, `_max_volts`, `_delta_volts` | delta = max − min |
| `bms_temperature_celsius{sensor="1".."N"}` | sensors reporting `0x7FFF` are left out |
| `bms_charge_voltage_limit_volts`, `bms_discharge_voltage_limit_volts` | this pack's CVL / DVL |
| `bms_charge_current_limit_amperes`, `bms_discharge_current_limit_amperes` | this pack's CCL / DCL; CCL drops to 0 when full |
| `bms_warning_code`, `bms_fault_code` | this pack's warning and fault words (`BBwarn`, `BBfault`), 0 = none |
| `bms_bank_warning_code`, `bms_bank_fault_code` | the parallel bank's (`Bwarn`, `Bfault`: OR of all packs) |
| `bms_problem_code` | `Bwarn + Bfault` |
| `bms_rssi_dbm`, `bms_poll_interval_seconds` | |

Value metrics are left out once the last good sample is older than 3 poll intervals (at least 3 min).
Graphs then show a gap instead of a flat line. Alert on `bms_up == 0` for a few minutes, or on
`absent(bms_voltage_volts)`.

## Releasing a new version

```sh
git tag -a v0.2.0 -m "Short description shown on the board"
git push origin v0.2.0
```

The `Release firmware` workflow publishes `fs-bms.bin` and `version.json` for the ESP32-S3, and
`fs-bms-esp32c3.bin` and `version-esp32c3.json` for the ESP32-C3; each board fetches its own chip's. Boards install it from
Settings → **Check for updates** (or `ota check` / `ota update`). A new image that can't reach Wi-Fi
within 60 s rolls back on its own. Boards look for updates at `CONFIG_APP_OTA_MANIFEST_URL`; a fork
should point it at its own releases.

## HTTP API

| Method | Path | |
|---|---|---|
| GET | `/metrics` | Prometheus text format |
| GET | `/api/bms` | status and last sample as JSON (same as the `bms` WebSocket topic) |
| GET | `/api/bms/raw` | `{real, basic}`: the last raw replies from the pack |
| GET | `/api/bms/history` | `{interval_s, len, soc[], power[], delta_mv[]}`: the last 240 good samples (RAM only, lost on reboot) |
| POST | `/api/bms/scan` | start an 8 s search for batteries |
| GET | `/api/bms/scan` | `{state: idle\|searching\|done, results: [{name, addr, rssi, pinned, in_use}]}` |
| POST | `/api/bms` | (admin) any of `{enabled, interval_s, addr, pin, pause_min, poll: true}` |
| GET | `/api/info` | firmware, board and Wi-Fi stats |
| GET | `/api/ota`, POST `/api/ota/check`, `/api/ota/update` | OTA (admin for POST) |
| POST | `/api/reboot` | (admin) |
| POST | `/api/factory-reset` | (admin) erase all settings and reboot |
| GET/POST | `/api/settings/wifi`, `/api/settings/name`, `/api/settings/password` | Wi-Fi (tried for 30 s before saving), hostname, admin password (admin for POST) |
| GET | `/api/logs?after=<seq>` | (admin) buffered log lines |

POST requests must send an `X-Requested-With` header. Admin routes need the admin password once one
is set. `/ws` carries `{"t": topic, "d": data}`: `bms` (after every poll), `sysinfo`, `temp`.

## Tests

`test/run.sh` builds `main/features/bms_proto.c` on the host with ASan/UBSan and checks the framing
and decoding against sample replies, plus every capture in `test/fixtures/*.json`. Run
`idf.py build` once first; it downloads cJSON.

## Layout

| | |
|---|---|
| `main/features/bms.c` | BLE session (NimBLE central, pairing), polling task, `/metrics`, `/api/bms`, `bms` command |
| `main/features/bms_proto.c` | Felicity protocol: JSON reassembly and decoding, no BLE (host-tested) |
| `main/web/features/bms.js` | the home page's Battery card and Settings → Battery |
| `main/features/factory_reset.c`, `button.c` | factory reset (also on a 10 s BOOT-button hold) |
| `main/core/` | Wi-Fi + mDNS + setup hotspot with captive DNS, NVS settings, admin auth, log buffer, OTA, HTTP + WebSocket, console |
| `docs/felicity-protocol.md` | BLE protocol reference: GATT, pairing, commands, field layout |

## License

Apache License 2.0, see [LICENSE](LICENSE). The BMS protocol decoder is ported from
[aiobmsble](https://github.com/patman15/aiobmsble), and the field reference draws on
[esphome-felicity-bms](https://github.com/andrein/esphome-felicity-bms) (both Apache-2.0); see
[NOTICE](NOTICE).
