<script setup lang="ts">
// The gateway's power policy for one node. Power is desired state: the gateway
// asks for a level in its acknowledgements and the node reports the level it
// actually uses, which the card's tiles show -- this is only the request. A
// choice applies the moment it is made, so there is no draft for the card's
// periodic refresh to overwrite.
import { computed, ref, watch } from 'vue'
import { api, errorCode, isAdmin } from '../api/client'
import type { GatewayNode } from '../api/types'

const props = defineProps<{ node: GatewayNode }>()
const emit = defineEmits<{ updated: [] }>()

// Up to this many choices (Automatic plus the levels) sit side by side. The
// protocol allows a ceiling of 31, and a node with a high one gets a list.
const SEGMENTED_MAX_OPTIONS = 6
const AUTO = 'auto'

const ceiling = computed(() => props.node.max_power_level ?? 0)
const levels = computed(() => Array.from({ length: ceiling.value + 1 }, (_, level) => level))
const segmented = computed(() => levels.value.length + 1 <= SEGMENTED_MAX_OPTIONS)
// The policy as one value for both controls: 'auto' or a fixed level.
const current = computed(() =>
  props.node.power_policy === 'fixed' && props.node.fixed_power_level !== undefined
    ? String(props.node.fixed_power_level)
    : AUTO,
)
// The choice just sent, shown until the refreshed record carries it.
const requested = ref<string | null>(null)
const shown = computed(() => requested.value ?? current.value)
const saving = ref(false)
const failure = ref('')

watch(current, () => {
  requested.value = null
})

async function choose(value: string) {
  if (saving.value || value === shown.value) return
  saving.value = true
  failure.value = ''
  requested.value = value
  try {
    await api.setPowerPolicy(
      props.node.node_id,
      value === AUTO
        ? { power_policy: 'auto' }
        : { power_policy: 'fixed', fixed_power_level: Number(value) },
    )
    emit('updated')
  } catch (error) {
    requested.value = null
    failure.value = errorCode(error)
  } finally {
    saving.value = false
  }
}

function onSelect(event: Event) {
  const select = event.target as HTMLSelectElement
  choose(select.value)
  // A refused choice must not stay selected in the list.
  select.value = shown.value
}
</script>

<template>
  <section class="node-radio">
    <div class="power-row">
      <span class="power-label">{{ $t('radio.policy') }}</span>
      <template v-if="isAdmin">
        <div
          v-if="segmented"
          class="segmented"
          role="group"
          :aria-label="$t('radio.policy')"
        >
          <button
            type="button"
            :class="{ active: shown === AUTO }"
            :aria-pressed="shown === AUTO"
            :disabled="saving"
            @click="choose(AUTO)"
          >
            {{ $t('radio.auto') }}
          </button>
          <button
            v-for="level in levels"
            :key="level"
            type="button"
            :class="{ active: shown === String(level) }"
            :aria-pressed="shown === String(level)"
            :disabled="saving"
            @click="choose(String(level))"
          >
            {{ level }}
          </button>
        </div>
        <select
          v-else
          class="select compact"
          :value="shown"
          :disabled="saving"
          :aria-label="$t('radio.policy')"
          @change="onSelect"
        >
          <option :value="AUTO">{{ $t('radio.auto') }}</option>
          <option
            v-for="level in levels"
            :key="level"
            :value="String(level)"
          >
            {{ $t('radio.levelOption', { level }) }}
          </option>
        </select>
      </template>
      <strong
        v-else
        class="power-value"
        >{{
          current === AUTO ? $t('radio.autoValue') : $t('radio.fixedValue', { level: current })
        }}</strong
      >
    </div>
    <p class="radio-hint">
      {{ shown === AUTO ? $t('radio.autoHint') : $t('radio.fixedHint', { max: ceiling }) }}
    </p>
    <div
      v-if="failure"
      class="notice error"
    >
      {{ $t(`error.${failure}`) }}
    </div>
  </section>
</template>

<style scoped>
.node-radio {
  display: grid;
  gap: var(--space-2);
  padding-top: var(--space-4);
  border-top: 1px solid var(--line);
}
.node-radio .notice {
  margin: 0;
}
.power-row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: var(--space-2) var(--space-4);
}
.power-label {
  color: var(--ink-soft);
  font-size: var(--text-sm);
  font-weight: 500;
}
.power-row .segmented button {
  font-size: var(--text-sm);
}
.power-row .segmented button:disabled {
  cursor: progress;
}
.power-value {
  font-size: var(--text-sm);
}
.radio-hint {
  color: var(--muted);
  font-size: var(--text-sm);
}
</style>
