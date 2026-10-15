# Node crypto and entropy bench

Isolated measurements behind [radio security](../../protocol/SECURITY.md) on the node. Complete node images are measured in the [node README](../README.md#memory).

## Crypto

ATtiny1614 at 4 MHz; the bench environments target ATtiny3224. The AES implementation computes each round in one pass per output column ("fused round"), with an expanded key and an S-box generated at compile time.

| Cost on ATtiny1614 at 4 MHz                                  |                                                                                      Value |
| ------------------------------------------------------------ | -----------------------------------------------------------------------------------------: |
| Crypto Flash, including key derivation                       |                                                                                    1 380 B |
| RAM                                                          | 384 B (two 176 B expanded keys, 32 B CMAC subkeys); the derivation reuses the CMAC context |
| One AES block                                                |                                                                               6 791 cycles |
| Report: CTR + CMAC over one block                            |                                                                              14 594 cycles |
| ACK tag check, one block                                     |                                                                               7 360 cycles |
| Each further tag block                                       |                                                                         about 7 050 cycles |
| Key derivation from the factory key and setup, once per boot |                                                                35 534 cycles, about 8.9 ms |

A report with its ACK costs about 5.5 ms and 9 µC, about 4 % of a 215 µC report. The node runs the crypto while the RFM69 sleeps: in standby (about 1.25 mA) the radio would nearly double that cost.

AES-CCM with one key was measured as the alternative: 176 B of RAM, but 35 062 cycles per report and 21 069 per ACK, about 14 ms and 22 µC (10 %), because it spends five AES blocks on a short frame and three on an ACK. Its 1 386 B of Flash exclude key derivation.

| Environment             | Links                                                                                           | Flash | Static RAM |
| ----------------------- | ----------------------------------------------------------------------------------------------- | ----: | ---------: |
| `size_empty`            | Arduino core only                                                                               |   492 |            |
| `size_security`         | Key derivation, report sealing, ACK verification, join authentication, gateway reply decryption |  2755 |        472 |
| `size_security_frames`  | Node pairing and command codecs, counter-bound reply verification, shared crypto                |  3726 |        456 |
| `size_security_pairing` | Node pairing, EEPROM configuration, key setup                                                   |  4792 |        505 |
| `size_security_storage` | Configuration and frame counter stores with the Arduino EEPROM backend and a zero sink          |  2588 |         26 |
| `size_security_entropy` | Entropy extractor and conditioning over a synthetic source                                      |  1882 |        219 |

Sizes include the Arduino core; `size_security_storage` is measured on ATtiny3224, the others on ATtiny1614. None of these images contains the complete radio, ACK, storage or profile paths.

The bench holds the measured implementations and their known-answer tests (FIPS-197 C.1, SP 800-38A F.5.1, RFC 4493, SP 800-38C example 1, RFC 3610 packet vector 1):

| Check                           | Command, from `node/crypto_bench`                                                                                         |
| ------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| Known answers on the host (WSL) | `g++ -std=c++17 -DOSK_AES_FUSED test_host/main.cpp lib/OskCrypto/src/OskCrypto.cpp -o /tmp/osk_crypto && /tmp/osk_crypto` |
| Flash cost                      | `pio run -e size_empty -e size_two_keys -e size_ccm`, then subtract `size_empty`                                          |
| Cycles on the chip              | `pio run -e bench_fused -t upload`, then read COM12 at 9600 baud                                                          |

## RTC entropy

ATtiny1614, internal main clock at 4 MHz, internal RTC clock at 32 kHz.
`src/entropy.cpp` routes RTC overflow every 64 RTC ticks through EVSYS to
TCB0 frequency capture. Each sample is a hardware measurement in main-clock
cycles. Interrupts are disabled during each 128-sample burst; UART output
follows the burst at 9600 baud on COM12.

| 4096 samples in `entropy-capture-samples.txt` |               Value |
| --------------------------------------------- | ------------------: |
| Capture range                                 |    7760–7774 cycles |
| Distinct values                               |                  15 |
| LSB ones                                      |      2064 (50.39 %) |
| LSB transitions                               |                2039 |
| Longest LSB run                               |                  19 |
| Maximum absolute LSB correlation, lags 1–32   |             0.02765 |
| Von Neumann output                            | 1028 bits, 512 ones |

ATtiny3224, same bench at room temperature: runs of 4096 samples at 3.3 V and
1.8 V, each beginning with the first burst after a UPDI reset or a power cycle.
The 1.8 V run has BOD disabled, since the 1.8 V BOD level holds the chip in
reset at that supply.

| ATtiny3224                                  | 3.3 V, UPDI resets, 12 288 samples | 3.3 V, power cycle, 4096 samples | 1.8 V, UPDI reset, 4096 samples |
| ------------------------------------------- | ---------------------------------: | -------------------------------: | ------------------------------: |
| Capture range                               |                   7821–7838 cycles |                 7825–7840 cycles |                7861–7876 cycles |
| Distinct values                             |                                 18 |                               16 |                              16 |
| Most common value                           |                             14.6 % |                           16.0 % |                          18.7 % |
| LSB ones                                    |                     6069 (49.39 %) |                   2061 (50.32 %) |                  1993 (48.66 %) |
| LSB transitions                             |                               6155 |                             2012 |                            2067 |
| Longest LSB run                             |                                 12 |                               12 |                              10 |
| Maximum absolute LSB correlation, lags 1–32 |       0.0246 (0.028–0.043 per run) |                           0.0366 |                          0.0243 |
| Von Neumann output                          |               3089 bits, 1543 ones |               991 bits, 491 ones |             1033 bits, 537 ones |
| 512-sample bursts passing the health guards |                           24 of 24 |                           8 of 8 |                          8 of 8 |

The first samples after a power cycle sit 5–7 cycles above the later mean;
at 1.8 V the mean rises by about 40 cycles (0.5 %). Neither shift affects the
LSB statistics. Start-up sequences differ between runs.

The source is accepted for pairing on these captures. They cover one device per
chip type at room temperature and do not establish a minimum entropy bound.

The node runtime collects 512 samples through RTC overflow events and TCB0
frequency capture before each Join request. It pauses PIT ticks during the
burst, restores the sleep-clock registers, and fails on a bounded capture
timeout. The integrated driver and bench target ATtiny3224 at 4 MHz, with
TCB1 providing `millis()`. PIT recovery after the integrated capture is
unverified on hardware.

`PairingEntropy.h` processes 512 capture LSBs in non-overlapping pairs,
discards equal pairs and keeps 64 Von Neumann bits. It validates the entire
burst before returning material; capture errors, insufficient output or failed
health checks leave the caller's output unchanged.

| Health guard                         | Limit      |
| ------------------------------------ | ---------- |
| Raw identical-bit run                | At most 32 |
| Ones in each 128-sample window       | 32–96      |
| Identical-bit run in retained output | At most 16 |
| Ones in retained 64-bit output       | 16–48      |

These guards detect stuck or strongly biased sequences; they do not prove
independence or a minimum entropy bound. All eight 512-sample bursts in the
capture pass them. A factory-key CMAC conditions the material with domain byte
`0x03`, UID and frame counter; its low 32 bits provide the entropy word. The
counter remains the separate low word of `request_nonce`.

| Action                           | Command from `node/crypto_bench`                       |
| -------------------------------- | ------------------------------------------------------ |
| Build and upload                 | `pio run -e entropy -t upload`                         |
| Analyze captured decimal samples | `node analyze_entropy.mjs entropy-capture-samples.txt` |

Software polling of each RTC tick quantizes the result: in an 8192-sample run,
7853 samples were 121 cycles and the LSB was constant. Hardware capture avoids
that polling-loop quantization.

### Optional second source: RFM69 noise RSSI

Not implemented. A `next(uint16_t&)` source over SPI plugs into
`pairing_entropy::collect` unchanged; its 8 bytes would join the RTC material in
the CMAC input, and pairing would require both sources to pass their checks.

| Register                      | Use                                                                                                                              |
| ----------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| RSSI (0x23 `RssiStart`, 0x24) | Candidate: noise-floor LSBs in RX without a signal; trigger each measurement, since back-to-back reads repeat the averaged value |
| FEI/AFC (0x1E, 0x1F–0x22)     | Behaviour on noise without a preamble is undocumented; measure first                                                             |
| Temperature (0x4E, 0x4F)      | No entropy at 1 °C resolution; record it as a qualification condition                                                            |

A nearby transmitter can push RSSI off the noise floor and bias it, so RSSI is
not a sole source. Qualification would compare antenna against a 50 Ω load and
against `radio_flood` at short range. The demodulated data (DIO2) and the crystal
CLKOUT (DIO5) are not wired to the MCU; only DIO0 is.
