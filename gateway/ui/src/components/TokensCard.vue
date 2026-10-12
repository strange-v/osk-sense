<script setup lang="ts">
// Long-lived API keys: what an integration authenticates with to read this
// gateway. Like the users card, every action here reaches the gateway the
// moment it is pressed.
//
// The one thing this card must not get wrong is the generated secret. The
// gateway returns it once and stores only its SHA-256, so a value lost to a
// stray click is gone for good. It is therefore shown in the page rather than
// in a dialog, next to a copy button, and stays until it is explicitly
// dismissed. It is never written to storage.
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { api, errorCode } from '../api/client'
import type { ApiToken, CreatedApiToken } from '../api/types'
import { dateTime } from '../utils/format'
import { TOKEN_LIMIT, canAddToken } from '../utils/tokens'
import { copyText } from '../utils/clipboard'
import Icon from './Icon.vue'
import Modal from './Modal.vue'
import TokenDialog from './TokenDialog.vue'

const { locale } = useI18n()

const tokens = ref<ApiToken[]>([])
const loading = ref(true)
const failure = ref('')
const adding = ref(false)
const removing = ref<ApiToken | null>(null)
const deleteBusy = ref(false)
const created = ref<CreatedApiToken | null>(null)
const copied = ref(false)
const copyFailed = ref(false)

const full = computed(() => !canAddToken(tokens.value))

async function load() {
  loading.value = true
  try {
    tokens.value = (await api.tokens()).tokens
    failure.value = ''
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    loading.value = false
  }
}

async function afterCreate(token: CreatedApiToken) {
  adding.value = false
  copied.value = false
  copyFailed.value = false
  created.value = token
  await load()
}

function copy() {
  if (!created.value) return
  copied.value = copyText(created.value.token)
  copyFailed.value = !copied.value
}

async function remove() {
  if (!removing.value) return
  deleteBusy.value = true
  failure.value = ''
  try {
    await api.deleteToken(removing.value.id)
    // Revoking the key that is on screen makes its secret worthless, and
    // leaving it there reads as if it still worked.
    if (created.value?.id === removing.value.id) created.value = null
    removing.value = null
    await load()
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    deleteBusy.value = false
  }
}

onMounted(load)
// Nothing keeps the secret alive past this screen.
onUnmounted(() => {
  created.value = null
})
</script>

<template>
  <section class="panel admin-card">
    <header class="panel-heading">
      <div>
        <h2>{{ $t('tokens.title') }}</h2>
        <p>{{ $t('tokens.hint') }}</p>
      </div>
      <span class="panel-count">{{ tokens.length }} / {{ TOKEN_LIMIT }}</span>
    </header>

    <div
      v-if="created"
      class="revealed-token notice"
    >
      <div>
        <strong>{{ $t('tokens.createdTitle', { name: created.name }) }}</strong>
        <span>{{ $t('tokens.createdWarning') }}</span>
        <code class="generated-token">{{ created.token }}</code>
        <div class="revealed-actions">
          <button
            class="button secondary"
            type="button"
            @click="copy"
          >
            {{ copied ? $t('tokens.copied') : $t('tokens.copy') }}
          </button>
          <span
            v-if="copyFailed"
            class="notice error"
            role="alert"
            >{{ $t('tokens.copyFailed') }}</span
          >
          <button
            class="button primary"
            type="button"
            @click="created = null"
          >
            {{ $t('tokens.saved') }}
          </button>
        </div>
      </div>
    </div>

    <div
      v-if="loading"
      class="empty-state"
    >
      <span class="loader"></span>
    </div>
    <template v-else>
      <ul
        v-if="tokens.length"
        class="token-list"
      >
        <li
          v-for="token in tokens"
          :key="token.id"
        >
          <span
            class="token-symbol"
            aria-hidden="true"
            ><Icon name="key"
          /></span>
          <span class="token-name">
            <strong>{{ token.name }}</strong>
            <small>{{ dateTime(locale, token.created_at_ms) }}</small>
          </span>
          <span class="token-actions">
            <button
              class="icon-button danger"
              type="button"
              :aria-label="$t('tokens.deleteToken', { name: token.name })"
              :title="$t('tokens.delete')"
              @click="removing = token"
            >
              <Icon name="delete" />
            </button>
          </span>
        </li>
      </ul>
      <p
        v-else
        class="capacity-note"
      >
        {{ $t('tokens.empty') }}
      </p>

      <div
        v-if="failure"
        class="notice error"
      >
        {{ $t(`error.${failure}`) }}
      </div>
      <p
        v-if="full"
        class="capacity-note"
      >
        {{ $t('tokens.capacity', { limit: TOKEN_LIMIT }) }}
      </p>
      <button
        v-else
        class="button primary"
        type="button"
        @click="adding = true"
      >
        <Icon name="plus" /> {{ $t('tokens.add') }}
      </button>
    </template>

    <TokenDialog
      v-if="adding"
      :tokens="tokens"
      @close="adding = false"
      @created="afterCreate"
    />

    <Modal
      v-if="removing"
      :title="$t('tokens.deleteTitle')"
      :busy="deleteBusy ? $t('tokens.deleting') : undefined"
      @close="removing = null"
    >
      <div class="form-stack">
        <div class="notice error">
          <span>
            <strong>{{ $t('tokens.deleteConfirm', { name: removing.name }) }}</strong>
            {{ $t('tokens.deleteHint') }}
          </span>
        </div>
        <div class="modal-actions">
          <button
            class="button secondary"
            type="button"
            @click="removing = null"
          >
            {{ $t('common.cancel') }}
          </button>
          <button
            class="button danger"
            :disabled="deleteBusy"
            type="button"
            @click="remove"
          >
            {{ $t('tokens.delete') }}
          </button>
        </div>
      </div>
    </Modal>
  </section>
</template>

<style scoped>
.token-list {
  display: grid;
  gap: var(--space-1);
  margin: 0;
  padding: 0;
  list-style: none;
}
.token-list li {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--space-2) var(--space-3);
  padding: var(--space-2) 0;
}
.token-list li + li {
  box-shadow: 0 -1px 0 var(--line);
}
.token-symbol {
  width: 30px;
  height: 30px;
  display: grid;
  place-items: center;
  border-radius: var(--radius-md);
  color: var(--primary);
  background: var(--primary-soft);
}
.token-name {
  flex: 1 1 8rem;
  min-width: 0;
  display: grid;
}
.token-name strong {
  font-size: var(--text-md);
  font-weight: 500;
}
.token-name small {
  color: var(--muted);
  font-size: var(--text-xs);
}
.token-actions {
  display: flex;
  margin-left: auto;
}
.token-actions .icon-button {
  width: 30px;
  height: 30px;
  font-size: var(--text-md);
}
.capacity-note {
  color: var(--ink-soft);
  font-size: var(--text-sm);
}
/* Not a dialog: the secret cannot be recovered, so it must not be possible to
   lose it by clicking beside a modal. */
.revealed-token {
  margin-bottom: 0;
}
.revealed-token > div {
  display: grid;
}
.revealed-token strong {
  color: var(--warning);
}
.revealed-actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2);
}
</style>
