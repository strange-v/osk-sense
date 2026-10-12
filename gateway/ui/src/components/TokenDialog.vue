<script setup lang="ts">
// Creates one API token. It deliberately does not display the generated
// secret: a dialog can be dismissed by a stray click on the backdrop, and the
// gateway cannot produce that value a second time. The secret is handed to the
// card, which shows it in the page until it is explicitly dismissed.
import { computed, reactive, ref } from 'vue'
import { api, errorCode } from '../api/client'
import type { ApiToken, CreatedApiToken } from '../api/types'
import Modal from './Modal.vue'
import { TOKEN_NAME_MAX_BYTES, findTokenNamed, tokenDraftError } from '../utils/tokens'

const props = defineProps<{ tokens: ApiToken[] }>()
const emit = defineEmits<{ close: []; created: [token: CreatedApiToken] }>()

const draft = reactive<{ name: string }>({ name: '' })
const busy = ref(false)
const failure = ref('')

const problem = computed(() => tokenDraftError(draft, props.tokens))
// The gateway accepts duplicate names, so this is a warning, not a block --
// but two identical rows in the list are impossible to tell apart later.
const duplicate = computed(
  () => draft.name !== '' && findTokenNamed(props.tokens, draft.name) !== undefined,
)

async function create() {
  if (problem.value) return
  busy.value = true
  failure.value = ''
  try {
    emit('created', await api.createToken(draft.name))
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    busy.value = false
  }
}
</script>

<template>
  <Modal
    :title="$t('tokens.addTitle')"
    @close="emit('close')"
  >
    <div class="form-stack">
      <label>
        <span>{{ $t('tokens.name') }}</span>
        <input
          v-model.trim="draft.name"
          :maxlength="TOKEN_NAME_MAX_BYTES"
          autocapitalize="none"
          autocomplete="off"
          spellcheck="false"
        />
        <small>{{ $t('tokens.nameHint') }}</small>
      </label>

      <p class="scope-note">{{ $t('tokens.grantNote') }}</p>

      <div
        v-if="problem"
        class="notice"
      >
        {{ $t(`tokens.problem.${problem}`) }}
      </div>
      <div
        v-else-if="duplicate"
        class="notice"
      >
        {{ $t('tokens.duplicateName') }}
      </div>
      <div
        v-if="failure"
        class="notice error"
      >
        {{ $t(`error.${failure}`) }}
      </div>

      <div class="modal-actions">
        <button
          class="button secondary"
          type="button"
          @click="emit('close')"
        >
          {{ $t('common.cancel') }}
        </button>
        <button
          class="button primary"
          :disabled="busy || problem !== ''"
          type="button"
          @click="create"
        >
          {{ busy ? $t('tokens.creating') : $t('tokens.create') }}
        </button>
      </div>
    </div>
  </Modal>
</template>

<style scoped>
.scope-note {
  margin: 0;
  color: var(--ink-soft);
  font-size: var(--text-sm);
}
</style>
