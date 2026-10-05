# Node EEPROM allocation

ATtiny1614 provides 256 bytes of EEPROM organized in 32-byte physical pages.

The separate 32-byte USERROW stores immutable manufacturing credentials. It is programmed through UPDI, is not part of the EEPROM address space, and survives ordinary flash/chip erase operations.

| USERROW offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `RSFC` |
| 4 | 1 | Schema version (`1`) |
| 5 | 1 | Flags (`1` means provisioned) |
| 6 | 2 | Reserved zero |
| 8 | 16 | Unique factory commissioning key |
| 24 | 2 | CRC16-CCITT over bytes 0..23, little-endian |
| 26 | 2 | Bitwise inverse of the stored CRC bytes |
| 28 | 4 | Reserved zero |

The chip UID is never duplicated in writable memory; firmware and the provisioning station read its 10 bytes from `SIGROW_SERNUM0`. An unconfigured node without a valid USERROW record does not start its commissioning radio.

## Common nodes

All node profiles reserve two configuration slots and two frame-counter reserves:

| Address | Size | Purpose |
| --- | ---: | --- |
| `0x00..0x19` | 26 | Network configuration slot A |
| `0x1A..0x1F` | 6 | Frame-counter reserve A |
| `0x20..0x39` | 26 | Network configuration slot B |
| `0x3A..0x3F` | 6 | Frame-counter reserve B |
| `0x40..0xFF` | 192 | Profile-owned or unused |

Each network configuration slot has this exact format:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | Magic `RN` |
| 2 | 1 | Storage schema version (`3`) |
| 3 | 1 | Wrapping generation |
| 4 | 1 | Provisioning state: `1` provisional, `2` active |
| 5 | 1 | Node ID |
| 6 | 1 | Gateway ID |
| 7 | 1 | Network ID |
| 8 | 8 | Per-node derivation salt |
| 16 | 8 | Request nonce, little-endian |
| 24 | 2 | CRC16-CCITT over bytes 0..23, little-endian |

Profile ID, firmware version, sensor selection, pins, reporting intervals, and the transmit power ceiling are compile-time values and are not stored here. The radio power level and fallback flag are held only in RAM, so adapting the level never writes EEPROM ([README.md](README.md#radio-power)).

Saving always targets the older/inactive slot. Its magic is invalidated first, the payload and CRC are written next, and the two magic bytes are committed last. On boot, both slots are validated and the newest generation is selected, including across the 8-bit generation wrap.

Configuration commits and resets are read back before success. The factory key
and stored salt derive the session keys at boot.

Each reserve stores a four-byte little-endian bound followed by CRC16-CCITT.
The node reserves 1024 counters before transmission and skips the reserved
range after restart. Retries retain the same counter and bytes. If both reserves
are invalid with a configuration present, the node discards the configuration
and pairs again. See [counter persistence](../protocol/V3-SECURITY.md#counter-persistence).

Network factory reset invalidates only the two common configuration slots. It does not modify USERROW or profile-owned EEPROM.

## Counter node

The counter profile divides its 192-byte application area by write frequency:

| Address | Size | Purpose |
| --- | ---: | --- |
| `0x40..0x4F` | 16 | Last `SET_COUNT` result slot A |
| `0x50..0x5F` | 16 | Last `SET_COUNT` result slot B |
| `0x60..0xFF` | 160 | Wear-levelled cumulative-count ring |

Each `SET_COUNT` result slot has this exact format:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | Commit magic (`0xC7`) |
| 1 | 1 | Storage schema version (`1`) |
| 2 | 1 | Wrapping generation |
| 3 | 1 | Status: `Pending` or `Applied` |
| 4 | 2 | Command ID, little-endian |
| 6 | 4 | Count before the command, little-endian |
| 10 | 4 | Requested/applied count, little-endian |
| 14 | 2 | CRC16-CCITT over bytes 0..13, little-endian |

The result slots use the same invalidate/payload/commit sequence as network configuration. Applying `SET_COUNT` is a recoverable three-step operation:

1. Persist a `Pending` result with the command ID, old count, and requested count.
2. Persist the requested value in the counter ring.
3. Persist the result as `Applied`, then send the command result.

On boot, a valid `Pending` result is completed before new pulses are accepted. Repeating a command with the same ID returns the stored result instead of changing the count again.

A new installation starts a new command ID sequence, so commissioning replaces a stored `SET_COUNT` result with one whose command ID is zero. The gateway never issues zero.

Each ring entry contains a one-byte sequence followed by a four-byte unsigned cumulative count. Sequence `0xFF` means an empty entry; valid sequences wrap from `0xFE` to `0x00`. A save writes the count, then commits the sequence byte last. Until that commit the destination still carries the oldest sequence in the ring, so an interrupted save is never selected on boot. There are 32 entries.

Every confirmed LOW-to-HIGH transition increments the count and immediately persists it before telemetry transmission. Capacity planning uses 150,000 pulses/year to include higher winter gas consumption. A 32-entry ring therefore visits each entry about 4,688 times per year. The sequence byte and the low count byte are each written once per visit and wear first. Against the device's 100,000-cycle minimum EEPROM endurance, this is about 21 years of nominal minimum endurance (3.2 million persisted pulses total).

The counter area survives network factory reset.

## Native verification

The host-side tests cover CRC fallback, interrupted records, both generation wraps, factory-reset isolation, and multiple counter-ring rotations:

```sh
wsl bash node/scripts/run_native_tests_wsl.sh
```
