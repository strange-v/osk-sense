<script setup lang="ts">
// A node's waiting command or the result of its last one, and the form that
// queues the next. A sleeping node fetches its command in a radio session of
// its own, so the card polls until the node has answered. Commands are rare,
// so the form stays folded behind a button rather than lengthening every card.
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { api, errorCode, isAdmin } from '../api/client'
import type { CommandType, GatewayNode, NodeCommand } from '../api/types'
import {
  commandArguments,
  commandTakesValue,
  commandValue,
  parseCommandValue,
  supportedCommands,
} from '../utils/commands'

const props = defineProps<{ node: GatewayNode }>()
const { t } = useI18n()

// The gateway answers the listing from RAM, and a button press on the node
// should show its outcome without a noticeable wait.
const POLL_MS = 3_000

const types = computed(() => supportedCommands(props.node.profile_id))
const command = ref<NodeCommand | null>(null)
const selectedType = ref<CommandType>(types.value[0] ?? 'set_count')
const composing = ref(false)
const draftValue = ref('')
const valueInput = ref<HTMLInputElement | null>(null)
const busy = ref(false)
const failure = ref('')
let timer: number | undefined
let pollInFlight = false

const waiting = computed(
  () => command.value?.state === 'pending' || command.value?.state === 'delivered',
)
const takesValue = computed(() => commandTakesValue(selectedType.value))
const parsedValue = computed(() => parseCommandValue(selectedType.value, draftValue.value))
const invalidValue = computed(() => draftValue.value.trim() !== '' && parsedValue.value === null)
const ready = computed(() => !takesValue.value || parsedValue.value !== null)

const stateLabel = computed(() => {
  const current = command.value
  if (!current) return ''
  return current.state === 'completed'
    ? t(`commands.status.${current.status ?? 'applied'}`)
    : t(`commands.state.${current.state}`)
})

const stateHint = computed(() => {
  const current = command.value
  if (!current) return ''
  if (current.state === 'pending') return t('commands.pendingHint')
  if (current.state === 'delivered') return t('commands.deliveredHint')
  if (current.status === 'unsupported') return t('commands.unsupportedHint')
  if (current.status === 'invalid_argument') return t('commands.invalidHint')
  const result = current.result
  if (result && 'count' in result)
    return t('commands.countResult', { previous: result.previous_count, count: result.count })
  if (result && 'firmware' in result) {
    return t('commands.infoResult', {
      firmware: result.firmware,
      profile: result.profile_id,
      level: result.max_power_level,
    })
  }
  return ''
})

async function load(quiet = false) {
  try {
    const list = await (quiet ? api.poll.commands() : api.commands())
    command.value = list.commands.find((entry) => entry.node_id === props.node.node_id) ?? null
  } catch (error) {
    // A missed poll keeps the last state; only the first read reports.
    if (!quiet) failure.value = errorCode(error)
  }
}

async function poll() {
  if (!waiting.value || pollInFlight) return
  pollInFlight = true
  try {
    await load(true)
  } finally {
    pollInFlight = false
  }
}

async function compose() {
  draftValue.value = ''
  failure.value = ''
  composing.value = true
  await nextTick()
  valueInput.value?.focus()
}

async function send() {
  if (!ready.value || busy.value) return
  busy.value = true
  failure.value = ''
  try {
    command.value = await api.queueCommand({
      node_id: props.node.node_id,
      type: selectedType.value,
      arguments: commandArguments(selectedType.value, parsedValue.value),
    })
    draftValue.value = ''
    composing.value = false
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    busy.value = false
  }
}

async function cancel() {
  busy.value = true
  failure.value = ''
  try {
    await api.cancelCommand(props.node.node_id)
    await load()
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    busy.value = false
  }
}

watch(selectedType, () => {
  draftValue.value = ''
})
onMounted(() => {
  load()
  timer = window.setInterval(poll, POLL_MS)
})
onBeforeUnmount(() => window.clearInterval(timer))
</script>

