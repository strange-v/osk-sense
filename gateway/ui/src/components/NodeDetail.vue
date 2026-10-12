<script setup lang="ts">
// Everything known about one node, and what an administrator can change on it.
// It exists as a dialog rather than a row menu because device_uid has nowhere
// to live in the table. Every control applies on its own -- there is no
// card-wide Save. The list keeps refreshing the record while the card is open,
// so the only draft here is the name, and only while it is being edited.
import { computed, nextTick, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { api, errorCode, isAdmin } from '../api/client'
import type { GatewayNode } from '../api/types'
import Icon from './Icon.vue'
import Modal from './Modal.vue'
import NodeCommands from './NodeCommands.vue'
import NodeRadio from './NodeRadio.vue'
import SignalBars from './SignalBars.vue'
import {
  byteLength,
  isMeasuredRssi,
  lastSeen,
  nodeName,
  signal,
  truncateToBytes,
  volts,
} from '../utils/format'
import { groupHex } from '../utils/hexCredentials'

const props = defineProps<{ node: GatewayNode }>()
// `changed` closes the dialog; `updated` refreshes it in place.
const emit = defineEmits<{ close: []; changed: []; updated: [] }>()

const { t } = useI18n()
const DISPLAY_NAME_MAX_BYTES = 48
// The grouping of the pairing field, and so of the printed label: the UID can
// be checked against either at a glance.
const UID_GROUP_SIZE = 4

const editing = ref(false)
const draftName = ref('')
const saving = ref(false)
const renameFailure = ref('')
const nameInput = ref<HTMLInputElement | null>(null)
// The name just saved, shown until the refreshed record carries it, so the
// title does not flick back to the old one in between.
const savedName = ref<string | null>(null)

const deleting = ref(false)
const confirmingDelete = ref(false)
const failure = ref('')

const active = computed(() => props.node.state === 'active')
const title = computed(() =>
  nodeName(t, {
    node_id: props.node.node_id,
    display_name: savedName.value ?? props.node.display_name,
  }),
)
const uid = computed(() => groupHex(props.node.device_uid, UID_GROUP_SIZE))

// The API accepts an empty name (it clears one), and the field never takes
// more than the ceiling, so only control characters make a draft invalid.
const controlCharacters = /\p{Cc}/u
const invalidName = computed(() => controlCharacters.test(draftName.value))

// The level the node reports against its ceiling, and apart from it the level
// the gateway asks for: the wanted level is never presented as the real one.
const reported = computed(() => props.node.tx_power_level !== undefined)
const adjusting = computed(
  () =>
    props.node.tx_power_target !== undefined &&
    reported.value &&
    props.node.tx_power_target !== props.node.tx_power_level,
)

watch(
  () => props.node.display_name,
  () => {
    savedName.value = null
  },
)

// Another node is another card. A refresh of the same one is not, and must
// leave an edit or a pending confirmation alone.
watch(
  () => props.node.node_id,
  () => {
    editing.value = false
    savedName.value = null
    confirmingDelete.value = false
    renameFailure.value = ''
    failure.value = ''
  },
)

async function startEditing() {
  draftName.value = savedName.value ?? props.node.display_name
  renameFailure.value = ''
  editing.value = true
  await nextTick()
  nameInput.value?.select()
}

// Holds the name to its byte ceiling as it is typed, the way maxlength does
// for characters: whatever of the text just typed or pasted does not fit is
// dropped, and the text after the caret is kept.
function onNameInput(event: Event) {
  if ((event as InputEvent).isComposing) return
  const input = event.target as HTMLInputElement
  const caret = input.selectionStart ?? input.value.length
  const suffix = input.value.slice(caret)
  const prefix = truncateToBytes(
    input.value.slice(0, caret),
    Math.max(0, DISPLAY_NAME_MAX_BYTES - byteLength(suffix)),
  )
  const fitted = prefix + suffix
  if (fitted !== input.value) {
    input.value = fitted
    input.setSelectionRange(prefix.length, prefix.length)
  }
  draftName.value = fitted
}

function stopEditing() {
  editing.value = false
  renameFailure.value = ''
}

async function rename() {
  if (invalidName.value || saving.value) return
  if (draftName.value === (savedName.value ?? props.node.display_name)) {
    stopEditing()
    return
  }
  saving.value = true
  renameFailure.value = ''
  try {
    await api.renameNode(props.node.node_id, draftName.value)
    savedName.value = draftName.value
    stopEditing()
    emit('updated')
  } catch (error) {
    renameFailure.value = errorCode(error)
  } finally {
    saving.value = false
  }
}

async function remove() {
  deleting.value = true
  failure.value = ''
  try {
    await api.deleteNode(props.node.node_id)
    emit('changed')
    emit('close')
  } catch (error) {
    failure.value = errorCode(error)
    confirmingDelete.value = false
  } finally {
    deleting.value = false
  }
}
</script>

<template>
  <Modal
    :title="title"
    @close="emit('close')"
  >
    <template #title>
      <form
        v-if="editing"
        class="title-edit"
        @submit.prevent="rename"
      >
        <input
          ref="nameInput"
          :value="draftName"
          :class="{ invalid: invalidName }"
          :placeholder="$t('nodes.unnamed', { id: node.node_id })"
          :aria-label="$t('nodes.displayName')"
          :aria-invalid="invalidName || undefined"
          @input="onNameInput"
          @compositionend="onNameInput"
          @keydown.esc.prevent.stop="stopEditing"
        />
        <button
          class="icon-button"
          type="submit"
          :disabled="saving || invalidName"
          :aria-label="$t('nodes.save')"
        >
          <Icon name="check" />
        </button>
      </form>
      <div
        v-else
        class="title-view"
      >
        <h2>{{ title }}</h2>
        <button
          v-if="isAdmin"
          class="icon-button"
          type="button"
          :aria-label="$t('nodes.rename')"
          @click="startEditing"
        >
          <Icon name="pencil" />
        </button>
      </div>
    </template>

    <div class="node-card">
      <div
        v-if="renameFailure"
        class="notice error"
      >
        {{ $t(`error.${renameFailure}`) }}
      </div>

      <dl class="simple-details">
        <div>
          <dt>{{ $t('nodes.deviceUid') }}</dt>
          <dd class="node-uid">{{ uid }}</dd>
        </div>
        <div>
          <dt>{{ $t('nodes.columnState') }}</dt>
          <dd>
            <span
              class="inline-status"
              :class="{ warning: !active }"
              >{{ $t(`nodes.state.${node.state}`) }}</span
            >
          </dd>
        </div>
        <div>
          <dt>ID</dt>
          <dd>{{ node.node_id }}</dd>
        </div>
        <div>
          <dt>{{ $t('nodes.profileLabel') }}</dt>
          <dd>{{ node.profile_id }}</dd>
        </div>
        <div>
          <dt>{{ $t('nodes.columnFirmware') }}</dt>
          <dd>{{ node.firmware || '—' }}</dd>
        </div>
        <div>
          <dt>{{ $t('nodes.columnLastSeen') }}</dt>
          <dd>{{ lastSeen(t, node.last_seen_at_ms) }}</dd>
        </div>
        <div>
          <dt>{{ $t('nodes.columnSupply') }}</dt>
          <dd>{{ volts(node.supply_mv) }}</dd>
        </div>
        <div>
          <dt>{{ $t('radio.uplink') }}</dt>
          <dd>
            <span class="node-signal"
              ><SignalBars
                v-if="isMeasuredRssi(node.rssi)"
                :rssi="node.rssi"
              />{{ signal(node.rssi) }}</span
            >
          </dd>
        </div>
        <div>
          <dt>{{ $t('radio.downlink') }}</dt>
          <dd>
            <span class="node-signal"
              ><SignalBars
                v-if="isMeasuredRssi(node.downlink_rssi)"
                :rssi="node.downlink_rssi"
              />{{ signal(node.downlink_rssi) }}</span
            >
          </dd>
        </div>
        <div>
          <dt>{{ $t('radio.level') }}</dt>
          <dd>
            {{
              reported
                ? $t('radio.levelValue', {
                    level: node.tx_power_level,
                    max: node.max_power_level ?? 0,
                  })
                : '—'
            }}
            <small
              v-if="adjusting"
              class="radio-target"
              :title="$t('radio.targetHint')"
              >{{ $t('radio.targetValue', { level: node.tx_power_target }) }}</small
            >
          </dd>
        </div>
      </dl>
      <div
        v-if="node.radio_fallback"
        class="notice radio-fallback"
      >
        {{ $t('radio.fallback') }}
      </div>

      <NodeRadio
        v-if="active"
        :node="node"
        @updated="emit('updated')"
      />
      <NodeCommands
        v-if="active"
        :key="node.node_id"
        :node="node"
      />

      <!-- A div, not <footer>: layout.css styles that element as the app footer. -->
      <div
        v-if="isAdmin"
        class="node-footer"
      >
        <div
          v-if="failure"
          class="notice error"
        >
          {{ $t(`error.${failure}`) }}
        </div>
        <template v-if="confirmingDelete">
          <div class="notice error confirm-delete">
            <span>
              <strong>{{ $t('nodes.deleteConfirmTitle', { name: title }) }}</strong>
              {{ $t('nodes.deleteConfirmHint') }}
            </span>
          </div>
          <div class="modal-actions">
            <button
              class="button secondary"
              type="button"
              @click="confirmingDelete = false"
            >
              {{ $t('common.cancel') }}
            </button>
            <button
              class="button danger"
              :disabled="deleting"
              type="button"
              @click="remove"
            >
              {{ deleting ? $t('nodes.deleting') : $t('nodes.deleteConfirm') }}
            </button>
          </div>
        </template>
        <button
          v-else
          class="button danger-text compact"
          type="button"
          @click="confirmingDelete = true"
        >
          <Icon name="delete" /> {{ $t('nodes.delete') }}
        </button>
      </div>
      <p
        v-else
        class="coming-soon"
      >
        {{ $t('nodes.adminOnly') }}
      </p>
    </div>
  </Modal>
</template>

<style scoped>
.title-view,
.title-edit {
  flex: 1;
  min-width: 0;
  display: flex;
  align-items: center;
  gap: var(--space-1);
}
.title-view h2 {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.title-edit input {
  min-height: 36px;
  padding-block: var(--space-1);
  font-size: var(--text-lg);
  font-weight: 600;
}
.title-edit input.invalid {
  border-color: var(--danger);
}

.node-card {
  display: grid;
  gap: var(--space-4);
}
.node-card .notice {
  margin: 0;
}

.node-uid {
  font-family: var(--font-mono);
  font-size: var(--text-sm);
  letter-spacing: 0.04em;
  user-select: all;
}
.node-signal {
  display: inline-flex;
  align-items: center;
  gap: var(--space-2);
}
.radio-target {
  display: block;
  color: var(--warning);
  font-size: var(--text-xs);
  font-weight: 500;
}

.node-footer {
  display: grid;
  gap: var(--space-3);
  padding-top: var(--space-4);
  border-top: 1px solid var(--line);
}
.node-footer > .button {
  justify-self: start;
  margin-left: calc(var(--space-3) * -1);
}
.confirm-delete strong {
  display: block;
  margin-bottom: 2px;
}
.node-footer .modal-actions {
  justify-content: space-between;
  margin-top: 0;
}
</style>
