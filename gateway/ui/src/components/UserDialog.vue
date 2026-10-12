<script setup lang="ts">
// Add or edit one local account. The same dialog serves both: on edit the
// password field is optional and means "replace it", left blank it keeps the
// stored one, which is why there is no separate "change password" flow.
import { computed, reactive, ref } from 'vue'
import { api, errorCode, sessionUser } from '../api/client'
import type { GatewayUser } from '../api/types'
import Modal from './Modal.vue'
import {
  PASSWORD_MAX_BYTES,
  USERNAME_MAX,
  isLastEnabledAdmin,
  userDraftError,
} from '../utils/users'

const props = defineProps<{ users: GatewayUser[]; user?: GatewayUser }>()
const emit = defineEmits<{ close: []; saved: [selfRevoked: boolean] }>()

const editing = computed(() => props.user !== undefined)
const draft = reactive({
  username: props.user?.username ?? '',
  password: '',
  role: props.user?.role ?? ('viewer' as 'admin' | 'viewer'),
  enabled: props.user?.enabled ?? true,
})

const busy = ref(false)
const failure = ref('')

// The gateway will not let the last enabled admin lose the role or be
// switched off, so those two controls are locked rather than left to fail.
const lockedAsLastAdmin = computed(
  () => props.user !== undefined && isLastEnabledAdmin(props.users, props.user),
)
const problem = computed(() => userDraftError(draft, props.users, props.user))

// A successful PUT revokes every session of that user -- including this
// browser's when you are editing yourself.
const editingSelf = computed(
  () => props.user !== undefined && props.user.id === sessionUser.value?.id,
)

async function save() {
  if (problem.value) return
  busy.value = true
  failure.value = ''
  try {
    if (props.user) {
      await api.updateUser(props.user.id, {
        username: draft.username,
        role: draft.role,
        enabled: draft.enabled,
        ...(draft.password ? { password: draft.password } : {}),
      })
    } else {
      await api.createUser({
        username: draft.username,
        password: draft.password,
        role: draft.role,
        enabled: draft.enabled,
      })
    }
    emit('saved', editingSelf.value)
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    busy.value = false
    draft.password = ''
  }
}
</script>

<template>
  <Modal
    :title="editing ? $t('users.editTitle') : $t('users.addTitle')"
    :busy="busy ? $t('users.saving') : undefined"
    @close="emit('close')"
  >
    <div class="form-stack">
      <label>
        <span>{{ $t('users.username') }}</span>
        <input
          v-model.trim="draft.username"
          :maxlength="USERNAME_MAX"
          autocapitalize="none"
          autocomplete="off"
          spellcheck="false"
        />
        <small>{{ $t('users.usernameHint') }}</small>
      </label>

      <label>
        <span>{{ editing ? $t('users.newPassword') : $t('users.password') }}</span>
        <input
          v-model="draft.password"
          type="password"
          :maxlength="PASSWORD_MAX_BYTES"
          autocomplete="new-password"
        />
        <small>{{ editing ? $t('users.newPasswordHint') : $t('users.passwordHint') }}</small>
      </label>

      <label>
        <span>{{ $t('users.role') }}</span>
        <select
          class="select"
          v-model="draft.role"
          :disabled="lockedAsLastAdmin"
        >
          <option value="admin">{{ $t('users.roleAdmin') }}</option>
          <option value="viewer">{{ $t('users.roleViewer') }}</option>
        </select>
        <small>{{
          draft.role === 'admin' ? $t('users.roleAdminHint') : $t('users.roleViewerHint')
        }}</small>
      </label>

      <label class="toggle-row">
        <input
          v-model="draft.enabled"
          type="checkbox"
          :disabled="lockedAsLastAdmin"
        />
        <span>{{ $t('users.enabled') }}</span>
      </label>

      <div
        v-if="lockedAsLastAdmin"
        class="notice"
      >
        {{ $t('users.lastAdminLocked') }}
      </div>
      <div
        v-else-if="problem"
        class="notice"
      >
        {{ $t(`users.problem.${problem}`) }}
      </div>
      <div
        v-if="editingSelf"
        class="notice"
      >
        {{ $t('users.selfEditWarning') }}
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
          @click="save"
        >
          {{ busy ? $t('users.saving') : editing ? $t('users.save') : $t('users.add') }}
        </button>
      </div>
    </div>
  </Modal>
</template>
