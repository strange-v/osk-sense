# OSK Sense Node firmware

This PlatformIO project produces one statically composed ATtiny3224 image per stable telemetry profile. Shared code owns commissioning, radio, EEPROM, power, the wake clock, and command sessions; profiles own acquisition, report scheduling, and payload encoding.

## Builds

| Environment | Profile | Measurements | Tick |
| --- | ---: | --- | ---: |
| `climate_tmp112` | 2 | TMP112 temperature, supply voltage | 32 s |
| `climate_tmp112_debug` | 2 | same, with UART diagnostics | 32 s |
| `binary` | 5 | binary state, supply voltage | 250 ms |
| `binary_debug` | 5 | same, with UART diagnostics | 250 ms |
| `binary_sht40` | 7 | binary state, SHT40 temperature and humidity, supply voltage | 250 ms |
| `binary_sht40_debug` | 7 | same, with UART diagnostics | 250 ms |
| `counter_reed` | 6 | pulse count, supply voltage | 250 ms |
| `counter_reed_debug` | 6 | same, with UART diagnostics | 250 ms |
| `radio_power_sweep` | diagnostic | supply voltage at power levels 0..31 | 250 ms |
| `radio_power_sweep_button` | diagnostic | one report every 5 s; PA6 selects the power level | 250 ms |
| `radio_flood` | 1 | acknowledged voltage reports back to back, for gateway load tests | 250 ms |

The binary-input image with TMP112 (profile 8) is not implemented yet. An installation decides whether a binary input is a door, a window, or a float switch.

Build or upload one environment:

```powershell
pio run -e climate_tmp112
pio run -e climate_tmp112 -t upload
```

For the SHT40 binary node, build `binary_sht40` or `binary_sht40_debug` and upload the chosen environment. It uses the SHT40 at I2C address `0x44`, the active-low contact on PA5, and a five-minute reporting interval; all three settings are in `platformio.ini`.

Uploading to a paired node keeps its network configuration, because the fuses preserve EEPROM on chip erase. The gateway learns the new firmware version and profile only when an administrator sends the *Read node information* command from the node's card ([PROTOCOL.md](../protocol/PROTOCOL.md#command-types)).

Hardware environments inherit serial UPDI upload on COM11 at 115200 baud and serial monitoring on COM12 at 9600 baud. Only the adapter's RX line is connected to COM12.

`radio_power_sweep` is a bench image for an already commissioned node. It sends one acknowledged voltage telemetry frame every 5 seconds, increasing the RFM69 power level from 0 through 31, then sends nothing until reset. `radio_power_sweep_button` sets `NODE_POWER_SWEEP_BUTTON_LEVEL` and sends a frame every 5 seconds at the selected level. It starts at `NODE_POWER_SWEEP_FIRST_LEVEL`; each PA6 press selects the next level, wrapping to the first after `NODE_POWER_SWEEP_LAST_LEVEL`. The frame's radio state contains the level used for that transmission. Keep the gateway close so every level completes in one attempt. Use a current-capable measurement supply and lower `NODE_POWER_SWEEP_LAST_LEVEL` when the board or its supply must not be exposed to all 32 levels.

`radio_flood` is a gateway load-test image. It joins as profile 1 (voltage) on a short PA6 press, like any unconfigured node, and a 10-second hold resets its network configuration. Once active it sends acknowledged voltage reports without sleeping or backing off, `NODE_FLOOD_INTERVAL_MS` (50 ms) apart, at the fixed level `NODE_FLOOD_POWER_LEVEL` (2); power targets from the gateway are counted, not applied, so set the node to a fixed policy at that level. One exchange takes about 18 ms, so a node sends about 14 reports per second. Every 100 reports it prints `sent=… ack=… tgt=… ms=…` on the UART at 9600 baud. It draws tens of milliamperes continuously: supply it from a bench supply, not a coin cell.

Upload never writes fuses. Write them once per chip before the first upload; every environment uses the same values (4 MHz from the 16 MHz oscillator, BOD 1.8 V in active mode only, EEPROM preserved on chip erase, UPDI pin kept):

```powershell
pio run -e climate_tmp112 -t fuses
```

## Runtime composition

