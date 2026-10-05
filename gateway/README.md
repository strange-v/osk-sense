# OSK Sense Hub firmware

Shared ESP32 firmware for:

- Wireless-Tag WT32-ETH01 with LAN8720/RMII Ethernet.
- Waveshare ESP32-S3-ETH with W5500/SPI Ethernet and optional PoE.

The application does not require PSRAM. Current firmware version is declared in `include/FirmwareVersion.h`.

## Build

From this directory:

```powershell
pio run
pio run -e gateway_wt32_eth01
pio run -e gateway_waveshare_s3_eth
```

PlatformIO is pinned in `platformio.ini`, along with exact library versions. On the current Windows workstation the executable is available at `C:\Users\stran\.platformio\penv\Scripts\platformio.exe` when it is not in `PATH`.

## Hardware

WT32 RFM69 wiring: SCK 12, MISO 15, MOSI 4, CS 14, DIO0/IRQ 36, RESET 33. Waveshare RFM69 uses a separate HSPI bus from W5500: SCK 43, MISO 44, MOSI 1, CS 2, DIO0/IRQ 38, RESET 39. `GATEWAY_RFM69_RESET` names the GPIO wired to the module's RESET pin and is optional; without it the module keeps its state through an ESP32 reset and only a power cycle clears it. Add a pull-up from RFM69 CS to 3.3 V so it remains deselected through reset and UART recovery.

The physical-presence button is BOOT/GPIO0 on Waveshare and, on WT32, a push button from GPIO32 to GND using the internal pull-up. GPIO21 drives the Waveshare RGB LED; WT32 has no status LED, so the serial log reports every button action. WT32 is outside the primary hardware acceptance scope.

## Local secrets and OTA

Copy `include/LocalSecrets.example.h` to the Git-ignored `include/LocalSecrets.h`. A nonempty OTA password enables ArduinoOTA. Operational and per-device commissioning radio keys are generated or entered at runtime and are never taken from this build-time header. Existing invalid secret data is never overwritten automatically.

OTA targets:

```powershell
pio run -e gateway_wt32_eth01_ota -t upload
pio run -e gateway_waveshare_s3_eth_ota -t upload
```

Use a non-OTA target for serial or USB recovery.

## Releases

Pushing a tag `major.minor.patch` that equals `FirmwareVersion.h` runs `.github/workflows/gateway-release.yml`: Web UI and native tests, firmware and Web UI images for both boards, then a GitHub Release with these assets:

| Asset | Content |
| --- | --- |
| `gateway-<board>-firmware.bin` | App image for an OTA slot |
| `gateway-<board>-ui.bin.gz` | LittleFS image of that board's `web` partition, gzip with a 4 KB deflate window |
| `manifest.json` | Format 2: version, and per board each image's file, download size and SHA-256; for the Web UI also `image_size`, with the hash over the inflated image |
| `manifest.signed` | First line: base64 of the DER ECDSA P-256 / SHA-256 signature; then the exact `manifest.json` bytes |

A release already drafted for the tag receives the assets and is published with its own title and notes; otherwise the workflow creates one titled with the version and generated notes.

Boards are `wt32-eth01` and `waveshare-s3-eth`. The workflow signs with the `GATEWAY_SIGNING_KEY` repository secret; the matching public key is `gateway-signing.pub.pem`. Only the public key is committed. `scripts/release/verify_manifest.py <dir>` checks a downloaded release the way the gateway does.

The gateway installs a release itself through `/ui/update` (see API.md, Firmware update): it downloads `manifest.signed` from `releases/latest/download/` over HTTPS with the ESP-IDF certificate bundle, verifies the signature against the embedded key, and installs its board's images. The Web UI is inflated with the ROM miniz and written over the `web` partition sector by sector, skipping sectors that already hold the new bytes; a failure leaves the recovery page. The firmware then streams into the inactive OTA slot, which becomes bootable only when size and SHA-256 match the manifest.

## Web UI filesystem