<template>
  <section
    v-if="types.length && (isAdmin || command)"
    class="form-stack node-commands"
  >
    <div class="commands-head">
      <h3>{{ $t('commands.title') }}</h3>
      <button
        v-if="isAdmin && !waiting && !composing"
        class="button secondary compact"
        type="button"
        @click="compose"
      >
        {{ $t('commands.compose') }}
      </button>
    </div>

    <div
      v-if="command"
      class="command-state"
    >
      <div class="command-line">
        <strong>{{
          $t(`commands.describe.${command.type}`, { value: commandValue(command) })
        }}</strong>
        <span
          class="inline-status"
          :class="{ warning: command.state !== 'completed' || command.status !== 'applied' }"
          >{{ stateLabel }}</span
        >
      </div>
      <p
        v-if="stateHint"
        class="command-hint"
      >
        {{ stateHint }}
      </p>
      <div
        v-if="waiting && isAdmin"
        class="command-actions"
      >
        <small v-if="command.state === 'delivered'">{{ $t('commands.cancelDeliveredHint') }}</small>
        <button
          class="button secondary compact"
          :disabled="busy"
          type="button"
          @click="cancel"
        >
          {{ busy ? $t('commands.cancelling') : $t('commands.cancel') }}
        </button>
      </div>
    </div>

    <template v-if="isAdmin && !waiting && composing">
      <div
        v-if="types.length > 1"
        class="segmented"
        role="group"
        :aria-label="$t('commands.kind')"
      >
        <button
          v-for="type in types"
          :key="type"
          type="button"
          :class="{ active: selectedType === type }"
          @click="selectedType = type"
        >
          {{ $t(`commands.type.${type}`) }}
        </button>
      </div>
      <label v-if="takesValue">
        <span>{{ $t(`commands.field.${selectedType}`) }}</span>
        <input
          ref="valueInput"
          v-model="draftValue"
          inputmode="numeric"
          :aria-label="$t(`commands.field.${selectedType}`)"
          @keydown.enter="send"
        />
        <small :class="{ invalid: invalidValue }">{{
          invalidValue
            ? $t(`commands.invalid.${selectedType}`)
            : $t(`commands.hint.${selectedType}`)
        }}</small>
      </label>
      <div
        v-else
        class="command-state"
      >
        <strong>{{ $t(`commands.describe.${selectedType}`) }}</strong>
        <p class="command-hint">{{ $t(`commands.hint.${selectedType}`) }}</p>
      </div>
      <div class="compose-actions">
        <button
          class="button secondary"
          type="button"
          @click="composing = false"
        >
          {{ $t('common.cancel') }}
        </button>
        <button
          class="button primary"
          :disabled="busy || !ready"
          type="button"
          @click="send"
        >
          {{ busy ? $t('commands.sending') : $t('commands.send') }}
        </button>
      </div>
    </template>

    <div
      v-if="failure"
      class="notice error"
    >
      {{ $t(`error.${failure}`) }}
    </div>
  </section>
</template>

<style scoped>
.node-commands {
  gap: var(--space-3);
  padding-top: var(--space-4);
  border-top: 1px solid var(--line);
}
.node-commands .notice {
  margin: 0;
}
.commands-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--space-3);
  min-height: 32px;
}
.commands-head h3 {
  color: var(--ink-soft);
  font-size: var(--text-sm);
  font-weight: 500;
}
.node-commands .segmented {
  justify-self: start;
}
.command-state {
  display: grid;
  gap: var(--space-2);
}
.command-line {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--space-3);
}
.command-hint {
  color: var(--ink-soft);
  font-size: var(--text-sm);
}
.command-actions,
.compose-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: var(--space-2);
}
.command-actions small {
  margin-right: auto;
  color: var(--muted);
  font-size: var(--text-sm);
}
</style>
