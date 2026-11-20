# fs-bms

ESP-IDF v6.1 firmware for an ESP32-S3 N16R8 (16 MB flash, 8 MB PSRAM) that reads a Felicity Solar
LiFePO4 BMS over BLE and serves it as Prometheus `/metrics`, with a web UI, WebSocket hub,
GitHub-Releases OTA and a USB console.

## BMS

- Protocol reference: [docs/felicity-protocol.md](docs/felicity-protocol.md). Keep it a current
  reference (no change history); update it when a reply shows something new.
- `main/features/bms_proto.c` must stay free of BLE/IDF calls: `test/run.sh` builds it on the host.
  Run it after any change there; add real captures to `test/fixtures/`.
- The BMS requires **passkey pairing** (DisplayOnly, fixed passkey). The default, 123456, is
  `CONFIG_APP_BMS_DEFAULT_PASSKEY`; a different one is set at runtime with `bms pin` (NVS). Never
  commit a user's own passkey.
- **Hold one BLE connection at a time**: one session per poll, then disconnect, so the FSolar app can
  connect in between. Don't keep the link open.
- Don't keep polling with a wrong passkey: `bms pause <min>` stops the attempts.
- Report this pack's flags (`BBwarn`/`BBfault`, `BattList`, `BLVolCu`) as the pack's values; `Bwarn`,
  `Batt`, `LVolCur` are bank aggregates. See the protocol reference.
- Opening the serial port can reset the board, and `bms pause` is RAM-only. Set the port's DTR/RTS
  low before opening it.
- Keep the NimBLE peripheral role enabled: it carries the ATT server, and the pack's MTU request must
  be answered.

## Build, flash, monitor

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py flash monitor     # Ctrl-] quits monitor
```

- If a build dir predates a change to `sdkconfig.defaults`, delete `sdkconfig` (and rerun the build) so the
  new defaults apply. Missing WebSocket support stops the build with an `#error` saying exactly this.
- If after flashing the board sits in the ROM bootloader (no app output), reset it into the app:
  `esptool --port <port> --before no-reset --after watchdog-reset chip-id`
- Never use GPIO19/20, TinyUSB or deep sleep in dev firmware: they break USB-Serial/JTAG, which the
  console and auto-reset flashing depend on.
- Secrets (Wi-Fi password) go only in the git-ignored `sdkconfig.secrets`
  (copy `sdkconfig.secrets.example`). Never put them in `sdkconfig.defaults` or source.
- CI (`.github/workflows/build.yml`) builds every PR and push to `main`. Builds must have no
  warnings from our sources.

## Board access

- The board reports over Wi-Fi: `curl http://<board-ip>/api/info`, `/api/bms`, `/metrics`, and
  Settings → Logs (or `/api/logs`) when nobody is on the USB console. The console `status` command
  shows the IP.
- Hangs that only some clients see: check the router's IPS log first. Some block clients that repeat
  `Authorization: Basic admin:…`, which is why the page uses a session token.
- The board keeps Wi-Fi credentials, the BMS passkey and the BLE bond in NVS across flashes and OTA.
- **Setup hotspot** (`wifi.c`, `captive_dns.c`): with no network saved, or the saved one unreachable for
  `CONFIG_APP_SETUP_AP_AFTER_S` (180 s), the board also runs an access point named after its hostname at
  `http://192.168.4.1/` with a captive-portal DNS. It goes off a minute after the station is back on
  Wi-Fi. Console: `ap [on|off]`. BLE polling keeps running while it is on.
- **Factory reset** (`features/factory_reset.c`) erases NVS and reboots: Settings → System, console
  `factory-reset yes`, or hold BOOT for 10 s (`button.c` posts `APP_EVENT_BUTTON`). NVS also holds the
  BMS settings and the NimBLE bond, so the board pairs with the battery again afterwards. Getting the
  board back on Wi-Fi then needs a phone (or `sdkconfig.secrets` and a USB flash).
- `main/core/` and the web shell are project-independent infrastructure; keep BMS-specific code in
  `features/` and `web/features/`.

## Releasing

```sh
git tag -a vX.Y.Z -m "One-line notes shown on the board"
git push origin vX.Y.Z
```

`release.yml` builds and publishes `fs-bms.bin` and `version.json`; boards install it from
Settings → Check for updates (or `ota check` / `ota update` on the console). A new image that can't
reach Wi-Fi within 60 s rolls back on its own.

## Layout

| Path | |
|---|---|
| `main/app_main.c` | boot order: settings → Wi-Fi → features init → web → feature routes → console → OTA confirm |
| `main/core/` | shared infrastructure: `wifi` (station + setup hotspot), `captive_dns`, `settings` (NVS), `auth` (admin password), `logbuf` (log ring buffer), `ota`, `web` (HTTP + static files), `web_settings` (settings/admin API), `web_ws` (WebSocket hub), `console_cmds`, `app_events` |
| `main/features/` | one `.c` per feature, plus the registry `features.c`: `bms` (+ `bms_proto`), `button`, `factory_reset`, `sysinfo`, `temp` |
| `main/web/` | frontend, embedded as-is: `index.html`, `app.css`, `app.js` (shell), `settings.js`, `chart.js` (live line chart for cards) |
| `main/web/features/` | one ES module per feature UI, loaded automatically |
| `main/web_assets.cmake` | embeds `main/web/**` and generates the file table and `/features.json` |

The frontend is plain ES modules: no build step, no CDN, works offline. Every file under `main/web/`
is served at its path (`main/web/features/bms.js` → `/features/bms.js`) with `Cache-Control: no-cache`.