The Web UI is stored in a LittleFS partition named `web`. Its partition-table subtype remains the legacy `spiffs` value required by the pinned ArduinoOTA/PlatformIO filesystem command; the bytes, generator, and mounted filesystem are LittleFS. The source frontend remains outside the firmware source tree; its production output is copied to `data/` before building the filesystem image. The gateway never formats LittleFS automatically. A missing, damaged, or incompatible image therefore leaves the REST API operational and serves a small recovery page from firmware instead of erasing evidence or configuration. The page signs in an admin (or, before setup, asks for the gateway button) and reinstalls the Web UI of the running firmware's release through `POST /ui/update/repair-ui`.

Every UI image must contain `/index.html` and `/ui-manifest.json`:

```json
{"ui_version":"0.9.0","required_firmware":"0.9","build":"git-sha"}
```

Both versions come from `include/FirmwareVersion.h` at build time: `ui_version` is the full version, `required_firmware` its `major.minor`. The gateway serves the image only when that `major.minor` matches its own; the patch level is ignored. `build` is diagnostic metadata and is not used for compatibility. The UI does not gate on `api_version`: that number describes the external client contract under `/api`, which the UI barely uses, while the UI depends on `/ui` and so moves with the firmware.

The Vue 3 frontend source lives in `ui/`. The generated `data/` directory is git-ignored and must not be committed. From `ui/`:

```powershell
npm ci
npm run dev
npm test
npm run build
```

The development server proxies API requests to `http://osk-hub.local`. Set `GATEWAY_URL` before `npm run dev` to use another hostname. The production build replaces the generated contents of `data/`, writes the manifest from the package version, API version, and Git SHA, precompresses JavaScript and CSS as deterministic gzip files, and enforces compressed and total-size budgets. `index.html` and `ui-manifest.json` remain uncompressed because firmware reads them directly.

Run `npm run build` first, then build or upload the generated contents of `data/` over a local cable:

```powershell
pio run -e gateway_wt32_eth01 -t buildfs
pio run -e gateway_wt32_eth01 -t uploadfs
pio run -e gateway_waveshare_s3_eth -t uploadfs
```

Upload it over the existing authenticated ArduinoOTA connection:

```powershell
pio run -e gateway_wt32_eth01_ota -t uploadfs
pio run -e gateway_waveshare_s3_eth_ota -t uploadfs
```

Firmware and filesystem uploads are deliberately separate. Every new firmware image, from a release or ArduinoOTA, runs on trial with A/B rollback (see API.md, Firmware update); the LittleFS partition has no rollback. An interrupted filesystem update can make the UI unavailable, but it does not affect the API, configuration, or the embedded recovery page. Retry `uploadfs`, or install the release's Web UI from the recovery page.

Changing a partition table is not part of an application OTA. Existing gateways must therefore receive one cable upload with the firmware layout before their first LittleFS upload. Back up important configuration before repartitioning. Subsequent firmware and UI releases can use OTA normally while the layout remains unchanged.

## Erasing NVS

For routine backup, restore and Waveshare button reset, see [Installation backup and recovery](BACKUP.md). Cable erase is the fallback when firmware or NVS cannot perform button reset.

Erasing the NVS partition returns a gateway to its first-boot state without touching the firmware, OTA state, or Web UI. Use it when storage no longer loads, for example after a storage layout change: the serial log reports `Gateway storage contains data but no valid snapshot`, the UI asks for initial setup, and setup fails with `setup_storage_failed`.

The erase deletes every store: settings, users and API tokens, the installation key and network ID, the device secret, the node registry, and the command book. `gateway_id` derives from the device secret, so clients see a new gateway, and every paired node must be factory-reset and paired again.