Node radio traffic uses V3 software CTR/CMAC and persistent frame counters.
The gateway authenticates V3 traffic with per-node keys. Release and debug profiles fit the 32 KiB Flash;
sizes and the inspection command are in [V3-SECURITY.md](../protocol/V3-SECURITY.md#flash).

The SOIC-14 pin assignments are PA4 radio CS, PA7 radio IRQ, PA1/PA2/PA3
SPI, PB0/PB1 I2C, PA5 contact, PA6 button, PA0 UPDI and PB2/PB3 UART.
Supply measurement uses the ADC's VDD/10 input against its 1.024 V reference;
the ADC is disabled after measurement. TCB1 provides `millis()`, TCB0 captures
RTC events for pairing entropy, and the RTC PIT provides sleep ticks.
Supply accuracy, sleep current, entropy and stack high-water measurements on
ATtiny3224 hardware remain pending; [POWER.md](POWER.md) measurements are from ATtiny1614.

Each environment compiles exactly one composition root from `src/` through `build_src_filter`. `NodeRuntime<Profile>` owns commissioning, the provisioning button, radio retry backoff, supply-voltage measurement, and sleep. The profile class in `include/Profiles/` owns acquisition, report scheduling, and payload encoding; the contract is documented in `NodeRuntime.h`. Profiles are template parameters, not virtual interfaces, because avr-gcc keeps vtables in RAM.

`NODE_TICK_MS` selects the RTC PIT wake period: 32 s for periodic images, 250 ms for polled inputs.

No input may float in sleep. Each composition root disables the digital input buffer of the pins its image leaves unconnected: the sensor I2C pads on counter and plain binary-input images, the reed pad on climate images, and PB2/PB3 outside debug builds. `NodeRadio` pulls up MISO, which the RFM69 releases while deselected.

Reported supply voltage is the lower of the measurement taken just before transmission and the one taken immediately after the previous transmission, so it reflects battery sag under radio load.

A node transmits only while its resting supply is above `NODE_MIN_TRANSMIT_MILLIVOLTS` (2000 mV); at or below it, reports, joins, and command sessions wait, and the supply is measured again 60 s later. Transmit sag below that level resets the MCU before it returns the radio to standby, and the module then transmits until the supply is exhausted. The bench images `radio_power_sweep` and `radio_flood` have no minimum.

## Factory provisioning

Production firmware contains no shared commissioning key. Each ATtiny3224 must receive a unique 16-byte factory key in its 32-byte USERROW after the common firmware is flashed. Install the QR exporter once and provision a connected node through the same SerialUPDI adapter:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" -m pip install -r scripts/requirements-provisioning.txt
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" scripts/provision_node.py --port COM11
```

The tool reads the 10-byte SIGROW UID, refuses to replace an existing valid record unless `--force` is supplied, generates the key with the operating system CSPRNG, writes and verifies USERROW, then exports a text credential, SVG QR, and `manifest.csv` under the git-ignored `provisioned_nodes/` directory. The QR payload is `web+opensmartkit:pair?v=1&family=sense&uid=<20 HEX>&key=<32 HEX>`. Treat every exported file as a secret manufacturing artifact and back it up outside the repository.

## Implemented runtimes

Every image supports per-node-key UID commissioning, recovery of provisional commissioning, dual-slot network configuration, Vcc measurement, acknowledged telemetry, [retries](#lost-gateway) paced for a lost gateway, and RTC power-down scheduling independent of sleeping `millis()`. The climate image adds TMP112 one-shot measurement; the binary-input images add the PA5 contact state, with SHT40 temperature and humidity on `binary_sht40`; the counter image adds the PA5 pulse input and the wear-levelled counter journal.

The solar/supercapacitor climate policy schedules nominal 60 seconds above 2500 mV and 300 seconds at or below it, by the voltage each report measured, acknowledged or not. The 32-second RTC step yields about 64/320 seconds. Battery-powered climate builds use one compile-time interval and do not persist it.

`*_debug` environments log at 9600 baud on PB2. They preserve the RTC timebase but never call `sleep_cpu()`; idle iterations use a short delay. Do not use them to measure sleep current.

Debug builds enable `NODE_STACK_DIAGNOSTICS`: `stkfree <bytes>` reports the smallest observed free heap-to-stack gap whenever it decreases. Startup paints unused SRAM; reports scan the remaining marker after runtime operations. Record the minimum during pairing, activation, reports and command sessions. Debug UART and instrumentation consume stack, so this measures the debug image; use release images for power measurements.

## Event-node policy

PA5 is the active-low reed/counter input. Each 250 ms tick reads it once, with the pull-up enabled only for that read. A read that disagrees with the confirmed state starts a debounce burst of five equal reads at 1 ms spacing in the same wake-up, so confirmation adds no tick of latency and costs power only on real changes. A LOW or HIGH phase longer than one tick is detected, except while a commissioning receive window or a held button blocks polling.

A counter additionally accepts a new level only after it has persisted for `NODE_COUNTER_MINIMUM_PHASE_MS` (500 ms) of RTC time, because a magnet moving slowly near the pull-in distance makes the reed chatter far longer than the debounce burst. With the 250 ms tick, phases of at least 750 ms are always accepted and phases shorter than 500 ms never are. Binary inputs do not use this filter.

A binary input reports each confirmed change in the same wake-up, even while radio retry backoff is active; if that attempt fails, the report waits for the backoff like any other. State `1` means the contact is open. `binary_sht40` measures temperature and humidity for every report and sends a full frame at least every five minutes after the last acknowledged report. The plain binary-input and counter profiles use a rolling one-hour keep-alive. A counter counts from boot, including before commissioning, persists every confirmed LOW-to-HIGH pulse before any transmission, and reports the absolute count no more than once per minute while dirty.

Measured consumption and the battery budget are in [POWER.md](POWER.md).

PA6 is the active-low provisioning button. A hold is timed with `millis()` while the MCU stays awake, so input polling pauses until the button is released or the hold completes.

| Gesture | Effect |
| --- | --- |
| Released within 10 s | Unconfigured or provisional node: commissioning attempt. Active node: command session. |
| Held for 10 s | Invalidates both network configuration slots and restarts the node unconfigured. USERROW and profile EEPROM, including the counter, are preserved. Refused when the factory credentials are invalid, because the node could never rejoin. |

Without a button press, a node that is not active attempts commissioning at boot, then every 5 minutes for twelve attempts in all; a press starts the series again. After that a provisional node retries hourly, because the gateway answers its Join confirm at any time, and an unconfigured node waits for the button, because the gateway accepts a Join request only inside a pairing window.

The gateway rejects a join request from a UID it still holds as active. To pair a reset node again, also delete it on the gateway (`DELETE /ui/nodes`).

## Radio power

Each image has a transmit power ceiling for its board and supply, `NODE_RADIO_MAX_POWER_LEVEL`: 23 for the supercapacitor-powered `climate_tmp112` and 15 for the CR2032-powered `binary`, `binary_sht40`, and `counter_reed` images. The diagnostic power-sweep images use 23 for commissioning but sweep their configured level range independently. Transmission current and report charge per level are in [POWER.md](POWER.md#radio-power-levels). The node sends the ceiling in Join request and reports its level, fallback flag, and the RSSI of the last acknowledgement in every telemetry frame. That RSSI is sampled when the acknowledgement's sync word matches: the RFM69 keeps measuring the channel after a frame ends, so a read after reception returns anything between the frame and the noise floor ([PROTOCOL.md](../protocol/PROTOCOL.md#radio-power)).

| Event | Level |
| --- | --- |
| Boot and commissioning | Ceiling |
| Acknowledgement carrying a power target | The target clamped to the ceiling, applied at once; fallback cleared |
| Third consecutive report without acknowledgement | Ceiling with the fallback flag, unless already there |

Level and fallback flag are held only in RAM, so adapting the level never writes EEPROM. After a restart the first report goes out at the ceiling, and its acknowledgement returns the node to the level the gateway still wants.

## Lost gateway

| | Battery images | Solar `climate_tmp112` |
| --- | --- | --- |
| Next attempt after an unacknowledged report | 1 min, 5 min, then every 15 min; hourly once the gateway has been silent for 24 h | The next report interval, about 64 s, or 320 s at low charge |
| Transmissions per report | 3; 1 after three consecutive unacknowledged reports, except for a binary state change or a set count | 3; 1 after three consecutive unacknowledged reports |

The solar climate image has no finite charge to spare: a drained supercapacitor recovers at sunrise, and the low-charge interval keeps the node alive through the night.

## Command sessions

An active node opens a command session when its button is short-pressed, or when an authenticated telemetry acknowledgement carries the command-pending flag. It sends Command ready, listens 250 ms for the answer, and repeats the exact frame up to three times with the same counter. Replies must authenticate against that counter before a command runs. Input polling pauses meanwhile, as during a commissioning window.

| Command | Handled by | Effect |
| --- | --- | --- |
| `set_count` | counter image | Pending result, ring write, Applied result ([EEPROM.md](EEPROM.md)); the new count is reported immediately |
| `read_info` | every image | Answers with the image's profile ID, firmware version, and power ceiling; writes nothing |

Sessions started by the flag are limited to one per five minutes, and after one the gateway did not answer, to the battery images' telemetry retry delays ([Lost gateway](#lost-gateway)). A button press always opens a session. An event node reports at least hourly, so the button is the prompt way to reach one.

## Fault recovery

| Fault | Response |
| --- | --- |
| I2C sensor read fails | The TWI0 bus is freed (up to nine SCL clocks, then STOP), the sensor is put back into its sleep state or reset, and the read is repeated once. A report whose read fails twice carries the invalid-value sentinel. |
| Radio or sensor work hangs | A watchdog armed only around startup, reports, command sessions, and commissioning attempts resets the MCU after 8 s. It is off in sleep and on radio-free ticks, which keeps the idle current unchanged. |
| RFM69 fails to initialize | Software restart 5 minutes after boot. A node without network configuration or factory credentials does not restart. |
| RFM69 loses its registers while the MCU runs | Before each report, command session, and commissioning attempt the node reads back the frequency, the AES flag, and the network sync byte. On a mismatch it reinitializes the module, disables hardware AES and reapplies its power level; if that fails too, the exchange is skipped and counts as unacknowledged. |

Debug builds log `tmp fail`, `rst wdt` after a watchdog reset, `rst rf` before a radio restart, and `rf lost` before a register repair.

## Persistence and protocols

Network configuration uses two CRC-protected generation slots. Counter state uses a separate wear-levelled journal, and accepted `SET_COUNT` results have recoverable slots. Exact layouts are in [EEPROM.md](EEPROM.md).

V3 radio frames are defined in [V3-SECURITY.md](../protocol/V3-SECURITY.md); telemetry payloads are defined in [PROTOCOL.md](../protocol/PROTOCOL.md). Profile IDs describe measurements, not installation labels.

## Native tests

```powershell
wsl bash node/scripts/run_native_tests_wsl.sh
```

Remaining implementation work is tracked in [../ROADMAP.md](../ROADMAP.md).
