# V3 node security

Authenticated, replay-resistant radio traffic between nodes and the gateway. Every change here alters wire or storage formats, so it means reflashing nodes and resetting the gateway registry ([AGENTS.md](../AGENTS.md#pre-release-data-and-compatibility)). `protocol-manifest.json` and `protocol-vectors.json` gain the v3 layouts and crypto known answers.

## Phase 1 scope

Implementation status:

| Piece | Status |
| --- | --- |
| Shared AES, key derivation, CTR/CMAC frame, ACK and join authentication | Implemented in `shared/RadioProtocol`; native known answers and bit-tampering tests |
| V3 crypto vectors | `protocol-vectors.json`, checked independently with Node.js AES |
| Pairing and command wire codecs | Implemented in `RadioSecurityFrames`; authenticated join codecs, nonce-free command payloads, counter-bound reply decoding and independent wire vectors |
| Immutable command reply cache | Implemented in `GatewayReplay`; exact bytes per node/counter, discarded on the next accepted frame or restart |
| Node target and Flash | ATtiny3224 at 4 MHz; all four release and debug profiles fit; see [Flash](#flash) |
| Pairing entropy | RTC/TCB0 hardware collection integrated; extractor, health guards and CMAC conditioning tested on [captured and synthetic data](../node/crypto_bench/ENTROPY.md); source qualification pending |
| Pairing transactions | Gateway `RegistryPairing` embeds `PairingTransaction` in the atomic registry, with stable replay slots and prepared-bound recovery; node `RadioSecurityPairing` pins salt in EEPROM before confirm; native restart, corruption and write-interruption tests; hooked into gateway commissioning |
| Node radio integration and command sessions | V3 pairing, telemetry, activation, authenticated ACKs and counter-bound command sessions integrated; native service tests cover retries, restart and failures |
| Gateway radio integration and command sessions | Integrated through `RegistryRadio`, `RadioService`, commissioning and command services; native tests cover tag tampering, replay, queue backpressure, restart, restore activation and immutable replies; both gateway targets build |
| EEPROM configuration and node counter reservations | Integrated in the node runtime; native interruption, overflow and service tests |
| Gateway bounds and backup activation | Implemented in `shared/RadioProtocol/GatewayReplay`, with `gateway/ReplayBoundStorage` NVS adapter and recovery erasure; native replay/activation/interruption tests; integrated into gateway radio reception |
| Failed-tag UI counters | Status page shows `failed_tags`, `replay_frames` and `activation_challenges` since boot |
| Gateway network secrets | Configured flag, network ID and device secret; radio keys are per node in the registry |

Node and gateway radio services use V3. ATtiny3224 entropy qualification,
real-radio interoperability, ACK timing during NVS reservations and stack
measurements remain hardware checks.

| Area | Change |
| --- | --- |
| [Crypto](#crypto) | Software AES-128: CTR encryption and a CMAC tag of 8 bytes from the node and 6 from the gateway, two keys per node, K_enc and K_mac. RFM69 hardware AES is not used. One shared implementation for node and gateway. |
| [Keys](#keys) | Keys per node, derived at pairing; no installation key shared by the nodes of a gateway |
| [Frames](#frames) | `protocol_major` 3, clear header and transport header under the tag, 32-bit frame counter, plaintext ACK with a tag, one tag input format |
| [Commands](#commands) | Sessions bound to the node's frame counter; `session_nonce` removed |
| [Pairing](#pairing) | Join frames in clear under a tag; the salt is the gateway's challenge; one transaction gives one set of reply bytes |
| [Counter persistence](#counter-persistence) | Node: counter with a reserve, N = 1024. Gateway: an upper bound per node and a counter floor in the ACK. After a backup restore, each node is activated by an Activation report answering a fresh challenge. |
| [EEPROM](#eeprom) | 26-byte network configuration slots, each followed by a reserve slot |
| [Flash](#flash) | RFM69 fork with `RF69_NO_READALLREGS`, `-mcall-prologues` |

## Crypto

AES-128, forward direction only: CTR for confidentiality, CMAC (RFC 4493) truncated to 8 bytes in node frames and 6 bytes in gateway frames for authenticity ([Tag length](#tag-length)), encrypt-then-MAC. CTR uses K_enc and CMAC uses K_mac, two independent [keys](#keys), as SP 800-38B requires for a CMAC key. The implementation computes each round in one pass per output column ("fused round"), with an expanded key and an S-box generated at compile time.

The node and the gateway use the same implementation, in `shared/RadioProtocol`, so nonce and tag formatting are one code path checked by the same native known-answer tests. ESP32 hardware AES would gain nothing: software AES there takes microseconds against the node's 40 ms ACK window.

The isolated bench measurements below use ATtiny1614 at 4 MHz. The bench
environments target ATtiny3224; complete node sizes are under [Flash](#flash).

`node/crypto_bench` environment `size_security` links the shared implementation
for key derivation, report sealing, ACK verification, join authentication and
gateway reply decryption. Its image is 2755 bytes of Flash and 472 bytes of
static RAM; `size_empty` is 492 bytes of Flash. The difference is 2263 bytes,
including the exercised frame glue.

`size_security_frames` exercises the node-side pairing and command codecs,
including counter-bound reply verification and shared crypto: 3726 bytes of
Flash and 456 bytes of static RAM with the Arduino core. It does not include
the complete radio, ACK, storage or profile paths.

`size_security_pairing` links node pairing, EEPROM configuration and key setup:
4792 bytes of Flash and 505 bytes of static RAM. Radio, frame-counter allocation,
entropy collection and profile paths are absent from this isolated bench.

| Cost on ATtiny1614 at 4 MHz | Value |
| --- | ---: |
| Crypto Flash, including key derivation | 1 380 B; complete node images are measured separately under [Flash](#flash) |
| RAM | 384 B (two 176 B expanded keys, 32 B CMAC subkeys); the derivation reuses the CMAC context |
| One AES block | 6 791 cycles |
| Report: CTR + CMAC over one block | 14 594 cycles |
| ACK tag check, one block | 7 360 cycles |
| Each further tag block | about 7 050 cycles |
| Key derivation from the factory key and setup, once per boot | 35 534 cycles, about 8.9 ms |

A report with its ACK costs about 5.5 ms and 9 µC, about 4 % of a 215 µC report. The node runs the crypto while the RFM69 sleeps: in standby (about 1.25 mA) the radio would nearly double that cost.

AES-CCM with one key was measured as the alternative: 176 B of RAM, but 35 062 cycles per report and 21 069 per ACK, about 14 ms and 22 µC (10 %), because it spends five AES blocks on a short frame and three on an ACK. Its 1 386 B of flash exclude key derivation.

[node/crypto_bench](../node/crypto_bench) holds the measured implementations and their known-answer tests (FIPS-197 C.1, SP 800-38A F.5.1, RFC 4493, SP 800-38C example 1, RFC 3610 packet vector 1):

| Check | Command, from `node/crypto_bench` |
| --- | --- |
| Known answers on the host (WSL) | `g++ -std=c++17 -DOSK_AES_FUSED test_host/main.cpp lib/OskCrypto/src/OskCrypto.cpp -o /tmp/osk_crypto && /tmp/osk_crypto` |
| Flash cost | `pio run -e size_empty -e size_two_keys -e size_ccm`, then subtract `size_empty` |
| Cycles on the chip | `pio run -e bench_fused -t upload`, then read COM12 at 9600 baud |

## Keys

```text
K_enc = CMAC(K_factory, 0x01 ‖ salt)
K_mac = CMAC(K_factory, 0x02 ‖ salt)
```

A counter-mode key derivation with CMAC as the PRF (SP 800-108). "The node's keys" below means this pair.

| Element | Rule |
| --- | --- |
| Salt | 8 random bytes, the gateway's challenge for one pairing transaction, sent in clear in Join accept. It is not secret: deriving the keys needs the factory key. |
| Node | Stores the salt, not the keys; derives both at boot from the factory key in USERROW |
| Gateway | Stores both keys in the node's registry record (32 bytes per node); it wipes the factory key after pairing, and selects the keys by the clear sender ID |
| Re-pairing | A new salt gives new keys; frames under the old keys fail |
| Network ID | Stays: it sets the RFM69 sync word that filters other installations |
| Exposure | Whoever has a node's QR code and recorded its pairing can derive its keys. A node read out of its flash exposes only itself. |

## Frames

RFM69 hardware AES is off, so the transport header (length, target, sender, control) travels in clear.

```text
frame:      target | sender | control | header | counter (4) | ciphertext                 | tag (8 or 6)
activation: target | sender | control | header | counter (4) | challenge (8) | ciphertext | tag (8)
ACK:        target | sender | control | payload (0..10)                                   | tag (6)
```

The activation layout belongs only to the [Activation report](#backup-restore).

| Field | Rule |
| --- | --- |
| Transport header | Target, sender, and control are covered by every tag, so no field that steers processing, such as the ACK request flag, can be changed without failing the tag |
| Header | One byte, `protocol_major` 3 and the frame kind; in clear, covered by the tag. The gateway needs the kind before decrypting, and frame length reveals it anyway. |
| Counter | The node's full 32-bit frame counter, little-endian. The gateway compares it with its last accepted value directly; a truncated counter would need reconstruction within a window that boot skips and gateway outages can exhaust. |
| Ciphertext | The kind's payload under CTR |
| Tag | The CMAC truncated to 8 bytes in node frames and to 6 bytes in gateway frames and ACKs ([Tag length](#tag-length)) |
| ACK payload | In plaintext: nothing in it is secret, and an ACK may change its content for the same report counter, which under CTR would reuse the keystream. Flags byte: bit 0 `command_pending`, bit 1 `power_target` (1 byte follows), bit 2 `counter_floor` (4 bytes follow, little-endian), bit 3 `challenge` (8 bytes follow); bits 2 and 3 are exclusive. |

### Tag input

One format for every tag, encrypt-then-MAC; CMAC needs no length prefix:

```text
logical header | target | sender | control | counter (4) | payload
```

| Frame | Logical header | Counter | Payload |
| --- | --- | --- | --- |
| Node or gateway frame | Its header byte | Its counter; for a reply, the counter of the node frame it answers | Ciphertext |
| Activation report | Its header byte | Its counter | The clear challenge, then the ciphertext |
| ACK | Frame kind 31 (`0x7F`), reserved for this purpose and never transmitted | The counter of the frame it acknowledges | ACK payload |
| Join frames | Their header byte | Absent; see [Pairing](#pairing) | Their fields |

The header byte separates frame kinds, and the transport header gives the direction and the node. The prefix is 8 bytes, so a tag input stays within one AES block while the payload is at most 8 bytes; profiles 4 and 7 (10 and 9 bytes) need a second block, about 1.8 ms.

CTR counter block, 16 bytes:

| Offset | Bytes | Value |
| ---: | ---: | --- |
| 0 | 1 | Direction: 0 node to gateway, 1 gateway to node |
| 1 | 1 | Node ID |
| 2 | 1 | Header byte |
| 3 | 4 | Full frame counter, little-endian; for a reply, the counter of the node frame it answers |
| 7 | 8 | Zero |
| 15 | 1 | Block index |

### Tag length

Forgery risk follows the attacker's verification attempts, about q / 2^(8 × tag bytes), not the rate of genuine reports (SP 800-38B, appendix A).

| Verifier | Attempts available | Tag | Risk |
| --- | --- | ---: | --- |
| Gateway, for node frames | Always listening: a forged frame takes about 4.3 ms of air, so about 230 attempts a second, 2·10⁷ a day, 7·10⁹ a year of continuous transmission | 8 bytes | About 4·10⁻¹⁰ after a year. A 4-byte tag would reach about 0.5 % after a day and 82 % after a year, and CTR lets a recorded frame's plaintext bits be flipped, so a hit would be a chosen change such as a door state. |
| Node, for ACK, Command, and No command | Only while it listens: about 9 attempts in the 40 ms after each transmission, about 58 in a 250 ms command session | 6 bytes | About 2.5·10⁻⁷ over five years for a node reporting every minute with three transmissions per report. A forged ACK changes state: it can carry `counter_floor`, which the node bounds (below). |

The 8-byte tag adds 4 bytes to every node frame, about 15 µC or 7 % of a report. The 6-byte ACK tag costs 2 bytes of reception over a 4-byte one, about 0.29 ms and 5 µC. The gateway counts failed tag checks per node and shows the count in its UI: CRC rejects corrupted frames before the tag check, so a failure almost always means a forgery attempt or a bug.

### Rules

| Rule | Reason |
| --- | --- |
| New content takes a new counter | CTR nonce uniqueness |
| A retransmission reuses the counter and the exact bytes | Same plaintext under the same counter is not keystream reuse |
| Gateway: a counter at or above the node's floor is accepted; a repeat of the last accepted counter is re-ACKed without publishing; any other counter is not published and gets an ACK carrying the floor | Replay resistance; retransmissions do not produce duplicate reports; a node behind the floor catches up ([Gateway bound](#gateway-bound)) |
| The ACK tag covers the acknowledged counter | A replayed ACK cannot confirm a different frame |
| The gateway sends the ACK right after the tag check | The node listens 40 ms per attempt |
| The node puts the RFM69 to sleep as soon as it has copied a received ACK and checks the tag with the radio asleep. On a wrong tag it listens again, but only until the attempt's original deadline. | Crypto with the radio in standby would nearly double its cost; a forged ACK cannot stretch the receive window |
| The node accepts `counter_floor` only above its current counter and at most 256 above it, and reaches it with a single EEPROM reservation | A genuine floor is never further ahead ([Gateway bound](#gateway-bound)); a forged one cannot exhaust the counter |
| The pulse count stays a full 32-bit value | No consumer rebuilds it across wraps or node resets |
| Application payloads keep their current layouts and precision. A report frame is the application frame plus 15 bytes, 4–10 bytes longer than today's 16 padded bytes; ACKs are 9–19 bytes against 16. | Shortening the payloads was dropped: the bytes it could save without losing precision cut `binary_sht40`, reporting every five minutes, by about 0.025 µA of about 4.0 µA (0.6 %, about two weeks of a CR2032), and `binary` by 0.06 %. Sleep and input polling are about 77 % of the current. |

A replayed ACK can only repeat an earlier ACK for the same counter, which happens when the gateway changed `command_pending` or `power_target` between retransmissions. The node then acts on stale flags until the next report; it cannot be made to accept anything the gateway did not send.

## Commands

`RadioSecurityFrames` encodes command payloads without `session_nonce`.
`openCommandReply()` requires the current Command ready counter before decoding
an authenticated Command or No command. The gateway's `Guard` caches one sealed
reply per node: caching different bytes under the same counter fails; a new
accepted frame or restart discards the cache.

| Payload | Layout | Bytes |
| --- | --- | ---: |
| Command ready, No command | Empty | 0 |
| Command | `command_id` LE16, type, arguments | 3..11 |
| Command result | `command_id` LE16, status, result data | 3..11 |

Command ID is nonzero. Unknown command types remain representable for an
Unsupported result; status values are 0..3. Arguments and result data each
have an 8-byte limit.

```text
Node                                          Gateway
  |-- report (c0) ----------------------------->|
  |<-- ACK, tag(c0), command_pending -----------|
  |-- Command ready (c1) ---------------------->|  checks tag, c1 fresh
  |<-- Command (reply to c1) -------------------|  caches the reply bytes for c1
  |   applies; command_id detects redelivery    |
  |-- Command result (c2) --------------------->|  records the result
  |<-- ACK, tag(c2) ----------------------------|
```

| Rule | Reason |
| --- | --- |
| The gateway has no frame counter of its own: every gateway frame answers a node frame and uses that frame's counter with direction 1 | No gateway counter state per node |
| Command ready, Command, and No command lose `session_nonce` | c1 binds the session; a reply from an older session fails its tag |
| A repeated Command ready (same c1, same bytes) gets the cached reply bytes, never a newly built reply. The cache holds one reply per node in RAM until the gateway accepts that node's next frame. | A changed queue would otherwise put new content under the same nonce |
| After a restart the gateway answers a repeated c1 with `counter_floor`, not with a reply | Its floor is then above every counter it answered before the restart ([Gateway bound](#gateway-bound)), so a second reply under the same c1 cannot exist; the node moves on and sends a new Command ready |
| An unactivated node after a [backup restore](#backup-restore) gets no reply to Command ready | Its counters are not yet known to be fresh |
| Command result uses the node's next counter c2 and carries `command_id` | An old result fails the counter check |
| Redelivery stays as today | A `command_id` equal to the recorded one returns the stored result without applying again |

## Pairing

`RadioSecurityFrames` seals and verifies the complete join messages. Decoders
authenticate before assigning output fields. UID is 10 bytes and
`request_nonce` is unsigned LE64; profile ID is LE16.

| Frame | Body after header | On-air bytes, including tag |
| --- | --- | ---: |
| Join request | UID, nonce, profile ID, firmware major/minor/patch, power ceiling | 33 |
| Join accept | UID, nonce, salt, assigned node ID, network ID | 35 |
| Join confirm | UID, nonce | 27 |
| Join complete | UID, nonce | 25 |

The gateway ID comes from the authenticated transport sender. Assigned node IDs
are 1..99, network ID is nonzero, and transmit power ceiling is 0..31.

Join frames are not encrypted: nothing in them is secret, so pairing uses no CTR and cannot reuse a keystream. Every join frame carries a tag.

| Frame | Key | Payload, in clear |
| --- | --- | --- |
| Join request | Factory key | UID, `request_nonce`, profile ID, firmware version, transmit power ceiling |
| Join accept | Factory key | UID, `request_nonce`, salt, node ID, network ID |
| Join confirm | K_mac | UID, `request_nonce` |
| Join complete | K_mac | UID, `request_nonce` |

Tag inputs follow the [common format](#tag-input) without a counter. Target, sender, and control are the transport header as sent; an unpaired node uses transport address 0, as today:

```text
Join request:  header | target | sender | control | UID | request_nonce | profile ID | firmware | power ceiling
Join accept:   header | target | sender | control | UID | request_nonce | salt | node ID | network ID
Join confirm:  header | target | sender | control | UID | request_nonce | salt
Join complete: header | target | sender | control | UID | request_nonce | salt
```

Confirm and complete include the salt in their tag although it is not on air, so both challenges bind every message after the accept, besides the keys derived from the salt.

A pairing transaction is identified by (UID, `request_nonce`).

| Rule | Reason |
| --- | --- |
| The gateway selects the factory key by the clear UID | Only inside a pairing window for that UID, as today |
| The gateway stores the salt and `request_nonce` with the pending registry reservation before it sends Join accept, and answers every repeat of the same Join request with the same Join accept bytes, also after its own restart | A replayed Join request cannot make the gateway issue a second salt |
| A Join request with a known `request_nonce` but other fields is rejected | One transaction has one request |
| A replacement pending attempt must have a higher frame-counter part of `request_nonce`; an equal or lower counter is rejected. An erased counter requires explicit deletion of the registry reservation before re-enrolment. | A delayed request cannot replace the current attempt or obtain another salt for a retired nonce |
| After Join complete the gateway rejects that `request_nonce` | A finished transaction cannot restart |
| The node accepts Join accept only for its current `request_nonce`, stores the first one it accepts in its provisional configuration, and ignores later ones, even with a valid tag and another salt | A late accept cannot switch a transaction in progress |
| The node then accepts only Join complete under the K_mac of that salt | The transaction finishes under the keys it fixed |
| `request_nonce` is 64 bits: the node's 32-bit frame counter, then 32 bits of entropy; the node takes a new frame counter value for every pairing attempt | A repeated `request_nonce` lets a recorded Join accept pass the factory-key tag and hand the node an old salt |
| The frame counter part makes `request_nonce` unique for as long as the [counter reserve](#counter-persistence) survives | Uniqueness does not rest on the entropy source |
| The entropy part is mandatory | An EEPROM erase restarts the counter while the factory key in USERROW stays; then only the entropy keeps `request_nonce` from repeating |
| The entropy source, such as jitter of the 32 kHz oscillator against the main clock, is measured and checked on the chip before use; `micros()` does not qualify | Right after power-up `micros()` is nearly deterministic |

`PairingTransaction` operates on a registry-owned UID slot through an atomic,
committed blob interface. Its 123-byte snapshot holds state, both transport
headers, exact Join request/accept bytes, derived keys, generation and CRC-32;
it holds no factory key. A failed write or mismatched readback blocks responses
until reload. Corrupt data requires explicit deletion, never a fresh transaction
created implicitly from the damaged record.

Before committing Active, its bound adapter ensures the initial bound belongs
to the pending keys. Retrying after an interrupted Active commit must preserve
an already prepared bound. An Active transaction answers matching confirms
without calling that adapter or writing the record.

`RegistryPairing` commits the UID reservation, exact transaction snapshot and
derived keys in one registry blob. Each secure record owns a stable replay slot,
independent of registry order. New pending keys forget that slot before their
commit; confirm checks key ownership and preserves a prepared Paired bound.
Registry write or read-back failure blocks pairing until reload; corruption
blocks the registry instead of selecting an older transaction. Backup preserves
the transaction and keys but excludes bounds.

`RadioSecurityPairing` saves the first matching accept as Provisional and fixes
its salt, assignment and nonce across restarts. It authenticates complete with
that salt and saves Active before allowing operational use. A failed EEPROM
readback blocks confirm and complete until reload. The caller allocates the
request counter, qualifies entropy and derives the session MAC from the saved
salt.

## Counter persistence

The node never reuses a counter under its keys, also after a restart: with CTR a reused counter reuses the keystream. Under valid keys the counter never decreases; any state in which that cannot be shown discards the keys or stops transmission.

| Element | Rule |
| --- | --- |
| Node counter | 32 bits in RAM |
| Reserve | EEPROM holds a limit R. The node uses counters below R only. To go further it first writes a new R; after a restart it continues from R, skipping the values it did not use. |
| N | Each reservation adds 1024: about 1.4 writes a day at one report a minute, plus one per boot that transmits. A `counter_floor` catch-up reserves floor + 1024 in the same single write. |
| Lazy reservation | After a restart the node writes its reservation right before its first transmission (report, Join request, or Command ready), not at boot. A reboot loop that never reaches a transmission writes nothing; one that does is bounded by the delayed first report after a brown-out (ROADMAP item 2). |
| Lifetime | R belongs to the node, not to its keys: re-pairing and a network factory reset keep it. Only an EEPROM erase restarts it. |

Reserve record:

| Rule | Detail |
| --- | --- |
| Format | Two slots, each R (4 bytes, little-endian) and CRC-16 (2 bytes), at the addresses in [EEPROM](#eeprom). No wear ring: writes are rare. |
| Selection | The larger R among the valid slots. R only grows, so no generation number is needed. |
| Commit | The new R goes to the slot holding the smaller or an invalid value, CRC last, then is read back. Counters at or above the old R are used only after the read-back matches. A torn write leaves the old, smaller R, which no used counter has passed. |
| Both slots erased, no network configuration | A new node: the counter starts at 0 |
| Both slots invalid, network configuration present | The counter cannot be recovered: the node discards its keys and needs re-pairing. New keys from a new salt make a counter restarting at 0 safe. |
| Overflow | When a reservation would pass 2³² − 1 the node stops transmitting for good. Re-pairing cannot help, because R is kept across it; recovery is the same as for a damaged EEPROM: erase it and pair again. That is about 8 000 years of one report a minute, or about 4 million transmitting boots. |

Telemetry does not depend on the gateway's reverse link, so a node that cannot hear ACKs still delivers its reports; after a gateway restart it does so after at most M new frames.

### Gateway bound

Guarantee: every published frame of a node has a higher counter than every frame of that node published before it, across gateway restarts and backup restores.

The gateway keeps a counter reserve instead of rewriting its registry for every accepted frame. The registry holds up to 12,560 bytes in a 64 KiB NVS partition ([STORAGE.md](../gateway/STORAGE.md)).

| Element | Rule |
| --- | --- |
| Bound H | Per node, a counter is accepted only once a successfully stored H covers it. When the counter c to accept is above H, the gateway first writes H = c + M − 1, saturating at 2³² − 1; a large jump needs one write, not a loop. The reserve then covers exactly M values, c included. |
| M | Per node, the number of frames it accepted from that node in the last 24 hours, between 1 and 256, and 256 while it has no such history: about one write a day per node, at most about six for one reporting every minute |
| Storage | One small NVS blob with presence/state bitmaps and 4 bytes per registry slot, separate from the registry; NVS journals replacements |
| Record states | A present record holds H, or marks a node paired but with no frame accepted yet. Valid keys without a present record mean an [unactivated](#backup-restore) node. |
| Initialization | The gateway writes the "no frame accepted yet" state once, when the node turns active under new keys: under fresh keys every counter is fresh, so the first frame is accepted at any counter. A repeated Join confirm only repeats Join complete, as today ([STORAGE.md](../gateway/STORAGE.md)), and never touches the record, because resetting a stored H would make old counters acceptable again. |
| Floor | In RAM: the last accepted counter + 1. After a restart it starts at H + 1, because every frame accepted before the restart, and therefore every replay, has a counter at most H. |
| Exhausted range | H equal to 2³² − 1 means the range under these keys is used up: H + 1 is never computed, the gateway rejects every frame under the keys and sends no `counter_floor`. Recovery is an EEPROM erase and pairing again, as for the node's [overflow](#counter-persistence). The node stops earlier, because its reservation cannot pass 2³² − 1, but the gateway does not rely on that. |
| Catch-up | A frame with a valid tag below the floor is not published. The gateway answers with an ACK carrying `counter_floor`, under the tag bound to that frame's counter. The node, which accepts it only for its current frame and only within 256 of its counter, moves to the floor and sends the report again under the new counter. A genuine floor is at most M above the node's counter: a retransmission of an accepted c after a restart meets the floor c + M. |
| One-way nodes | A node that cannot hear the floor passes it on its own after at most M new frames; after a lost ACK the node retries every 15 minutes and later hourly, so this gives no calendar bound |
| Abuse | The floor cannot be forged (tag) or replayed (bound to one frame counter). A floor answered to an attacker's replay reaches no node that is waiting for it. |

`GatewayReplay` provides the bound codec, single-blob store and authenticated-frame
policy. `Guard::inspect()` runs after tag verification: only `Accept` permits
publication, while `Duplicate` permits re-ACK or a cached command reply.
`AcceptanceHistory` counts accepted frames in the trailing 24 hours using
monotonic uptime; M is 256 until a full day has been observed, then 1..256.

The blob must validate at load. Missing or invalid bounds make every node with
existing keys unactivated. A replacement is committed and read back before
acceptance. If power is lost before publication, either the previous committed
bound or the new bound covers all previously published frames. A write or
read-back failure blocks acceptance until reload. Fresh-key initialization
refuses to reset a present record.

### Backup restore

A backup keeps node keys and metadata, so restoring needs no re-pairing. It does not keep the bound records: a stored bound would let frames accepted after the backup, or after an earlier restore of the same backup, look fresh again. A restored node therefore has valid keys and no bound record, which makes it unactivated; the state survives any number of gateway restarts until activation stores H.

The Activation report is a frame kind that carries its challenge in clear, so the gateway can check it whether or not it still remembers the challenge:

```text
Node                                           Gateway (node unactivated)
  |-- report (c0) ----------------------------->|  valid tag, nothing published
  |<-- ACK, challenge G ------------------------|  G pending in RAM
  |-- Activation report (c1, G, telemetry) ---->|  G matches: stores H, then activates
  |<-- ACK, tag(c1) ----------------------------|
```

| Situation | Behaviour |
| --- | --- |
| Unactivated node, any frame with a valid tag other than an Activation report carrying the pending challenge | Nothing published, no Command ready answered. The ACK carries `challenge`: the pending one if the gateway has one, otherwise a new random 8-byte value, which becomes pending. Repeats get the same pending challenge. |
| Activation report whose challenge matches the pending one | The gateway stores H covering c1, activates the node, publishes the telemetry, and ACKs. Storing comes first. |
| Activation report with any other challenge, such as one issued before a gateway restart | Not published. The ACK carries the current pending challenge, or a new one; the node builds a new Activation report under a new counter. |
| ACK of an accepted Activation report lost | The node repeats the same frame. The node is now active, so ordinary rules apply: the same counter is re-ACKed. |
| Gateway restarts after storing H but before its ACK | The repeated Activation report has a valid tag and a counter at most H, so the gateway answers with an authenticated `counter_floor`, as for any frame below the floor |
| The node received `challenge` | It keeps the challenge and sends its next report as an Activation report, repeating that exact frame until it is acknowledged |
| Restoring the same backup again | Every node is unactivated again and passes a new challenge; no replay window between restores |
| BACKUP.md | States that each node needs a working reverse link to be activated after a restore: a one-way node cannot receive its challenge |

Activation costs 8 bytes in one frame per node per restore; ordinary reports do not grow.

## EEPROM

Network configuration slot, 26 bytes: magic (2), schema (1), generation (1), provisioning state (1), node ID (1), gateway ID (1), network ID (1), salt (8), `request_nonce` (8), CRC-16 (2).

| Address | Size | Record |
| --- | ---: | --- |
| `0x00` | 26 | Network configuration A |
| `0x1A` | 6 | Reserve A |
| `0x20` | 26 | Network configuration B |
| `0x3A` | 6 | Reserve B |
| `0x40` | 192 | Profile-owned, unchanged |

The reserve fits the space the installation key leaves, so no image moves its profile storage, including `counter_reed`, whose profile area is full.

`RadioSecurityStorage.h` exposes the V3 configuration and frame counter stores.
Configuration saves and resets are read back before success is returned. A
counter load returns `KeysMustBeDiscarded` when both reserves are invalid with a
network configuration present; `take()` then fails until the configuration and
its keys are discarded. `take()` reserves lazily and returns a new counter for
new content; retransmissions retain the original frame. `advanceToFloor()`
accepts a floor only for the last issued counter, within 256 values, and stores
floor + 1024 before making the floor available.

The `size_security_storage` bench environment compiles these stores with the
Arduino EEPROM backend: 2534 bytes of Flash and 32 bytes of static RAM, including
the bench and Arduino core. It does not write EEPROM with its default zero sink.

## Flash

`lib_deps` points to the `no-readallregs` branch of the strange-v/RFM69 fork, and release environments define `RF69_NO_READALLREGS`. Without it `readAllRegs()` links `Serial`, UART0, and its interrupt vectors into images that never use them.

ATtiny3224 has 32 768 bytes of Flash and 3072 bytes of SRAM. Builds use
megaTinyCore 2.6.7 and the 4 MHz internal clock.

| V3 profile | Flash | Free Flash | Static RAM |
| --- | ---: | ---: | ---: |
| `counter_reed` | 20 831 | 11 937 | 764 |
| `climate_tmp112` | 20 822 | 11 946 | 815 |
| `binary_sht40` | 20 685 | 12 083 | 799 |
| `binary` | 18 809 | 13 959 | 710 |
| `counter_reed_debug` | 22 850 | 9918 | 915 |
| `climate_tmp112_debug` | 22 718 | 10 050 | 966 |
| `binary_sht40_debug` | 23 050 | 9718 | 950 |
| `binary_debug` | 20 812 | 11 956 | 861 |

Run `node/scripts/probe_v3_size.ps1 -Environment counter_reed` from PowerShell;
the other release environment names select their own composition roots.
The script builds the actual release environment, including RTC entropy
collection, and inspects its ELF without uploading. The MCU's memory limits
apply unchanged. Flash includes `.text`, `.rodata` and `.data`.

Static RAM leaves at least 2106 bytes for stack and dynamic state. Debug images
paint unused SRAM during startup and report `stkfree <bytes>` when the minimum
remaining heap-to-stack gap decreases. The painter is absent from release images.
Peak stack usage remains unverified until measured on hardware.

## ATtiny3224 hardware verification

| Check | Required evidence |
| --- | --- |
| Supply measurement | Compare VDD/10 ADC readings with a meter across the battery voltage range; verify the 2000 mV transmit gate |
| Sleep and wake | PPK2 current measurements with RTC PIT enabled and ADC disabled, including a radio transmission |
| Pairing entropy | RTC/TCB0 captures across devices, supply voltages, temperature and power cycles; integrated capture restores PIT operation |
| Provisioning | SerialUPDI USERROW write/readback, factory key preservation and EEPROM reservations after reset |
| Stack | Record debug UART `stkfree` minimum during pairing, activation, telemetry and command sessions; check gateway task minima in `/ui/status` |

### Radio timing and replay reservations

Read the authenticated `/ui/status` endpoint after pairing, normal reports,
`READ_INFO`, node reboot, gateway reboot and backup restore. Its `bench` object
contains receive-processing time, replay NVS write time, IRQ-to-ACK time and
ESP32 task stack minima; field definitions are in [API.md](../gateway/API.md#liveness-and-status).

Exercise replay reservations with node reboots, which skip the node's reserved
counter range, and repeat with a full registry. Record `max_receive_us`,
`max_reservation_write_us`, `max_ack_us`, `acks_over_40ms`, reservation failures
and node retry/ACK results. Verify activation after gateway reboot and restore,
and rejection of captured stale frames and frames with a damaged tag.

IRQ-to-ACK timing includes scheduling, replay read-back and completed radio
transmission. Missed-IRQ recovery is excluded from `acks_measured`; node reception
and tag verification require the actual radio test. Debug UART and stack scanning
affect the node image; check the 40 ms deadline with release firmware too.

## Later

| Item | Note |
| --- | --- |
| `.noinit` counter carry-over | `.noinit` RAM survives watchdog, software, and UPDI resets but not power-on or, reliably, brown-out. Keyed on `RSTCTRL.RSTFR` and guarded by a 32-bit magic plus an inverted copy, it skips the reservation before the first transmission after a non-power reset; a doubtful check falls back to the stored reserve. |
| One-way link indicator | Reports without `downlink_rssi` and with `radio_fallback` set mean the node does not hear the gateway's ACKs |
| Decimal meter entry | The gateway stores a pulse value per node and converts a reading such as 01234,56 into a `SET_COUNT` |
| Calibration on the gateway | Revisit with MQTT; with the full count on air it is only an offset |
| `constexpr` constructors, one dual-slot EEPROM store | Flash reserve, about 0.4 KB each |
