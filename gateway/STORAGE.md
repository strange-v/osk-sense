# Gateway persistent storage layouts

This document is the byte-level source of truth for gateway settings, authentication, node registry, and installation-secret persistence. The settings, authentication, installation-secret, and command-book snapshots share storage schema version 2; the node registry carries its own schema version. Schema 2 is frozen by the portable codecs in `../shared/RadioProtocol` and their native known-layout, round-trip, validation, corruption-recovery, and interrupted-write tests. ESP32 NVS adapters and runtime ownership are implemented by `ConfigurationStore` and `NodeRegistryStore`.

All multi-byte integers are unsigned little-endian unless stated otherwise. No compiler structs are persisted directly. Reserved bytes and unused fixed records are encoded as zero and must be zero when decoding schema version 2.

## Common dual-slot rules

The settings, authentication, installation-secret and command stores each own two NVS blobs. A save serializes the complete next generation into the inactive slot, reads it back, validates every field and its CRC32, and only then publishes it as current. The previous valid generation remains recoverable after interruption or corruption. Registry and replay bounds use single blobs with NVS journaling.

Every snapshot begins with:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | Store-specific ASCII magic |
| 4 | 2 | Storage schema version, currently `2` |
| 6 | 4 | Wrapping generation |
| 10 | 2 | Exact total encoded size including CRC |

The final four bytes are CRC32 over every preceding byte, encoded little-endian. Loading validates both slots and selects the newer valid wrapping generation using a positive signed 32-bit difference. An absent store loads documented defaults at generation zero; it does not write merely because the gateway booted.

Generation advances only after a durable semantic change. Re-saving identical content is a no-op. Readers receive copies or immutable published snapshots and never retain pointers into mutable store memory.

## V3 replay-bound module

`GatewayReplay` and `ReplayBoundStorage` provide the V3 store; the operational
radio service does not use it yet. NVS namespace: `radio-bound`; key: `bounds`;
magic: `RSRB`; schema: `3`; exact size: 288 bytes.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 12 | Common header with schema 3 |
| 12 | 8 | Presence bitmap for 64 registry slots |
| 20 | 8 | Bound-present bitmap, a subset of presence |
| 28 | 256 | 64 upper bounds H, 4 bytes each |
| 284 | 4 | CRC32 |

Presence without a bound marks fresh keys with no accepted frame. Absent and
unbounded records have H = 0. The blob must validate before any stored bound
is trusted. Missing or invalid data loads all records absent; nodes with existing
keys require fresh activation. An NVS read error blocks writes.

NVS journals replacement of the single blob. Saves commit and verify exact
read-back before publishing the snapshot or accepting the frame. A failed
commit or read-back blocks acceptance until reload. A power cut before acceptance
may leave the previous committed blob or the replacement; both cover every
frame published before the write. Generation is a revision counter, not a
selection between copies.

Bounds are excluded from backup. Restore and factory reset erase `radio-bound`.
Slot ownership must stay tied to its node's keys; deleting or replacing keys
forgets that slot before fresh-key initialization.

## Gateway settings snapshot

NVS namespace: `gateway-config`; slot keys: `config_a`, `config_b`; magic: `RSGC`; exact schema-2 size: 248 bytes.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 12 | Common snapshot header |
| 12 | 1 | Flags |
| 13 | 1 | Hostname byte length, `0..32` |
| 14 | 1 | NTP server count, `0..3` |
| 15 | 1 | Reserved, zero |
| 16 | 2 | Node-pairing window seconds |
| 18 | 2 | Initial-admin setup window seconds |
| 20 | 32 | Hostname ASCII bytes, zero-padded |
| 52 | 192 | Three fixed 64-byte NTP server entries |
| 244 | 4 | CRC32 |

Flag bit 0 enables mDNS and bit 1 enables NTP; bits 2..7 are zero. Each NTP entry is one ASCII length byte followed by 63 zero-padded bytes. Entries after `ntp_server_count` are entirely zero. Accepted values are DNS hostnames or textual IP addresses without scheme, path, or port. Empty active entries, embedded NULs, control characters, duplicates, and values longer than 63 bytes are rejected. When NTP is enabled, count is `1..3`; when disabled, a saved list may remain.

