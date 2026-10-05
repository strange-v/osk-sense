# OSK Sense Protocol

[`protocol-manifest.json`](protocol-manifest.json) defines client-visible IDs, telemetry layouts, encodings, ranges and sentinels. [`protocol-vectors.json`](protocol-vectors.json) provides cross-language known answers. [V3-SECURITY.md](V3-SECURITY.md) defines authenticated radio frames, pairing, acknowledgements and counter persistence.

## Protocol layers

| Layer | Representation |
| --- | --- |
| RFM69 transport | Target, sender and control bytes, covered by the authentication tag |
| Radio traffic | V3 header, counter and encrypted payload with a CMAC tag; join frames are clear and authenticated |
| Gateway telemetry and WebSocket | Application header `0x40`, common prefix and opaque profile payload |

The gateway authenticates and checks replay state before forwarding telemetry. It removes the radio envelope and prepends `0x40` for the application codec. The profile examples below describe this client representation. The manifest's `security_v3` object describes the radio envelope; its `radio` object describes application codec IDs.

## Frame kinds

V3 headers use bits 7..5 for protocol major `3` and bits 4..0 for the kind. Unknown versions and kinds are rejected.

| Kind | Header | Name |
| ---: | ---: | --- |
| 0 | `0x60` | Telemetry |
| 1 | `0x61` | Join request |
| 2 | `0x62` | Join accept |
| 3 | `0x63` | Join confirm |
| 4 | `0x64` | Command |
| 5 | `0x65` | Command result |
| 7 | `0x67` | Join complete |
| 8 | `0x68` | Command ready |
| 9 | `0x69` | No command |
| 10 | `0x6A` | Activation report |

## Sleeping-node command session

A sleeping node pulls one pending command when its button is short-pressed or a telemetry ACK carries `command_pending`. Command ready has an empty payload. The gateway replies with Command or an empty No command under that ready's counter. Repeating a ready returns the same sealed reply bytes; after a gateway restart a counter-floor ACK requires a fresh ready. See [command wire layouts](V3-SECURITY.md#commands).

Command and result payloads begin with a nonzero uint16 little-endian command ID and a one-byte type or status, followed by up to eight argument or result bytes. A result carries a fresh node counter greater than the ready's counter. The gateway requires the command ID and pairing salt of its latest delivery to match.

| Status | Name | Meaning |
| ---: | --- | --- |
| 0 | `applied` | The effect is durable |
| 1 | `unsupported` | The firmware does not implement the type |
| 2 | `invalid_argument` | Wrong argument length or value |
| 3 | `storage_failure` | Nothing applied; retry in a later session |

Every status except `storage_failure` completes the command. The authenticated ACK confirms queuing; the pending command remains until its result is durably recorded.

### Command types

| Type | Name | Profiles | Arguments | Result data |
| ---: | --- | --- | --- | --- |
| 1 | `set_count` | 6 | `count`: uint32 LE | `previous_count`: uint32 LE, `count`: uint32 LE |
| 2 | `read_info` | All | None | `profile_id`: uint16 LE, `firmware.major`, `firmware.minor`, `firmware.patch`, `max_power_level`: unsigned bytes |

Commands are one-shot actions. Configuration the gateway maintains, such as radio power, is desired state carried by the telemetry acknowledgement instead.

`set_count` replaces the cumulative pulse count. `previous_count` is the count immediately before the command. The node sends pulse-counter telemetry with the updated count in the same wake-up.

`read_info` returns the identity Join request carries, as compiled into the running image. The gateway replaces the registry record's profile ID, firmware version, and transmit power ceiling with it before recording the result, so a node reflashed in place needs no new pairing. A changed profile discards the node's cached telemetry, and a fixed power level above a lowered ceiling returns to automatic control. Its result is bound to the gateway-issued command ID, so a recorded result cannot be replayed into the registry. The node keeps no state for it and answers every delivery afresh.

### Command IDs and redelivery

The gateway allocates command IDs from one wrapping 16-bit sequence per installation and never issues zero. The node records the ID of the last applied command of each type that changes its state in the same atomic write as its effect ([EEPROM.md](../node/EEPROM.md)). A command whose ID equals the recorded one is a redelivery: the node does not apply it again and resends the stored result under a fresh frame counter. Commissioning clears the recorded IDs, so a reinstalled gateway's sequence cannot collide with them.

The gateway marks the pending command complete only after a matching result is durably recorded. If the result is lost, a later session delivers the same command ID and payload.

## Telemetry

