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

This characterizes one device in one operating condition. It does not establish
a minimum entropy bound across devices, power cycles, supply voltage or
temperature. Source qualification remains pending.

The node runtime collects 512 samples through RTC overflow events and TCB0
frequency capture before each Join request. It pauses PIT ticks during the
burst, restores the sleep-clock registers, and fails on a bounded capture
timeout. This integrated driver has been compiled for ATtiny1614; hardware
verification of the integrated image remains pending.

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
source: 1882 bytes of Flash and 219 bytes of static RAM, including the Arduino
core and CMAC. It collects no hardware entropy.

| Action | Command from `node/crypto_bench` |
| --- | --- |
| Build and upload | `pio run -e entropy -t upload` |
| Analyze captured decimal samples | `node analyze_entropy.mjs entropy-capture-samples.txt` |

Software polling of each RTC tick quantizes the result: in an 8192-sample run,
7853 samples were 121 cycles and the LSB was constant. Hardware capture avoids
that polling-loop quantization.