Hostname is a DNS label: lowercase ASCII letters, digits and hyphens, never leading or trailing. Empty is valid and means the gateway derives `osk-hub-<mac>`. Pairing duration is `30..900` seconds and defaults to 120. Initial setup duration is `60..1800` seconds and defaults to 600.

Generation-zero defaults are an empty hostname, enabled mDNS and NTP, NTP servers `pool.ntp.org` and `time.cloudflare.com`, a 120-second pairing window, and a 600-second setup window. NTP changes take effect without reboot. Timezone is not stored because the gateway uses UTC. DHCP, telemetry cache, current clock, uptime, boot ID, and diagnostic counters are runtime state.

## Authentication snapshot

NVS namespace: `gateway-auth`; slot keys: `auth_a`, `auth_b`; magic: `RSAU`; exact schema-2 size: 1036 bytes.

It contains four fixed user slots and eight fixed API-token slots. Active records occupy the first counted slots; every remaining slot is zero.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 12 | Common snapshot header |
| 12 | 1 | User count, `0..4` |
| 13 | 1 | API-token count, `0..8` |
| 14 | 2 | Reserved, zero |
| 16 | 4 | Next user ID |
| 20 | 4 | Next token ID |
| 24 | 368 | Four 92-byte user records |
| 392 | 640 | Eight 80-byte token records |
| 1032 | 4 | CRC32 |

IDs are nonzero and unique within their record type. Next IDs are never-issued values; deletion does not reuse IDs. Zero is normalized to one before the first allocation. Exhaustion after `UINT32_MAX` is an error rather than silent reuse.

### User record

Exact size: 92 bytes.

| Relative offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | User ID |
| 4 | 1 | Username length, `1..32` |
| 5 | 32 | Normalized username, zero-padded ASCII |
| 37 | 1 | Role |
| 38 | 1 | Flags |
| 39 | 1 | Password-hash algorithm |
| 40 | 4 | PBKDF2 iteration count |
| 44 | 16 | Random salt |
| 60 | 32 | Password hash |

Role 1 is admin and role 2 is viewer. Flag bit 0 means enabled; other bits are zero. Hash algorithm 1 is PBKDF2-HMAC-SHA256 with 25,000 iterations by default, a 16-byte random salt, and a 32-byte output. The codec accepts 10,000 through 2,000,000 iterations and never silently downgrades a record.

Usernames are lowercase ASCII letters, digits, `.`, `_`, or `-`. Password input is UTF-8 between 8 and 128 bytes and is never persisted or logged. Mutations cannot remove, disable, or demote the last enabled admin. An empty user set is valid only for the physical initial-setup flow.

### API-token record

Exact size: 80 bytes.

| Relative offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | Token ID |
| 4 | 1 | Display-name byte length, `1..32` |
| 5 | 32 | Display-name UTF-8 bytes, zero-padded |
| 37 | 1 | Flags |
| 38 | 2 | Scope bitmask |
| 40 | 8 | Creation time, UTC Unix milliseconds or zero |
| 48 | 32 | SHA-256 of the raw token |

Flag bit 0 means enabled. Scope bit 0 means `telemetry:read`, the only scope: it covers both the node registry and the telemetry stream. An active record carries at least one scope; all other flag and scope bits are zero. The raw token is 32 cryptographically random bytes shown once as unpadded base64url. Only its digest is stored and verification uses constant-time comparison.

`last_used_at` is deliberately runtime-only because persisting it on requests would cause high-frequency flash wear. Login sessions, failed-login counters, rate-limit state, and CSRF material are runtime state too.

## Installation secrets snapshot

NVS namespace: `gateway-secrets`; slot keys: `secret_a`, `secret_b`; magic: `RSGS`; exact schema-2 size: 67 bytes.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 12 | Common snapshot header |
| 12 | 2 | Presence flags |
| 14 | 1 | Operational RFM69 network ID, `1..255` |
| 15 | 16 | Installation AES key |
| 31 | 32 | Gateway device secret |
| 63 | 4 | CRC32 |

