# V3 telemetry security study

Findings and measurements behind authenticated, replay-resistant telemetry. Nothing here is implemented yet; the open choice is in [Freshness after a restart](#freshness-after-a-restart).

## Problem

Telemetry and its acknowledgement are encrypted by RFM69 hardware AES, one 16-byte ECB block, with no counter and no MAC ([PROTOCOL.md](PROTOCOL.md#replay-resistance)).

| Weakness | Consequence |
| --- | --- |
| A recorded frame is accepted again | Jam a door's "open" report and replay an old "closed" one |
| Equal content gives equal ciphertext | State changes can be told apart on air without the key (voltage and RSSI bytes blur this only partly) |
| ACK is not bound to the report | Jam the gateway and replay an old ACK: the node believes the report was delivered and does not retry |
| One installation key in every node | A node whose flash is read out can forge every other node |

Zigbee and Z-Wave S2 carry a frame counter and a MIC, so reviewers compare against that. Nodes have no radio firmware update, so the wire format is effectively frozen once nodes are installed: the change belongs before release.

## Options

| | A: counter inside the RFM69 block | B: software AES-CTR + 4-byte CMAC |
| --- | --- | --- |
| How | 2-byte counter in the ECB block; the gateway checks it rises and the padding is zero (encode-then-encipher) | RFM69 AES off; LoRaWAN-like CTR encryption and truncated CMAC |
| Airtime | Always one 16-byte block | 3 + frame + 2 + 4 bytes, no padding; about today's airtime once frames are shortened |
| Forgery | Untargeted only (random values); about 32–70 check bits today, depending on free bytes per profile | 2⁻³² per attempt for every frame |
| Per-node keys | Impossible: the radio decrypts before the encrypted sender byte is known | Possible: sender travels in clear and selects the key |
| Multi-block frames | Not covered (block splicing) | Any length |
| Flash | Hundreds of bytes | About 1.16 KB plus protocol glue (estimated 0.2–0.4 KB) |
| Perception | Sound, but "ECB" needs explaining | Standard construction |

**Recommendation: B.** The measurements below show it fits the ATtiny1614 without a custom radio driver or a larger MCU.

## Crypto measurements

Prototype: forward-only AES-128 with an expanded key, compile-time generated S-box, CMAC per RFC 4493, single-block CTR. It passes FIPS-197 C.1, SP 800-38A F.5.1, and RFC 4493 vectors on the host and on the chip. ATtiny1614 at 4 MHz, cycles measured on the chip with `micros()` over 64 runs.

| Variant | Flash | AES block | Report (CTR + CMAC) | ACK tag check | CMAC key setup (once) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Separate SubBytes / ShiftRows / MixColumns | 1 076 B | 8 731 | 18 373 | 9 240 | 11 345 |
| **Fused round** (one pass per output column) | **1 162 B** | **6 793** | **14 578** | **7 363** | 9 409 |
| Fused and unrolled | 1 576 B | 6 317 | 13 610 | 6 886 | 8 865 |

RAM: 176 B expanded key and 32 B CMAC subkeys. `-O2` was 8 % slower; an S-box in SRAM gained 2 % for 256 B of RAM. Use the fused round.

A report costs two blocks when header and ciphertext fit one CMAC block, the ACK check one: about 5.5 ms and 9 µC at roughly 1.6 mA, about 4 % of a 215 µC report. Run the crypto while the RFM69 sleeps; in standby (about 1.25 mA) it would nearly double that cost.

5 MHz (20 MHz oscillator, divided by four; requires rewriting `OSCCFG`) shortens this by 20 % but leaves the charge per computation about the same, and every POWER.md figure would need re-measuring. Not needed for B.

## Flash budget

| Image | Today | `-mcall-prologues` | Serial stub | Both |
| --- | ---: | ---: | ---: | ---: |
| `counter_reed` | 14 832 | 14 326 | 14 024 | 13 538 |
| `climate_tmp112` | 14 529 | 14 199 | 13 735 | 13 425 |
| `binary_sht40` | 14 476 | 14 112 | 13 676 | 13 332 |
| `binary` | 12 592 | 12 298 | 11 784 | 11 510 |

| Saving | Size | Notes |
| --- | ---: | --- |
| Serial in release images | about 0.8 KB flash, 141 B RAM | `RFM69::readAllRegs()` references `Serial` unconditionally, which links UART0 and its vectors. `-Wl,--defsym=Serial0=0` in release environments removes it; pair it with a post-build check that the release ELF contains no `HardwareSerial`, because a live `Serial` use would otherwise write to address 0 silently. |
| `-mcall-prologues` | 0.3–0.5 KB | Shared register save/restore; about 20 cycles per call of an affected function |
| `constexpr` constructors | about 0.4 KB (estimate) | Global constructors are 746 B of field-by-field stores |
| One dual-slot EEPROM store | 0.4–0.6 KB (estimate) | `NetworkConfigStore` and `SetCountStore` repeat the same A/B logic; helps `counter_reed` only |
| Direct port access | under 0.1 KB | RFM69 keeps `pinMode` and `digitalWrite` linked anyway |
| Compiler flags | none | `-Os`, `-flto`, `-mrelax` are already on |

With both flags `counter_reed` keeps about 1.3 KB after B. A custom RFM69 driver (about 1.2–1.5 KB) and a 32 KB MCU stay as last resorts, in that order.

## Shorter frames

With B, every byte saved is airtime (about 3.7 µC per byte at 55.5 kbit/s); with A it is forgery margin.

| Field | Today | Proposed | Saving |
| --- | --- | --- | --- |
| `supply_voltage` | 2 B, mV | 1 B: 1.60 V + n × 10 mV; `255` unavailable | 1 B, every profile |
| Binary state | 1 B | 1 bit in `radio_state` bits 6..7, defined as profile flags | 1 B, profiles 5, 7, 8 |
| Temperature and humidity | 2 + 2 B, ×100 | 13-bit temperature in 0.05 °C (−80…+125) and 10-bit humidity in 0.1 % in 3 B | 1 B, profiles 3, 4, 7 |
| Pulse count | 4 B | 24 bits; the gateway extends wraps | 1 B, profile 6 |

| Profile | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Bytes today | 5 | 7 | 9 | 11 | 6 | 9 | 10 | 8 |
| Bytes proposed | 4 | 6 | 7 | 9 | 4 | 7 | 7 | 6 |

`radio_state` and `downlink_rssi` stay: automatic power control depends on them.

## Freshness after a restart

A MAC does not stop replay. Two things are needed in every design:

1. The gateway keeps the last accepted counter per node.
2. The node never reuses a counter under the same key. With CTR this is also a confidentiality requirement: a reused counter reuses the keystream.

| | 1: epoch in EEPROM | 2: counter from the gateway | 3: session key at boot |
| --- | --- | --- | --- |
| How | Node increments an EEPROM epoch at boot; nonce = (epoch, RAM counter) | Node asks for a starting counter with a nonce | K_session = AES(K_node, node nonce ‖ gateway nonce); counter restarts at 0 |
| Node writes | One per boot (wear-levelled ring) | None | None |
| Gateway writes | Epoch per node, only when a node restarts | Issued value per node | None |
| Gateway restart | Transparent to nodes | Nodes must resync (needs downlink) | Nodes must rekey (needs downlink) |
| Node hears nothing from the gateway | **Telemetry still delivered** | **Node stays silent** | **Node stays silent** |
| Node nonce quality | Not needed for telemetry | Must be unpredictable | Must be fresh |

Open decision. Notes for each:

- **Asymmetric links.** v1 showed reports arriving three times a second: the gateway heard the node, the node missed the ACK. That gateway had a poor antenna, so the cause may be gone, but other installations can hit it. Options 2 and 3 turn such a link into a total outage; option 1 keeps telemetry flowing.
- **EEPROM space.** `counter_reed` allocates all 192 profile bytes ([EEPROM.md](../node/EEPROM.md)); option 1 must take its ring from the counter ring.
- **Brown-out loops.** Option 1 writes once per boot, so a reboot loop (ROADMAP item 2) wears the ring; the planned delay of the first report after a brown-out also bounds this.
- **Node nonce.** `createNonce()` mixes `micros()` with the UID; right after power-up `micros()` is nearly deterministic, so options 2 and 3 need real entropy, such as jitter of the 32 kHz oscillator against the main clock.
- **RAM kept across resets.** `.noinit` RAM survives watchdog, software, and UPDI resets but not power-on or, reliably, brown-out. Keyed on `RSTCTRL.RSTFR` and guarded by a 32-bit magic plus an inverted copy, it can carry the counter (or session key) across non-power resets and skip the EEPROM write or rekey. A doubtful check must fall back to a new epoch: a wrong "valid" means keystream reuse.

## Protocol rules for B

| Rule | Reason |
| --- | --- |
| A retransmission reuses the counter and the exact bytes | The gateway re-ACKs a counter equal to the last one without publishing it, which removes duplicate reports; same plaintext under the same counter is not keystream reuse |
| New content takes a new counter | CTR nonce uniqueness |
| Gateway: counter > last accepts, = last re-ACKs, < last rejects | Replay resistance |
| ACK carries a tag over the report's counter | Stops forged ACKs that suppress retries or set `power_target` |
| The gateway sends the ACK right after the tag check | The node listens 40 ms per attempt; ESP32 AES takes microseconds, other processing waits |
| Nonce includes sender and direction | One key per node and direction never meets the same counter twice |
| Join and command sessions move to the same software AES | One crypto path; RFM69 AES can stay off |

## Diagnostics

Reports without `downlink_rssi` and with `radio_fallback` set mean the node does not hear the gateway's ACKs. The gateway UI should show this as a one-way link; ROADMAP item 2 already plans a similar flag for brown-out loops.