## How to add a feature

Copy `main/features/sysinfo.c` (every C hook) and `main/web/features/bms.js` (card + Settings section).
The home page shows only the Battery card; board stats live under Settings → System.

1. **C file** `main/features/<name>.c` defining
   ```c
   const feature_t <name>_feature = {
       .name = "<name>",
       .init = <name>_init,                     // drivers, tasks, web_ws_on(), event handlers
       .register_routes = <name>_routes,        // optional
       .register_console = <name>_console,      // optional
   };
   ```
   Any `.c` in `main/features/` is compiled automatically. If you need IDF components that `main`
   doesn't require yet, add them to `REQUIRES` in `main/CMakeLists.txt`, and managed components to
   `main/idf_component.yml`. Put a `<name>.h` next to it only if other code calls into the feature.
2. **Registry**: add one `X(<name>)` line to `FEATURE_LIST` in `main/features/features.c`, in
   alphabetical order.
3. **Routes** (in `register_routes`), always via `web_register_routes()` so the static-file wildcard
   stays last:
   ```c
   static const httpd_uri_t routes[] = {
       {.uri = "/api/<name>", .method = HTTP_GET,  .handler = get_handler},
       {.uri = "/api/<name>", .method = HTTP_POST, .handler = post_handler},
   };
   web_register_routes(routes, sizeof(routes) / sizeof(routes[0]));
   ```
   In POST handlers: `if (!web_check_csrf(req)) return ESP_OK;` (add `|| !web_require_admin(req)` from
   `auth.h` for anything that changes configuration), then
   `cJSON *body = web_read_json(req, 512); if (!body) return ESP_OK;`, and reply with
   `web_send_json(req, json)` (takes ownership).
4. **Live data** to browsers: `web_ws_broadcast("<name>", cJSON_data)` (takes ownership, any task,
   never blocks, drops when busy). Skip sampling when `web_ws_client_count() == 0`.
   **From browsers**: `web_ws_on("<name>", cb)` in init; `cb(cJSON *d)` runs on the HTTP task and
   borrows `d`.
5. **Console**: in `register_console`, call `esp_console_cmd_register()`. Commands show in `help`.
6. **Events between features**: add an ID to `app_event_id_t` in `core/app_events.h`, post with
   `app_event_post(APP_EVENT_X, &data, sizeof(data))`, subscribe with
   `esp_event_handler_register(APP_EVENT, APP_EVENT_X, handler, NULL)` in init.
   Example: `bms.c` posts `APP_EVENT_BMS_POLLED` after every poll; a feature that reacts to battery
   data subscribes to it and reads `bms_get_sample()` (`bms.h`).
7. **UI** (optional): `main/web/features/<name>.js` is picked up by the build and loaded by the page:
   ```js
   export default function (App) {
     App.card({ id: '<name>', title: 'Name', order: 50, span: 1, mount(el, App) {  // span: columns of 3
       el.innerHTML = '...';
       App.on('<name>', d => { /* update el */ });       // web_ws_broadcast("<name>", ...)
       App.on('$open', () => App.send('<name>'));       // re-sync after (re)connect
       App.send('<name>');
     } });
   }
   ```
   `App.api(path, {method, body})` does fetch with the CSRF header and JSON in/out, throwing
   `Error(text)` with `.status` on HTTP errors. On a 401 it asks for the admin password in its own
   dialog and retries, so a 401 that reaches you means the user cancelled. It signs in with a session
   token (`X-Admin-Token`), never per-request Basic auth: router IPSes (UniFi etc.) block clients that
   send `Authorization: Basic admin:…` repeatedly. Cards are mounted once and never unmounted. The main page is a 3-column grid (one column on phones);
   the Battery card (`order: 5`, full width) comes first. Pick `order`/`span` to fit.
   For a live chart, `import { lineChart } from '/chart.js'`. Use the CSS classes in
   `app.css` (`card`, `row`, `msg`, `dl`, `button.primary`, `meter`) and its colour variables.
8. **Settings** (optional): the main page is for using the board; configuration goes on the
   Settings page (cog icon), which has Back/Close and a Save/Cancel bar. The page is grouped into
   categories (`CATEGORIES` in `app.js`): Battery (the BMS connection; opens first), Network,
   System (firmware, hardware, health, logs) and Security, one shown at a time. Sections go under
   Battery by default; set `category` for one that belongs elsewhere. Add a section from the same module:
   ```js
   App.settings({ id: '<name>', title: 'Name', order: 50,   // order within the category
     mount(el, ctx) {                                        // ctx.dirty(true) on every edit
       el.innerHTML = '<input data-k="x">';
       el.querySelector('input').oninput = () => ctx.dirty(true);
     },
     async save() { /* POST the values; throw Error(msg) to keep the page open */ },
     reset() { /* put the inputs back to the stored values */ },
   });
   ```
   Save awaits `save()` of every dirty section, then returns to the main page; Cancel, Back, Close
   and Esc call `reset()` (asking first if anything is unsaved). Read-only sections omit both.
   Optional `show()`/`hide()` run when the section's category comes on screen or is left.

## Conventions

- C: 4-space indent, `static` file-local state with `s_` prefix, `static const char *TAG`, short
  comments that say why. Check every `esp_err_t`; a failing feature `init` is logged and skipped,
  never fatal (the OTA rollback confirmation must still run).
- Don't log from code that a log hook may call (`web_ws_broadcast` never logs for this reason).
- New settings follow the `settings_get_x` / `settings_set_x` pattern in `core/settings.c`
  (NVS namespace `app`).