Both boards use a 64 KiB (`0x10000`) `nvs` partition at the end of their Web UI storage area (see `partitions/`). Erase only the address for the matching board over a cable, with the port `pio device list` shows; `--chip` makes esptool refuse a board of the other type:

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM8 erase-region 0xfe0000 0x10000
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 --port COM5 erase-region 0x3e0000 0x10000
```

The first line is for the Waveshare board, the second for WT32-ETH01, which must be in its ROM bootloader as for any cable upload. esptool resets the board afterwards. It boots as a new gateway, with a fresh device secret and a random network ID, and waits for initial setup. Do not use `pio run -t erase`: it erases the whole flash, including the firmware and the Web UI.

## Load testing

`scripts/status_logger.py` appends `/ui/status` health to a CSV every five minutes: boot ID and reset reason, uptime, free heap, radio and WebSocket counters. It signs in again after a gateway restart and records an unreachable gateway as a row with an error. It asks for the password, or takes it from `OSK_PASSWORD`:

```powershell
python scripts/status_logger.py osk-hub-a085e3e6cc20.local admin soak.csv
```

The node image `radio_flood` ([node README](../node/README.md)) supplies burst traffic. Two of them, about 28 frames per second together with retries after collisions, were acknowledged for 12 minutes with no dropped telemetry, no missed interrupt, and free heap steady at about 195 KB; after a reset the radio received again without a power cycle.

## Crash dumps

A panic or task-watchdog reset writes a core dump to the `coredump` partition, where it stays until the next crash; `reset_reason` in `/ui/status` names the cause. Read the Waveshare dump over USB and decode it against the ELF of the running firmware:

```powershell
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port COM8 read-flash 0xff0000 0x10000 core.bin
$env:PATH = "$env:USERPROFILE\.platformio\packages\tool-xtensa-esp-elf-gdb\bin;$env:PATH"
esp-coredump --chip esp32s3 info_corefile -t raw -c core.bin .pio\build\gateway_waveshare_s3_eth\firmware.elf
```

`esp-coredump` is in the PlatformIO Python environment (`%USERPROFILE%\.platformio\penv\Scripts`). The WT32-ETH01 dump is at `0x3f0000`. The task watchdog guards only `loop()`; startup runs before it is enabled.

## Runtime

One priority-11 task owns RFM69 and all FIFO/SPI operations. After 5 s without an interrupt it services a DIO0 edge it missed (`missed_interrupts` in `/ui/status`), and every 5 s it checks that the module is in RX, that its carrier is the configured band, and reads back 24 configuration registers. A module that left RX, sits on another carrier, or no longer matches what the task wrote is reset through `GATEWAY_RFM69_RESET` when wired and configured again on the current network with hardware AES disabled; `module_restores` and `module_restore_failures` count the outcomes, `frequency_faults` counts wrong carriers (also at startup, which retries), and the serial log names the register. `frequency_hz` in `/ui/status` is the carrier last read from the module. Active telemetry enters bounded queues and is acknowledged only after acceptance. Commissioning commits registry transactions in its own task. The radio task authenticates V3 traffic, checks replay state and durably reserves bounds before accepting new counters. The main loop keeps the latest opaque telemetry frame for each node and publishes it through the binary WebSocket.

The gateway serves two API surfaces: `/api` is the external client contract (`info`, the node registry read) and moves only with `api_version`, while `/ui` is everything the Web UI needs and moves with the firmware. Outside both sit `/health`, an unauthenticated liveness probe carrying only `status` and `boot_id`, and `/ws`. See `API.md`. Settings, authentication and secrets use independent dual-slot stores; the V3 registry uses one atomic NVS blob. SNTP uses configured NTP servers and reapplies changes without reboot.

Before the first user exists, the Waveshare status LED blinks green and a short button press opens the physical setup window. `POST /ui/setup` creates the first admin and can set the hostname and operational network ID. Until then the radio sleeps; setup applies its operational network, so nodes can be paired without a restart. Radio encryption and authentication use separate keys per node. Pairing is opened from the management UI after manually entering the node UID and its unique factory key; a short button press can close an active pairing window.

References:

- [REST API](API.md)
- [WebSocket binary protocol](WEBSOCKET.md)
- [Persistent storage](STORAGE.md)
- [Installation backup and recovery](BACKUP.md)
- [System architecture](../ARCHITECTURE.md)
- [Unfinished work](../ROADMAP.md)
