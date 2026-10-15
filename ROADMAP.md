# OSK Sense roadmap

This file contains only unfinished work, most important first. Protocol and storage decisions belong in their reference documents.

## Milestones

1. **Gateway:** run a multi-day soak with every node and a WebSocket client, logged by `status_logger.py`: no resets, flat heap, no drops. Decode the core dump of any reset.
2. **Gateway and nodes:** verify the security fixes on hardware. Gateway: pairing on both boards with the SAR entropy source on, and Ethernet unaffected. Node: a forged frame ahead of the gateway's reply no longer ends a command session; a corrupted newest counter reserve recovers above every used counter.
3. **Nodes:** find out on the bench whether a depleted CR2032 makes a node reboot-loop on brown-out, transmitting at its ceiling after every boot and jamming the shared channel. Check which reset flag a battery insertion sets. If the loop exists, delay the first report after a brown-out reset through the retry backoff, and flag on the gateway a node that keeps sending first-after-boot reports (ceiling level, no downlink RSSI).
4. **Nodes:** qualify the pairing entropy source beyond the current captures ([crypto bench](node/crypto_bench/README.md#rtc-entropy)): several chips of each type, the temperature range, cold and warm starts, and a failing source; establish a minimum-entropy bound.
5. **Nodes and gateway:** check that every receive loop (node ACK, pairing and command windows; gateway queues and replay NVS writes) bounds what forged frames can cost in time, battery and writes.
6. **Gateway:** fill the registry and record replay reservation writes and failures.
7. **Nodes:** PPK2 current measurements of the images other than `binary_sht40` on ATtiny3224, with RTC PIT enabled and ADC disabled, including a radio transmission.
8. **Nodes and gateway:** after LowPowerLab/RFM69 merges the receive-length fix, pin both to that upstream commit or release instead of the strange-v fork ([node README](node/README.md#memory)).
9. **Nodes:** settle the −85…−75 dBm automatic-control window from field data.
10. **Nodes** _(optional)_: measure output power per level on our boards and replace the one-dB-per-level assumption of automatic control. Transmit current per level is in [POWER.md](node/POWER.md#radio-power-levels).
11. **Nodes** _(to consider)_: a `restart` command, for a sensor that stays invalid after bus recovery. The node answers first and restarts after the result is acknowledged; a redelivery costs one extra restart. Only the node handler and the command type must exist before nodes are installed; the gateway queue and the UI can follow.
12. **Gateway** _(optional)_: a DIY onboarding mode based on a random commissioning code scoped to one gateway installation. It must be opt-in and provisioned into custom nodes by the builder; it must never become a product-wide key or replace unique factory credentials for pre-provisioned nodes.

## Ideas

| Item                                                 | Note                                                                                                                                                                                                                                                                                                                  |
| ---------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `.noinit` counter carry-over                         | `.noinit` RAM survives watchdog, software, and UPDI resets but not power-on or, reliably, brown-out. Keyed on `RSTCTRL.RSTFR` and guarded by a 32-bit magic plus an inverted copy, it skips the reservation before the first transmission after a non-power reset; a doubtful check falls back to the stored reserve. |
| One-way link indicator                               | Reports without `downlink_rssi` and with `radio_fallback` set mean the node does not hear the gateway's ACKs                                                                                                                                                                                                          |
| Decimal meter entry                                  | The gateway stores a pulse value per node and converts a reading such as 01234,56 into a `SET_COUNT`                                                                                                                                                                                                                  |
| Calibration on the gateway                           | Revisit with MQTT; with the full count on air it is only an offset                                                                                                                                                                                                                                                    |
| `constexpr` constructors, one dual-slot EEPROM store | Flash reserve, about 0.4 KB each                                                                                                                                                                                                                                                                                      |
