# Installation backup and recovery

## Use

1. As an admin, open **Administration → Installation backup**, enter a separate backup password twice, and download the `.oskbackup` file. Keep the password in a password manager. Make a fresh copy after changing settings or nodes.
2. Prepare a gateway with compatible firmware and Web UI. Switch off the original gateway before activating a replacement.
3. On a clean gateway, open setup, choose **Restore backup**, select the file and enter its password. Review the gateway ID, date and node count.
4. Enter a new administrator username/password and confirm. Press the gateway button to open the setup window, then restore. After restart, sign in and create a new Home Assistant token. A restored hostname can change the address; DHCP/MAC remain specific to the physical board.

| Included | Excluded |
| --- | --- |
| Hostname, mDNS, NTP, setup/pairing durations | Users, password hashes, API tokens, sessions |
| Node UID/ID, profile, firmware metadata, state, nonce, name, power policy, V3 pairing transaction and per-node keys | Command book, telemetry and replay bounds |
| Radio network ID, device secret | Firmware, Web UI, OTA password, MAC, DHCP lease |

Restoring the radio credentials and registry avoids re-pairing nodes present in the backup. The device secret preserves `gateway_id`. A backup cannot recover later changes; a node whose credentials changed after export may need pairing again. Lost backup passwords cannot be recovered.

## Factory reset

On a running gateway, hold the button (BOOT on Waveshare, GPIO32 on WT32) for **10 seconds**; the Waveshare LED then flashes red, and WT32 reports it only in the serial log. Release, then press again within **5 seconds**. Without the second press, reset is cancelled. A short press acts on release and controls setup/pairing.

