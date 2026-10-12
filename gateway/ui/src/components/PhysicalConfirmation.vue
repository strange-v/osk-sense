<script setup lang="ts">
// Sits right above a setup submit button and explains why it is disabled:
// the gateway accepts setup only while its physical window is open.
import { computed } from 'vue'
import type { SetupStatus } from '../api/types'
import { countdown } from '../utils/format'

const ENDING_SECONDS = 30

const props = defineProps<{ status: SetupStatus | null }>()
const open = computed(() =>
  Boolean(props.status?.physical_window_active && !props.status.recovery_required),
)
const remaining = computed(() => props.status?.remaining_seconds ?? 0)
</script>

<template>
  <div
    class="physical-status"
    :class="{ open, ending: open && remaining <= ENDING_SECONDS }"
  >
    <span
      class="pulse"
      aria-hidden="true"
    ></span>
    <div v-if="open">
      <strong>{{ $t('setup.physicalOpenTitle') }}</strong>
      <p>{{ $t('setup.physicalOpen', { time: countdown(remaining) }) }}</p>
    </div>
    <div v-else>
      <strong>{{ $t('setup.physicalTitle') }}</strong>
      <p>{{ $t('setup.physicalClosed') }}</p>
    </div>
  </div>
</template>
