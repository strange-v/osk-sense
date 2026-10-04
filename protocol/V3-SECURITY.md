# V3 node security

Authenticated, replay-resistant radio traffic between nodes and the gateway. Every change here alters wire or storage formats, so it means reflashing nodes and resetting the gateway registry ([AGENTS.md](../AGENTS.md#pre-release-data-and-compatibility)).

## Phase 1 scope

| Area | Change |
| --- | --- |
| [Crypto](#crypto) | Software AES-128: CTR encryption and a 4-byte CMAC tag for every frame kind. RFM69 hardware AES is not used. |
| [Keys](#keys) | One key per node, derived at pairing; no installation key shared by the nodes of a gateway |
| [Pairing](#pairing) | Join frames under CTR + CMAC with the factory key |
| [Replay](#frames-and-replay) | Frame counter, tagged ACK, duplicate filtering on the gateway |
| [Counter persistence](#counter-persistence) | See [open decisions](#open-decisions) |
| [EEPROM](#eeprom) | Smaller network configuration slots; a counter reserve record |
| [Flash](#flash) | RFM69 fork with `RF69_NO_READALLREGS`, `-mcall-prologues` |

## Crypto

AES-128, forward direction only: CTR for confidentiality, CMAC (RFC 4493) truncated to 4 bytes for authenticity. The implementation computes each round in one pass per output column ("fused round"), with an expanded key and an S-box generated at compile time.

| Cost on ATtiny1614 at 4 MHz | Value |
| --- | ---: |
| Flash | 1 162 B, plus protocol glue (estimated 0.2–0.4 KB) |
| RAM | 208 B (176 B expanded key, 32 B CMAC subkeys) |
| One AES block | 6 793 cycles |
| Report: CTR + CMAC over one block | 14 578 cycles |
| ACK tag check | 7 363 cycles |
| CMAC key setup, once per boot | 9 409 cycles |

A report with its ACK costs about 5.5 ms and 9 µC, about 4 % of a 215 µC report. The node runs the crypto while the RFM69 sleeps: in standby (about 1.25 mA) the radio would nearly double that cost.

[node/crypto_bench](../node/crypto_bench) holds the implementation and its known-answer tests (FIPS-197 C.1, SP 800-38A F.5.1, RFC 4493):

| Check | Command, from `node/crypto_bench` |
| --- | --- |
| Known answers on the host (WSL) | `g++ -std=c++17 -DOSK_AES_FUSED test_host/main.cpp lib/OskCrypto/src/OskCrypto.cpp -o /tmp/osk_crypto && /tmp/osk_crypto` |
| Flash cost | `pio run -e size_empty -e size_fused`, then subtract |
| Cycles on the chip | `pio run -e bench_fused -t upload`, then read COM12 at 9600 baud |

## Keys

```text
K_node = CMAC(K_factory, salt)
```

| Element | Rule |
| --- | --- |
| Salt | 4–8 random bytes the gateway generates per pairing and sends in Join accept |
| Node | Stores the salt, not the key; derives K_node at boot from the factory key in USERROW |
| Gateway | Stores K_node in the node's registry record (16 bytes per node); it wipes the factory key after pairing |
| Re-pairing | A new salt gives a new key; frames under the old key fail |
| Network ID | Stays: it sets the RFM69 sync word that filters other installations |
| Exposure | Whoever has a node's QR code and recorded its pairing can derive its key. A node read out of its flash exposes only itself. |

## Pairing

| Rule | Reason |
| --- | --- |
| Join request, accept, confirm, and complete use CTR + CMAC under the factory key | Authenticated pairing; no block splicing between join frames |
| CTR covers several blocks | Join accept exceeds one block |
| `createNonce()` uses real entropy, such as jitter of the 32 kHz oscillator against the main clock, instead of `micros()` | Right after power-up `micros()` is nearly deterministic, which would let an old Join accept or command session be replayed |

## Frames and replay

On air: sender ID in clear, low 16 bits of the frame counter, ciphertext, 4-byte tag.

| Rule | Reason |
| --- | --- |
| The nonce combines sender, direction, and the full frame counter | A key never meets the same counter twice in one direction |
| New content takes a new counter | CTR nonce uniqueness |
| A retransmission reuses the counter and the exact bytes | Same plaintext under the same counter is not keystream reuse |
| Gateway: counter > last accepts, = last re-ACKs without publishing, < last rejects | Replay resistance; retransmissions do not produce duplicate reports |
| The gateway reconstructs the counter's high bits from its last accepted value | Only 16 bits travel |
| The ACK carries a tag over the report's counter | A replayed ACK cannot suppress retries or set `power_target` |
| The gateway sends the ACK right after the tag check | The node listens 40 ms per attempt |
| The pulse count stays a full 32-bit value | No consumer rebuilds it across wraps or node resets |

## Counter persistence

The gateway keeps the last accepted counter per node. The node must never reuse a counter under its key, also after a restart: with CTR a reused counter reuses the keystream.

Leading candidate, a counter with a reserve:

| Element | Rule |
| --- | --- |
| Node counter | 32 bits in RAM |
| Reserve | EEPROM holds a limit R; the node uses counters below R and writes R + N on reaching it; after a restart it continues from R, skipping unused values |
| Storage | Two copies of R with a CRC, about 12 bytes; no wear ring, because writes are rare (about 1.4 a day at N = 1024 and one report a minute, plus boots) |
| N | Below 2¹⁵, so the gateway can reconstruct the high bits |
| Brown-out loops | Each boot writes R; the delayed first report after a brown-out (ROADMAP item 2) bounds the writes |
| Gateway | Last accepted counter per node in RAM, persisted periodically to narrow the replay window after its own restart |

Telemetry with a reserve does not depend on the gateway's reverse link, so a node that cannot hear ACKs still delivers its reports.

## EEPROM

| Record | Change |
| --- | --- |
| Network configuration slots A and B | The 16-byte installation key becomes the 4–8-byte salt: about 20 instead of 32 bytes per slot, about 24 bytes free in total |
| Counter reserve | About 12 bytes in the space the slots free; the same address in every image, including `counter_reed`, whose profile area is full |

## Flash

`lib_deps` points to the `no-readallregs` branch of the strange-v/RFM69 fork, and release environments define `RF69_NO_READALLREGS`. Without it `readAllRegs()` links `Serial`, UART0, and its interrupt vectors into images that never use them.

| Image | Today | `RF69_NO_READALLREGS` and `-mcall-prologues` |
| --- | ---: | ---: |
| `counter_reed` | 14 832 | 13 538 |
| `climate_tmp112` | 14 529 | 13 425 |
| `binary_sht40` | 14 476 | 13 332 |
| `binary` | 12 592 | 11 510 |

`counter_reed` keeps about 1.3 KB after the crypto and its glue. Debug images use `Serial` themselves and do not fit; see [open decisions](#open-decisions).

## Open decisions

### Counter persistence

| | Counter with a reserve | Counter from the gateway | Session key at boot |
| --- | --- | --- | --- |
| How | [As above](#counter-persistence) | The node asks for a starting counter with a nonce | K_session = AES(K_node, node nonce ‖ gateway nonce); the counter restarts at 0 |
| Node writes | One per N frames and one per boot | None | None |
| Gateway writes | Periodic last counter | Issued value per node | None |
| Gateway restart | Transparent to nodes | Nodes resync over the reverse link | Nodes rekey over the reverse link |
| Node cannot hear the gateway | Telemetry still delivered | Node stays silent | Node stays silent |

`.noinit` RAM survives watchdog, software, and UPDI resets but not power-on or, reliably, brown-out. Keyed on `RSTCTRL.RSTFR` and guarded by a 32-bit magic plus an inverted copy, it can carry the counter across non-power resets. A doubtful check must fall back to the stored reserve.

### Debug images

| Image | Today | Free | With crypto (estimate) |
| --- | ---: | ---: | ---: |
| `binary_sht40_debug` | 16 209 | 175 | about 17.3 KB |
| `counter_reed_debug` | 16 121 | 263 | about 17.2 KB |
| `climate_tmp112_debug` | 15 773 | 611 | about 16.9 KB |
| `binary_debug` | 13 883 | 2 501 | about 15.0 KB |

| Option | Cost |
| --- | --- |
| Fewer, shorter debug messages | Less insight in the field |
| Debug builds without some features, such as crypto | Debugs logic, not the radio path as shipped |
| A bench-only 32 KB part with the same pinout (ATtiny3224, if its pinout matches) | Different ADC; release boards stay on the 1614 |

## Later

| Item | Note |
| --- | --- |
| Shorter frames | Do before release; until then a report costs about 4–5 % more |
| `.noinit` counter carry-over | Skips the boot write of R after a non-power reset |
| One-way link indicator | Reports without `downlink_rssi` and with `radio_fallback` set mean the node does not hear the gateway's ACKs |
| Decimal meter entry | The gateway stores a pulse value per node and converts a reading such as 01234,56 into a `SET_COUNT` |
| Calibration on the gateway | Revisit with MQTT; with the full count on air it is only an offset |
| `constexpr` constructors, one dual-slot EEPROM store | Flash reserve, about 0.4 KB each |

### Shorter frames

Every byte saved is about 3.7 µC of airtime at 55.5 kbit/s.

| Field | Today | Planned | Saving |
| --- | --- | --- | --- |
| `supply_voltage` | 2 B, mV | 1 B: 1.60 V + n × 10 mV; `255` unavailable | 1 B, every profile |
| Binary state | 1 B | 1 bit in `radio_state` bits 6..7, defined as profile flags | 1 B, profiles 5, 7, 8 |
| Temperature and humidity | 2 + 2 B, ×100 | 13-bit temperature in 0.05 °C (−80…+125) and 10-bit humidity in 0.1 % in 3 B | 1 B, profiles 3, 4, 7 |

| Profile | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Bytes today | 5 | 7 | 9 | 11 | 6 | 9 | 10 | 8 |
| Bytes planned | 4 | 6 | 7 | 9 | 4 | 8 | 7 | 6 |

`radio_state` and `downlink_rssi` stay: automatic power control depends on them.
