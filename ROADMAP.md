# OSK Sense roadmap

This file contains only unfinished milestones, most important first. Protocol and storage decisions belong in their reference documents.

1. **Gateway:** run a multi-day soak with every node and a WebSocket client, logged by `status_logger.py`: no resets, flat heap, no drops. Decode the core dump of any reset.
2. **Nodes:** find out on the bench whether a depleted CR2032 makes a node reboot-loop on brown-out, transmitting at its ceiling after every boot and jamming the shared channel. Check which reset flag a battery insertion sets. If the loop exists, delay the first report after a brown-out reset through the retry backoff, and flag on the gateway a node that keeps sending first-after-boot reports (ceiling level, no downlink RSSI).
3. **Nodes:** settle the −85…−75 dBm automatic-control window from field data.
4. **Nodes** *(optional)*: measure output power per level on our boards and replace the one-dB-per-level assumption of automatic control. Transmit current per level is in [POWER.md](node/POWER.md#radio-power-levels).
5. **Nodes** *(to consider)*: a `restart` command, for a sensor that stays invalid after bus recovery. The node answers first and restarts after the result is acknowledged; a redelivery costs one extra restart. Only the node handler and the command type must exist before nodes are installed; the gateway queue and the UI can follow.
6. **Gateway** *(optional)*: a DIY onboarding mode based on a random commissioning code scoped to one gateway installation. It must be opt-in and provisioned into custom nodes by the builder; it must never become a product-wide key or replace unique factory credentials for pre-provisioned nodes.