Presence bit 0 marks the installation key and bit 1 the device secret; other bits are zero. Bytes for an absent value are zero. The installation key is raw and may contain zeros.

The operational network ID is always `1..255`. Zero is never stored, because network 0 is the commissioning network that unprovisioned nodes use. On first boot the gateway stores a random ID; initial setup may replace it with a user-chosen value, generates the installation key, and commits both together in this snapshot. Ordinary edits are rejected while the registry contains an active node. A radio network reset clears the registry first, then generates a new installation key and network ID.

When the installation key is absent, its bytes are zero; the network ID is still nonzero. When the device secret is absent, its bytes are zero.

The device secret comes from the ESP32 hardware RNG, is not the public stable gateway ID, survives ordinary settings/auth/network reset, and is reserved for local secret derivation and authenticated export.

CRC32 provides neither confidentiality nor authenticity. Physical flash access can recover keys and authentication material; the production threat model accepts that risk and does not require flash encryption or secure boot.

An empty secrets namespace is initialized with a hardware-random device secret and a random operational network ID, without an installation key. Initial setup generates and durably stores the installation key; radio keys are never imported from build-time headers. Existing but invalid slot data is reported as corruption and is never treated as an empty store or automatically overwritten. A gateway whose stores do not all load runs without persistent storage until its NVS partition is erased, as the README describes; that erases every store, not only the damaged one.

This snapshot holds no commissioning profile. Every node has a unique factory commissioning key supplied with its UID, entered by hand or scanned from the node's QR code. The radio task holds the key only in RAM for one pairing transaction; it is never added to this snapshot or the registry. The commissioning network ID is a runtime constant of zero and is not stored either. The gateway wipes the key whenever the radio returns from the commissioning profile to the operational one.

## Node registry snapshot

NVS namespace: `node-reg`; key: `registry`; magic: `RSNR`; schema version 4; maximum size 12,560 bytes. Capacity: 64 records. Serialization and read-back scratch memory are allocated off the ESP32 task stack.

| ID | Use |
| ---: | --- |
| 0 | Unprovisioned node during commissioning |
| 1..99 | Persistent node IDs, lowest unused ID allocated first |
| 100 | Gateway |
| 101..254 | Reserved |
| 255 | Broadcast |

Pending, active, and disabled records retain their ID. Only explicit removal releases it.

Each stored record is 196 bytes:

| Relative offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 10 | Fixed tinyAVR factory UID |
| 10 | 1 | Node ID |
| 11 | 2 | Profile ID, little-endian; zero invalid |
| 13 | 3 | Firmware major, minor, patch |
| 16 | 1 | State: 1 pending, 2 active, 3 disabled |
| 17 | 4 | Commissioning nonce; low counter word of a V3 nonce |
| 21 | 1 | Display-name byte length, `0..48` |
| 22 | 48 | Display name, UTF-8, zero-padded |
| 70 | 1 | Transmit power ceiling from Join request, `0..31` |
| 71 | 1 | Power policy: 0 automatic; `N + 1` fixed level `N` |
| 72 | 1 | Stable replay slot, `0..63`; 255 means no V3 transaction |
| 73 | 123 | `PairingTransaction` snapshot, including full LE64 nonce, exact request/accept and both derived keys; zero without a V3 transaction |

Runtime last-seen time, RSSI, telemetry, radio power control state, and counters are not persisted.

| Snapshot offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `RSNR` |
| 4 | 2 | Schema version |
| 6 | 4 | Generation |
| 10 | 1 | Record count |
| 11 | 1 | Reserved, zero |
| 12 | `count * 196` | Records |
| end | 4 | CRC32 over preceding bytes |

The state flow is `absent -> pending -> active`; explicit management may set `disabled` or remove a record. Active or disabled records cannot be replaced through pairing. `RegistryPairing` authenticates before reserving a UID, persists one immutable reply per transaction and rejects retired counter words. V3 confirmation prepares the replay slot before committing Active. Matching repeated confirmation returns `JOIN_COMPLETE` without any registry or bound write.

