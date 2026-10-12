<script setup lang="ts">
// Link quality as bar count rather than a raw dBm figure: "-91 dBm" means
// nothing without the scale, "two bars of four" reads instantly. Thresholds
// are for sub-GHz RFM69 links, which run far below Wi-Fi numbers.
import { computed } from 'vue'
import { isMeasuredRssi } from '../utils/format'

const props = defineProps<{ rssi?: number }>()

// Anything outside the module's -128..0 dBm domain is not a measurement. The
// check matters most at 0, which the RFM69 driver uses for "nothing received
// yet": read as a number it is the top of the scale and would light every bar.
const measured = computed(() => isMeasuredRssi(props.rssi))

// Thresholds are anchored on measurements from this network rather than on the
// datasheet, which describes the radio and not the flat it sits in. Reference
// points: -37 node beside the gateway, -73..-77 one room and a wall away
// (balcony), -100 across the flat, through an iron door, to the outer corridor
// -- still delivering packets, and the furthest position that works.
//
// A measured value never falls to zero bars. Receiving a packet at all proves
// a link, so the weakest reading is one bar; "no link" is a different state
// and is already shown as "no telemetry" instead of this component. The bands
// also keep the known-good positions off their own edges, because RSSI swings
// a few dB between packets and a node parked on a boundary flickers.
const level = computed(() => {
  if (!measured.value) return 0
  const rssi = props.rssi as number
  if (rssi >= -65) return 4
  if (rssi >= -82) return 3
  if (rssi >= -93) return 2
  return 1
})
</script>

<template>
  <span
    class="signal-bars"
    :class="{ weak: level <= 1 }"
    :aria-label="measured ? `${rssi} dBm` : undefined"
  >
    <i
      v-for="bar in 4"
      :key="bar"
      :class="{ on: bar <= level }"
    ></i>
  </span>
</template>

<style scoped>
.signal-bars {
  display: inline-flex;
  align-items: flex-end;
  gap: 2px;
  height: 13px;
  color: var(--primary);
}
.signal-bars.weak {
  color: var(--warning);
}
.signal-bars i {
  width: 3px;
  border-radius: 1px;
  background: var(--line-strong);
}
.signal-bars i.on {
  background: currentColor;
}
.signal-bars i:nth-child(1) {
  height: 4px;
}
.signal-bars i:nth-child(2) {
  height: 7px;
}
.signal-bars i:nth-child(3) {
  height: 10px;
}
.signal-bars i:nth-child(4) {
  height: 13px;
}
</style>
