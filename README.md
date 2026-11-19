# fs-bms

ESP32-S3 firmware that reads a **Felicity Solar LiFePO4 battery's BMS** over Bluetooth LE
and publishes it over Wi-Fi as a **Prometheus `/metrics`** endpoint. It also has a live web page, a
JSON API and a USB console. It updates itself from this repo's GitHub Releases.

- **BLE**: every 30 s (configurable) it connects to the BMS, reads the live data and disconnects.
  The BMS accepts one connection at a time, so the FSolar app can connect between polls.
- **Pairing**: the BMS requires passkey pairing. Set the passkey once with `bms pin`; it is stored
  in NVS, never in the firmware image. Protocol details: [docs/felicity-protocol.md](docs/felicity-protocol.md).
- **Home page** (`http://<board>/`): SoC, power, voltage, current, the 16 cells (lowest/highest
  outlined), temperatures, warnings, and 2-hour SoC/power/cell-delta charts. Board stats are under
  Settings → System.
- **Exposed**: per-cell voltages, cell min/max/delta, pack voltage/current/power, SoC, remaining and
  full Ah, temperatures, warning and fault codes, BLE RSSI, poll health.

## Requirements

- ESP32-S3 with 16 MB flash and 8 MB PSRAM (N16R8), within BLE range of the battery
- ESP-IDF v6.1
- A Felicity Solar LiFePO4 battery with BLE (advertises as `F07…` or `F10…`) and its pairing passkey

## Build and flash over USB

```sh
. ~/esp/esp-idf/export.sh
cp sdkconfig.secrets.example sdkconfig.secrets   # Wi-Fi password, git-ignored
idf.py build flash monitor
```

The Wi-Fi password in `sdkconfig.secrets` only seeds NVS on first boot. Release builds don't
contain it, and OTA keeps what's in NVS. The same goes for the BMS passkey, which is never in the build.

## First setup (USB console)

```
bms> wifi MyNetwork mypassword
bms> name fs-bms             # DHCP/mDNS hostname
bms> passwd s3cret           # web admin password (protects Settings, OTA, POST /api/bms)
bms> bms pin <passkey>       # the battery's BLE pairing passkey
bms> bms                     # status + last sample
```

| Command | |
|---|---|
| `bms` / `bms status` | state, last error, poll counts, last sample |
| `bms raw` | the last raw JSON replies (live data and device info), as the pack sent them |
| `bms poll` | poll now |
| `bms interval <s>` | poll interval, 10–3600 s (default 30) |
| `bms addr <AA:BB:CC:DD:EE:FF>` / `bms addr -` | pin one pack / take the first named `F07*`/`F10*` |
| `bms pin <passkey>` / `bms pin -` | BLE pairing passkey (re-pairs on the next poll) |
| `bms pause <min>` | stop polling, e.g. to use the FSolar app; `bms pause 0` resumes. Not kept across reboots |

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
| `bms_info{name,address,serial,model,sw_version,fw_version}` | always 1 |
| `bms_voltage_volts`, `bms_current_amperes`, `bms_power_watts` | current > 0 while charging |
| `bms_state_of_charge_percent` | |
| `bms_remaining_capacity_amp_hours`, `bms_full_capacity_amp_hours` | |
| `bms_cell_voltage_volts{cell="1".."N"}`, `bms_cell_count` | |
| `bms_cell_voltage_min_volts`, `_max_volts`, `_delta_volts` | delta = max − min |
| `bms_temperature_celsius{sensor="1".."N"}` | sensors reporting `0x7FFF` are left out |
| `bms_warning_code`, `bms_fault_code`, `bms_problem_code` | `Bwarn`, `Bfault`, and their sum |
| `bms_rssi_dbm`, `bms_poll_interval_seconds` | |

Value metrics are left out once the last good sample is older than 3 poll intervals (at least 3 min).
Graphs then show a gap instead of a flat line. Alert on `bms_up == 0` for a few minutes, or on
`absent(bms_voltage_volts)`.

## Releasing a new version

```sh
git tag -a v0.2.0 -m "Short description shown on the board"
git push origin v0.2.0
```

The `Release firmware` workflow publishes `fs-bms.bin` and `version.json`. Boards install it from
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
| POST | `/api/bms` | (admin) any of `{interval_s, addr, pin, pause_min, poll: true}` |
| GET | `/api/info` | firmware, board and Wi-Fi stats |
| GET | `/api/ota`, POST `/api/ota/check`, `/api/ota/update` | OTA (admin for POST) |
| POST | `/api/reboot` | (admin) |
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
| `main/web/features/bms.js` | the home page's Battery card and Settings → Battery BMS |
| `main/core/` | Wi-Fi + mDNS, NVS settings, admin auth, log buffer, OTA, HTTP + WebSocket, console |
| `docs/felicity-protocol.md` | BLE protocol reference: GATT, pairing, commands, field layout |

## License

Apache License 2.0, see [LICENSE](LICENSE). The BMS protocol decoder is ported from
[aiobmsble](https://github.com/patman15/aiobmsble) (Apache-2.0); see [NOTICE](NOTICE).