```text
+----------------+-------------+---------------+----------------+--------------------------+
| DATA[0] = 0x40 | DATA[1]     | DATA[2]       | DATA[3..4]     | DATA[5..N]               |
| v2 / Telemetry | radio state | downlink RSSI | supply voltage | profile-specific payload |
+----------------+-------------+---------------+----------------+--------------------------+
```

Telemetry does **not** carry a profile ID. The gateway uses the RFM69 transport sender ID to look up the registration record:

```text
RFM69 sender ID ----> gateway registry ----> profile ID ----------------+
                                                                       |
DATA[5..N] profile-specific payload ------------------------------------+--> consumer decoder
```

Every telemetry frame has a five-byte common prefix:

| Offset | Bytes | Field | Encoding |
| ---: | ---: | --- | --- |
| 1 | 1 | `radio_state` | Bits 0..4 `tx_power_level` `0..31`; bit 5 `radio_fallback`; bits 6..7 zero |
| 2 | 1 | `downlink_rssi` | Signed dBm of the acknowledgement to the node's previous report, measured while its sync word arrives; `INT8_MIN` means not measured |
| 3 | 2 | `supply_voltage` | Unsigned little-endian millivolts; `UINT16_MAX` means unavailable |

The link fields come first and supply voltage last, next to the measurements. A future profile for a node without a meaningful supply voltage can therefore end the common prefix at offset 3 while every current profile keeps the same bytes, with supply voltage as its first field.

