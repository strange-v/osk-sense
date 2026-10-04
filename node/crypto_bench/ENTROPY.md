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
temperature. Pairing integration needs an entropy extractor and runtime health
checks.

| Action | Command from `node/crypto_bench` |
| --- | --- |
| Build and upload | `pio run -e entropy -t upload` |
| Analyze captured decimal samples | `node analyze_entropy.mjs entropy-capture-samples.txt` |

Software polling of each RTC tick quantizes the result: in an 8192-sample run,
7853 samples were 121 cycles and the LSB was constant. Hardware capture avoids
that polling-loop quantization.