Replay slots are unique and retained when removal changes record order. A new
Pending transaction forgets its slot before committing new keys. Retrying a
Pending confirmation preserves a Paired bound belonging to those keys.

The single registry blob is replaced through NVS journaling and checked against
the exact encoded bytes before publication. A failed commit or read-back blocks
mutations and key lookup until reload. Invalid data blocks initialization and
requires explicit reset or repair; no older transaction is selected.

NVS work never runs in the radio-owner task. Commissioning persists a reservation before queuing `JOIN_ACCEPT`; synchronized store APIs own all registry access.

A registry commit holds its mutex through the NVS write and read-back validation. The radio-owner task queues telemetry through its lock-free active-node view. V3 registry commit timing requires hardware measurement.

## Command book snapshot

NVS namespace: `node-cmd`; slot keys: `commands_a`, `commands_b`; magic: `RSCB`; exact size: 820 bytes.

The book holds at most 16 records, one per node: its pending command, or the result of its last command. Queuing replaces the node's record; when the book is full, the oldest result is evicted, and a book of 16 pending commands refuses another. Both command slots fit under 2 KiB. The 64 KiB NVS partition holds the registry, its journal replacement and the other installation stores.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 12 | Common snapshot header |
| 12 | 1 | Record count, `0..16` |
| 13 | 1 | Reserved, zero |
| 14 | 2 | Next command ID, nonzero |
| 16 | 800 | Sixteen fixed 50-byte records, oldest first |
| 816 | 4 | CRC32 |

Each record:

| Relative offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 10 | Factory UID of the node the command is for |
| 10 | 1 | Node ID, `1..99`, unique in the book |
| 11 | 2 | Command ID, nonzero |
| 13 | 1 | Command type |
| 14 | 1 | Argument length |
| 15 | 8 | Arguments, zero-padded |
| 23 | 1 | State: 1 pending, 2 completed |
| 24 | 1 | Result status; zero while pending |
| 25 | 1 | Result data length; zero while pending |
| 26 | 8 | Result data, zero-padded |
| 34 | 8 | Queued at, UTC Unix milliseconds or zero |
| 42 | 8 | Completed at, UTC Unix milliseconds or zero |

Types, argument layouts, statuses, and result lengths follow [PROTOCOL.md](../protocol/PROTOCOL.md); a completed record never holds `storage_failure`. Records after the count are entirely zero. The UID binds a command to one physical node: a record whose UID differs from the node's current registration is never delivered and is replaced by the next command for that node ID.

An absent or corrupt store starts empty with a hardware-random next command ID. The sequence wraps from 65535 to 1.

## Atomic operations and reset boundaries

There is no transaction across namespaces. Settings touch only `gateway-config`; users/tokens touch only `gateway-auth`; commissioning touches only `node-reg`; commands touch only `node-cmd`; installation-key rotation touches only `gateway-secrets` and needs a separate recovery workflow.

- Deleting a node removes its command record after the registry commit.
- A radio network reset clears the command book but keeps its command ID sequence.

- Settings reset restores `gateway-config` defaults.
- Authentication reset requires a physical setup action and clears auth records plus runtime sessions.
- Registry reset does not silently erase installation secrets.
- Full installation reset clears all stores only after separate destructive confirmation.
- Ordinary reboot writes none of these stores.

## Backup mapping

Diagnostic export may contain decoded settings, public identity, non-secret registry metadata, and diagnostics. It excludes password hashes, token hashes, radio keys, and device secret.

An [encrypted installation backup](BACKUP.md) contains logical settings, installation secrets and registry records. It excludes authentication, commands, padding, generations and slot CRCs. Restore accepts only a clean gateway, validates the full file before writing, and creates a new admin. `gateway-recover/operation` blocks normal startup after an interrupted restore and resumes an interrupted factory reset. Invalid files leave the gateway unchanged; interrupted writes require reset and another import.