Reset clears settings, users, tokens, registry, commands, replay bounds and the device secret. Firmware and Web UI remain. Nodes retain their credentials. Interrupted reset resumes at boot; interrupted restore requires reset followed by another import. The button works without valid application settings, but requires running firmware and writable NVS. An unbootable gateway, or an NVS failure that prevents recording reset, requires [cable recovery](README.md#erasing-nvs). Holding Waveshare BOOT during power-on/reset selects the ROM bootloader; the WT32 button has no effect on boot.

## API

These endpoints belong to `/ui`, use JSON requests, and send `Cache-Control: no-store` for exports/previews. Backup passwords are 12–128 UTF-8 bytes without NUL. Admin credentials follow ordinary setup validation. Import requests require `X-Backup-Request: 1`; restore also requires an open physical setup window. The request body limit is 24,924 bytes; export accepts at most 1,024 bytes.

| POST endpoint | Request | Result |
| --- | --- | --- |
| `/ui/backup/export` | `{"password":"…"}`; admin session + CSRF | Binary `.oskbackup` attachment |
| `/ui/backup/preview` | `{"file":"base64…","password":"…"}` | `gateway_id`, `created_at_ms`, `node_count`, `hostname`, `mdns_enabled` |
| `/ui/backup/restore` | Preview fields plus `username`, `admin_password` | `{"status":"restarting"}` |

Preview writes nothing and needs no physical window. Restore decrypts and validates the supplied file again; it does not rely on a cached preview. Only a gateway with ready storage, no users, an unconfigured radio and no registry records accepts import. Bearer tokens do not authorize export. Busy backup/OTA/reset operations are mutually excluded; snapshot capture holds the persistent mutation lock only while copying settings, secrets and registry.

| Error | Meaning |
| --- | --- |
| `409 backup_busy` | Pairing or another operation prevents backup |
| `409 restore_requires_clean_gateway` | Reset before importing |
| `409 recovery_required` | Recovery is blocked or the mutation lock is busy |
| `403 physical_setup_required` | Open/reopen the physical setup window |
| `422 invalid_backup_password` | Invalid export password length |
| `422 invalid_backup` | Wrong password, corrupt/unsupported file, or invalid contents |
| `500 restore_failed` | Reset and retry if recovery is required |

`GET /ui/setup` includes `recovery_required` and `recovery_reason` (`none`, `restore_incomplete`, `reset_incomplete`, `storage_unavailable`, `restarting`). The UI offers reset instructions when recovery is required.

## Container version 1

Maximum file size: **34,876 bytes**. Multibyte header integers are little-endian. The entire 44-byte header is AES-GCM additional authenticated data.

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 4 | ASCII `OSKB` |
| 4 | 1 | Container version `1` |
| 5 | 1 | KDF `1`: PBKDF2-HMAC-SHA256, 32-byte output |
| 6 | 1 | Cipher `1`: AES-256-GCM |
| 7 | 1 | Reserved, zero |
| 8 | 4 | Exactly 100,000 KDF iterations |
| 12 | 4 | Ciphertext length, at most 34,816 |
| 16 | 16 | Random salt |
| 32 | 12 | Random nonce |
| 44 | variable | Encrypted UTF-8 JSON |
| end | 16 | Authentication tag |

Each export uses hardware randomness for salt and nonce. The KDF runs through the password-hash worker; parsing happens only after GCM authentication succeeds. On Waveshare it dominates each request: export takes about 6.4 s, preview 5.8 s and restore 7.3 s, which adds the new admin's password hash. The web server answers nothing else meanwhile. No plaintext backup is written to flash. This protects the backup file, not HTTP traffic: use the gateway only on the trusted private LAN described in [API.md](API.md#security-and-backup).

Payload keys are `version` (`3`), `created_at_ms` (UTC milliseconds, zero if unavailable), `settings`, `network_id`, `device_secret`, `nodes`. Keys and UIDs are lowercase hexadecimal. Settings contain `hostname`, `mdns`, `ntp`, `pairing_seconds`, `setup_seconds`, `servers`. Each node contains `uid`, `id`, `profile`, `firmware` (three integers), `state` (1 pending, 2 active, 3 disabled), `nonce`, `name`, `max_power`, `power_policy`, `replay_slot`, `pairing` (123-byte transaction snapshot, including per-node keys, as hexadecimal). Values obey [storage limits](STORAGE.md); unknown nonzero profile IDs are preserved. Different container/payload versions are rejected.

Restoring excludes replay bounds. Once gateway V3 radio integration is complete,
each active V3 node must receive a fresh challenge and return an Activation report;
it needs a working reverse link before its telemetry can be published.

## Interrupted operations

NVS namespace `gateway-recover`, key `operation` (`uint8`): 0 idle, 1 restore incomplete, 2 factory reset requested. Unknown values or unreadable control data block normal startup.

Restore validates all data and the new admin before persisting marker 1. It clears the installation namespaces, writes and reads back each snapshot, then commits marker 0 and restarts. The writes take only the final moments of a restore, so a power cut usually leaves the gateway clean and the import can simply be repeated. Marker 1 prevents storage initialization, radio, commissioning and command processing at boot. The previous installation is not retained.

Button confirmation persists marker 2 and restarts. Before stores initialize, boot clears the installation namespaces and commits marker 0. The control namespace is never part of that erase. Firmware and `web` partitions are untouched.

## Verification

`wsl bash gateway/scripts/run_native_tests_wsl.sh` builds native tests against the actual codec, crypto, restore and recovery services with a fault-injected NVS adapter, under ASan/UBSan. It needs `curl`, `make`, `g++` and Node.js (Windows `node.exe` works). The first run downloads the pinned Mbed TLS and ArduinoJson releases, checks their SHA-256, and builds Mbed TLS into `~/.cache/osk-sense-native`; `MBEDTLS_DIR` overrides that with another built 3.x tree. The script then decrypts the test's encrypted fixture with `test/native/check_interop.mjs`, an independent Node.js implementation.

The serial log records each factory-reset step, what boot does with the control marker, the outcome and node count of every export, import and restore, and how long export encryption and import decryption took.
