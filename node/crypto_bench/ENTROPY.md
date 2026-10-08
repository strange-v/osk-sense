# RTC entropy measurements

ATtiny1614, internal main clock at 4 MHz, internal RTC clock at 32 kHz.
`src/entropy.cpp` routes RTC overflow every 64 RTC ticks through EVSYS to
TCB0 frequency capture. Each sample is a hardware measurement in main-clock
cycles. Interrupts are disabled during each 128-sample burst; UART output
follows the burst at 9600 baud on COM12.

| 4096 samples in `entropy-capture-samples.txt` | Value |
| --- | ---: |
| Capture range | 7760–7774 cycles |
| Distinct values | 15 |
| LSB ones | 2064 (50.39 %) |
| LSB transitions | 2039 |
| Longest LSB run | 19 |
| Maximum absolute LSB correlation, lags 1–32 | 0.02765 |
| Von Neumann output | 1028 bits, 512 ones |

ATtiny3224, same bench at room temperature: runs of 4096 samples at 3.3 V and
1.8 V, each beginning with the first burst after a UPDI reset or a power cycle.
The 1.8 V run has BOD disabled, since the 1.8 V BOD level holds the chip in
reset at that supply.

| ATtiny3224 | 3.3 V, UPDI resets, 12 288 samples | 3.3 V, power cycle, 4096 samples | 1.8 V, UPDI reset, 4096 samples |
| --- | ---: | ---: | ---: |
| Capture range | 7821–7838 cycles | 7825–7840 cycles | 7861–7876 cycles |
| Distinct values | 18 | 16 | 16 |
| Most common value | 14.6 % | 16.0 % | 18.7 % |
| LSB ones | 6069 (49.39 %) | 2061 (50.32 %) | 1993 (48.66 %) |
| LSB transitions | 6155 | 2012 | 2067 |
| Longest LSB run | 12 | 12 | 10 |
| Maximum absolute LSB correlation, lags 1–32 | 0.0246 (0.028–0.043 per run) | 0.0366 | 0.0243 |
| Von Neumann output | 3089 bits, 1543 ones | 991 bits, 491 ones | 1033 bits, 537 ones |
| 512-sample bursts passing the health guards | 24 of 24 | 8 of 8 | 8 of 8 |

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

| Health guard | Limit |
| --- | --- |
| Raw identical-bit run | At most 32 |
| Ones in each 128-sample window | 32–96 |
| Identical-bit run in retained output | At most 16 |
| Ones in retained 64-bit output | 16–48 |

These guards detect stuck or strongly biased sequences; they do not prove
independence or a minimum entropy bound. All eight 512-sample bursts in the
capture pass them. A factory-key CMAC conditions the material with domain byte
`0x03`, UID and frame counter; its low 32 bits provide the entropy word. The
counter remains the separate low word of `request_nonce`.

`size_security_entropy` links the extractor and conditioning with a synthetic
source. The ATtiny1614 measurement is 1882 bytes of Flash and 219 bytes of
static RAM, including the Arduino core and CMAC. It collects no hardware entropy.

| Action | Command from `node/crypto_bench` |
| --- | --- |
| Build and upload | `pio run -e entropy -t upload` |
| Analyze captured decimal samples | `node analyze_entropy.mjs entropy-capture-samples.txt` |

Software polling of each RTC tick quantizes the result: in an 8192-sample run,
7853 samples were 121 cycles and the LSB was constant. Hardware capture avoids
that polling-loop quantization.

## Optional second source: RFM69 noise RSSI

Not implemented. A `next(uint16_t&)` source over SPI plugs into
`pairing_entropy::collect` unchanged; its 8 bytes would join the RTC material in
the CMAC input, and pairing would require both sources to pass their checks.

| Register | Use |
| --- | --- |
| RSSI (0x23 `RssiStart`, 0x24) | Candidate: noise-floor LSBs in RX without a signal; trigger each measurement, since back-to-back reads repeat the averaged value |
| FEI/AFC (0x1E, 0x1F–0x22) | Behaviour on noise without a preamble is undocumented; measure first |
| Temperature (0x4E, 0x4F) | No entropy at 1 °C resolution; record it as a qualification condition |

A nearby transmitter can push RSSI off the noise floor and bias it, so RSSI is
not a sole source. Qualification would compare antenna against a 50 Ω load and
against `radio_flood` at short range. The demodulated data (DIO2) and the crystal
CLKOUT (DIO5) are not wired to the MCU; only DIO0 is.
