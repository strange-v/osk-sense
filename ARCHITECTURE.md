# OSK Sense architecture

## System overview

ATtiny3224 nodes send compact binary measurements over encrypted RFM69. The Ethernet ESP32 gateway owns node registration, acknowledges accepted telemetry, timestamps it in UTC, caches the latest frame per node, and exposes local REST and WebSocket interfaces. Home Assistant is read-only; pairing, users, settings, commands, and recovery belong to the gateway management interface.

```text
sensor node -- encrypted RFM69 --> gateway -- REST/WebSocket --> Home Assistant
```

Telemetry profiles define wire-level measurements. Labels such as gas, water, front door, or bedroom are installation metadata, not profile IDs.

## Node model

Each ATtiny3224 image is statically composed for one stable profile. Shared code owns commissioning, radio, EEPROM, power, the wake clock, radio retry, and commands; profile code owns acquisition, report scheduling, and telemetry encoding. There is no dynamic feature registry on the 32 KiB Flash/3 KiB RAM target.

An unconfigured production node uses address and commissioning network ID 0 and has a unique 16-byte factory key supplied with its UID in a QR credential. The gateway keeps that key only in RAM during pairing. Successful commissioning assigns a persistent node ID, operational network ID, and a salt from which the node derives separate encryption and MAC keys. Network configuration and counter state use separate EEPROM domains.

Solar climate nodes use nominal 60/300-second reporting above/below 2500 mV; the 32-second RTC step gives accepted effective intervals of about 64/320 seconds. Battery nodes target at least three years, preferably five, on one cell; their climate intervals are fixed compile-time settings. Binary-input and counter nodes are event-driven with a rolling one-hour keep-alive. Counter reports may coalesce, but every confirmed pulse is persisted immediately.

## Radio and registry model

The gateway uses address 100. IDs 1..99 are allocated persistently; 0 is for commissioning and 255 is broadcast. Registry records hold UID, node ID, profile, firmware, 64-bit commissioning nonce, immutable pairing transaction, per-node keys, replay slot and pending/active/disabled state. Pairing sets the profile and firmware; after a node is reflashed in place, an administrator's `read_info` command replaces them and the gateway announces the change like any other registry mutation.

RFM69 hardware AES is disabled. V3 frames use software AES-CTR and CMAC with keys derived separately for each node; join request and accept authenticate with that node's factory key. Initial gateway setup generates an operational network ID in 1..255 and permits an advanced edit before confirmation. Ordinary changes are locked after a node is active; later changes require a staged migration design.

Normal telemetry is `common telemetry prefix + opaque profile payload`. The prefix carries supply voltage and the node's radio state; the gateway may decode it but does not decode profile-specific measurements. Exact radio bytes are specified only in [protocol/PROTOCOL.md](protocol/PROTOCOL.md).

Node settings the gateway maintains are desired state, not commands. The node reports the value it actually uses in every telemetry frame, and the gateway repeats the value it wants in the telemetry acknowledgement until the two match, so a lost frame costs nothing and no history can drift from the truth. Hard limits belong to the node: it clamps radio power to its hardware ceiling and falls back on its own after losing the gateway.

Commands are one-shot actions, such as setting a pulse count or reading a reflashed node's identity. Sleeping-node commands use a pull session. A short button press, or a telemetry ACK carrying the pending-command flag, makes the node send counter-bound `COMMAND_READY`; it receives one durable command or `NO_COMMAND`. Commands and results are idempotent and durable before acknowledgement.

## Gateway runtime

One high-priority task exclusively owns RFM69 and its FIFO. The ISR only signals bounded queues. Commissioning persists registry transactions outside that task. Radio reception authenticates before checking replay state, takes the registry mutex without waiting, and persists replay reservations before accepting a counter outside its bound. A competing mutation causes a retry; reservation and ACK latency require hardware measurement.

The gateway caches only the latest telemetry frame per node in RAM. Frames from different nodes survive a temporary consumer disconnect and appear in the next snapshot; repeated frames from one node collapse to the latest. Reboot clears the cache. Receive timestamps are UTC from SNTP; zero means unsynchronized.

REST carries identity, registry metadata, settings, and management. The binary WebSocket carries snapshots and live telemetry. Home Assistant uses one scoped read-only token and one WebSocket connection per gateway.

## Persistence and security

Settings, users/tokens, node registry, and installation secrets have separate NVS ownership. Mutable data uses versioned codecs, CRC32, generations and read-back validation. The pairing registry uses one atomic NVS blob; replay bounds use a separate blob excluded from backups. Restored nodes must answer an authenticated activation challenge. Existing invalid secret data is never mistaken for an empty store.

Passwords use salted PBKDF2-HMAC-SHA256. API tokens contain 32 random bytes, are shown once, and are stored only as SHA-256 digests. Radio keys, password hashes, token hashes, and the device secret are excluded from normal APIs and diagnostic exports.

The gateway serves HTTP and WebSocket only and is intended exclusively for a trusted private LAN. It does not provide TLS/WSS and must not be exposed directly to the Internet or an untrusted network. LAN traffic capture, DNS/mDNS spoofing, a compromised LAN client, and physical flash access are outside the accepted production threat model; flash encryption and secure boot are not required.

Firmware updates are the one outbound connection: HTTPS to GitHub Releases, started by an administrator. Authenticity comes from an ECDSA P-256 signature over the release manifest, checked against a key built into the firmware, and from the manifest's SHA-256 of every image; TLS only transports them. A new firmware image runs on trial and the bootloader returns to the previous one unless it confirms itself on the network.

## Versions

Release versions name a build; contract versions decide compatibility. Compatibility never follows from a release version, except the Web UI's `major.minor` check against the firmware.

| Version | Source | Changes when |
| --- | --- | --- |
| Gateway release (firmware and Web UI) | `gateway/include/FirmwareVersion.h`, equal to the Git tag | Every release: patch for fixes, minor when `/ui/*` changes |
| Node release | `node/lib/NodeCore/include/NodeFirmware.h` | Every node release, independently of the gateway |
| `/api` | `gateway/include/ApiVersion.h` | Incompatible change to `/api` |
| WebSocket stream | `GatewayStream.h` `kVersion` | Incompatible change to the stream |
| Radio protocol | `RadioProtocol.h` `kProtocolMajor` | Incompatible frame change |
| Gateway NVS, node registry | `GatewayStorage.h`, `RegistryPersistence.h` | Incompatible persisted layout |
| Backup container, payload | `BackupCrypto.cpp` `kPrefix`, `BackupCodec.cpp` `kPayloadVersion` | Incompatible backup file |
| Release manifest | `make_manifest.py` `MANIFEST_FORMAT` | Incompatible manifest |

## Documentation ownership

- Radio frames and payload validation: `protocol/PROTOCOL.md`.
- REST endpoints and JSON: `gateway/API.md`.
- WebSocket bytes and resynchronization: `gateway/WEBSOCKET.md`.
- Persistent bytes: `gateway/STORAGE.md` and `node/EEPROM.md`.
- Measured node consumption and battery budgets: `node/POWER.md`.
- Build, upload, wiring, and bench use: component README files.
- Unfinished work only: `ROADMAP.md`.

Implementation history, test counts, firmware sizes, and completed iteration logs are deliberately not maintained as architecture documentation.