The node reports its actual radio state in every frame, so each RSSI the gateway measures is paired with the level the frame was sent at ([Radio power](#radio-power)). The gateway may decode the common prefix without knowing the profile. It treats bytes from offset 5 onward as opaque and forwards the complete frame together with the stored profile ID, sender ID, RSSI, and receive time in its WebSocket envelope. Telemetry from an unknown or inactive sender cannot be decoded safely and must be rejected and counted.

Radio frames add a counter and authentication tag to these payloads; see [V3 frame costs](V3-SECURITY.md#frames). The examples below use radio state `02` (level 2) and downlink RSSI `BA` (−70 dBm).

## Telemetry acknowledgement

A telemetry ACK is authenticated under the node's MAC key and transmitted frame counter. It can carry `command_pending`, a power target, a counter floor or an activation challenge. Only accepted frames and immediate duplicates receive a normal ACK; duplicate frames are never published again. See [ACK format](V3-SECURITY.md#frames) and [gateway replay rules](V3-SECURITY.md#gateway-bound).

A node that sees `command_pending` opens a command session in the same wake-up. The node README specifies its back-off after failed sessions.

## Radio power

Radio power is desired state, not a command. The node owns its level and reports it in every telemetry frame; the gateway owns a policy and, while the reported level differs from the one it wants, adds `power_target` to each telemetry acknowledgement. A lost acknowledgement is simply repeated by the next one, so no command ID or session is involved.

| Rule | Owner | Behaviour |
| --- | --- | --- |
| Ceiling | Node | A build constant for its hardware and supply, reported as `max_power_level` in Join request and `read_info` |
| Clamp | Node | Every level it uses — from a target, a fallback, or commissioning — is at most its ceiling |
| Start | Node | After commissioning and after every restart it transmits at its ceiling |
| Apply | Node | A target takes effect after the acknowledgement that carried it; the node keeps the level only in RAM |
| Fallback | Node | After three consecutive reports without acknowledgement it switches to its ceiling and sets `radio_fallback`; the next applied target clears it |
| Target | Gateway | A per-node policy: automatic, or a fixed level within the ceiling |
| Hold | Gateway | While it averages a newly reported level it keeps wanting its previous level, so the next acknowledgement returns a restarted node there; a `radio_fallback` report is taken at its word |

Registration stores one stable numeric profile ID; `read_info` replaces it after a reflash. The profile defines the complete node contract: telemetry layout, logical category, supported commands, and Home Assistant entities. A wire-incompatible telemetry layout or different command set requires a new profile ID. The profile ID is not repeated in normal telemetry.

## Pairing

An unprovisioned node uses sender ID and network ID `0`. The gateway's explicit pairing window selects its 10-byte factory UID and unique 16-byte factory key. Join request and accept authenticate with that key; confirm and complete use the MAC key derived from the transaction's salt. The gateway stores the exact transaction before replying and prepares the replay slot before committing Active. Matching repeated confirm returns the same complete without a write.

See [pairing layouts and persistence](V3-SECURITY.md#pairing) for the 64-bit request nonce, salt, assigned address and network ID. The factory key stays in gateway RAM only during the pairing window.

## Telemetry profile template

Each profile added to the manifest and described below must define all of the following:

| Item | Required definition |
| --- | --- |
| Identity | Stable numeric profile ID and human-readable profile name |
| Length | Exact payload length, excluding `DATA[0]` |
| Fields | Byte offset, byte count, signedness, and byte order |
| Meaning | Scale, unit, valid range, and sentinel values |
| Test vector | Complete application frame in hexadecimal and expected values |
| Commands | Supported command types |

`field.name` is a stable machine identifier, not a display label. It must be unique across the common and profile-specific fields of a complete telemetry frame. Once released, it must not be renamed or reused for another quantity. The same name in different profiles represents the same logical entity and must have the same `quantity` and `unit`; offsets and wire encodings may differ. A profile containing multiple measurements of one quantity gives each a distinct semantic name. User-facing labels come from the consumer's translations.

### Production profile numbering

Profile IDs are stable opaque keys allocated sequentially. Their numeric values do not encode a capability family, hardware type, or Home Assistant presentation. IDs are never reused after release. Profile 0 remains invalid.

All eight initial profiles are frozen below. The generated map is the compact byte-layout reference; each profile section adds its meaning, constraints, and a hexadecimal example mirrored by a native known-answer test.

Gas/water and door/window are installation presentation, not different wire profiles. A profile ID says how to decode the bytes; it never says what the device is called or what a pulse is worth.

![Telemetry profile byte map](generated/telemetry-profiles.svg)

### Profile 1: supply voltage

Profile ID `1` carries only the common prefix. The application frame is exactly five bytes. Example for 3300 mV: `40 02 BA E4 0C`.

### Profile 2: temperature test node

The application frame is exactly seven bytes: the common prefix followed by signed little-endian temperature in degrees C x 100, valid from `-8000` through `12500`; `INT16_MIN` means unavailable. Example for 3300 mV and 23.50 degrees C: `40 02 BA E4 0C 2E 09`.

### Profile 3: temperature and humidity

The application frame is exactly nine bytes: the common prefix, signed little-endian temperature in degrees C x 100, and unsigned little-endian humidity in percent RH x 100. Temperature is valid from `-8000` through `12500`, with `INT16_MIN` meaning unavailable. Humidity is valid from `0` through `10000`, with `UINT16_MAX` meaning unavailable.

Example for 3300 mV, 23.50 degrees C, and 45.67% RH: `40 02 BA E4 0C 2E 09 D7 11`.

### Profile 4: temperature, humidity, and pressure

The application frame is exactly eleven bytes: the profile 3 fields followed by unsigned little-endian atmospheric pressure in tenths of a hectopascal. Pressure is valid from `3000` through `11000` (300.0 through 1100.0 hPa); `UINT16_MAX` means unavailable.

Example for 3300 mV, 23.50 degrees C, 45.67% RH, and 1013.2 hPa: `40 02 BA E4 0C 2E 09 D7 11 94 27`.

### Profile 5: binary input

The application frame is exactly six bytes: the common prefix followed by one-byte state (`0` or `1`). State `1` means the contact is open; profiles 7 and 8 use the same meaning. Example for 3300 mV and state `1`: `40 02 BA E4 0C 01`.

### Profile 6: pulse counter

The application frame is exactly nine bytes: the common prefix followed by an unsigned 32-bit little-endian cumulative pulse count. Example for 3300 mV and count `0x12345678`: `40 02 BA E4 0C 78 56 34 12`.

Gas/water meaning, units per pulse, and display unit are installation metadata. They are not part of this telemetry frame. This profile additionally supports `set_count`.

### Profile 7: binary input with SHT40 climate data

The application frame is exactly ten bytes: the common prefix, state (`0` or `1`), signed little-endian temperature in degrees C x 100, and unsigned little-endian humidity in percent RH x 100. The profile 3 ranges and sentinels apply. Example for 3300 mV, state `1`, 23.50 degrees C, and 45.67% RH: `40 02 BA E4 0C 01 2E 09 D7 11`.

### Profile 8: binary input with TMP112 temperature

The application frame is exactly eight bytes: the common prefix, state (`0` or `1`), and signed little-endian temperature in degrees C x 100. The profile 2 range and sentinel apply. Example for 3300 mV, state `1`, and 23.50 degrees C: `40 02 BA E4 0C 01 2E 09`.

## Codec invariants

- No packed C/C++ structs are transmitted directly.
- Every multi-byte value has explicit byte order and fixed width.
- Encoding and decoding do not allocate memory.
- The common frame codec does not interpret profile-specific telemetry bytes.
- Documented hexadecimal examples are mirrored by native unit tests and `protocol-vectors.json`.
