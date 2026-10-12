<script setup lang="ts">
// The local accounts that can sign in to this gateway. Unlike the settings
// form, every action here reaches the gateway immediately -- there is no Save
// button that owns this card.
import { computed, onMounted, ref } from 'vue'
import { api, errorCode, sessionUser } from '../api/client'
import type { GatewayUser } from '../api/types'
import Icon from './Icon.vue'
import Modal from './Modal.vue'
import UserDialog from './UserDialog.vue'
import { USER_LIMIT, canAddUser, isLastEnabledAdmin } from '../utils/users'

const emit = defineEmits<{ signedOut: [] }>()

const users = ref<GatewayUser[]>([])
const loading = ref(true)
const failure = ref('')
const editing = ref<GatewayUser | null>(null)
const adding = ref(false)
const removing = ref<GatewayUser | null>(null)
const deleteBusy = ref(false)

const full = computed(() => !canAddUser(users.value))

async function load() {
  loading.value = true
  try {
    users.value = (await api.users()).users
    failure.value = ''
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    loading.value = false
  }
}

// A mutation on your own account revokes this browser's session, so there is
// nothing left to reload -- hand back to the shell to sign out cleanly rather
// than letting the next request fail with 401.
async function afterSave(selfRevoked: boolean) {
  adding.value = false
  editing.value = null
  if (selfRevoked) {
    emit('signedOut')
    return
  }
  await load()
}

async function remove() {
  if (!removing.value) return
  deleteBusy.value = true
  failure.value = ''
  const self = removing.value.id === sessionUser.value?.id
  try {
    await api.deleteUser(removing.value.id)
    removing.value = null
    if (self) {
      emit('signedOut')
      return
    }
    await load()
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    deleteBusy.value = false
  }
}

const isSelf = (user: GatewayUser) => user.id === sessionUser.value?.id
const protectedAdmin = (user: GatewayUser) => isLastEnabledAdmin(users.value, user)

onMounted(load)
</script>

<template>
  <section class="panel admin-card">
    <header class="panel-heading">
      <div>
        <h2>{{ $t('users.title') }}</h2>
        <p>{{ $t('users.hint') }}</p>
      </div>
      <span class="panel-count">{{ users.length }} / {{ USER_LIMIT }}</span>
    </header>

    <div
      v-if="loading"
      class="empty-state"
    >
      <span class="loader"></span>
    </div>
    <template v-else>
      <ul class="user-list">
        <li
          v-for="user in users"
          :key="user.id"
        >
          <span
            class="user-symbol"
            aria-hidden="true"
            ><Icon name="account"
          /></span>
          <span class="user-name">
            <strong
              >{{ user.username }}<em v-if="isSelf(user)">{{ $t('users.you') }}</em></strong
            >
            <small>{{
              user.role === 'admin' ? $t('users.roleAdmin') : $t('users.roleViewer')
            }}</small>
          </span>
          <span
            class="inline-status"
            :class="{ warning: !user.enabled }"
          >
            {{ user.enabled ? $t('users.stateEnabled') : $t('users.stateDisabled') }}
          </span>
          <span class="user-actions">
            <button
              class="icon-button"
              type="button"
              :aria-label="$t('users.editUser', { name: user.username })"
              :title="$t('users.edit')"
              @click="editing = user"
            >
              <Icon name="pencil" />
            </button>
            <button
              class="icon-button danger"
              type="button"
              :disabled="protectedAdmin(user)"
              :aria-label="$t('users.deleteUser', { name: user.username })"
              :title="protectedAdmin(user) ? $t('users.lastAdminLocked') : $t('users.delete')"
              @click="removing = user"
            >
              <Icon name="delete" />
            </button>
          </span>
        </li>
      </ul>

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
        {{ $t('users.capacity', { limit: USER_LIMIT }) }}
      </p>
      <button
        v-else
        class="button primary"
        type="button"
        @click="adding = true"
      >
        <Icon name="plus" /> {{ $t('users.add') }}
      </button>
    </template>

    <UserDialog
      v-if="adding"
      :users="users"
      @close="adding = false"
      @saved="afterSave"
    />
    <UserDialog
      v-else-if="editing"
      :users="users"
      :user="editing"
      @close="editing = null"
      @saved="afterSave"
    />

    <Modal
      v-if="removing"
      :title="$t('users.deleteTitle')"
      :busy="deleteBusy ? $t('users.deleting') : undefined"
      @close="removing = null"
    >
      <div class="form-stack">
        <div class="notice error">
          <span>
            <strong>{{ $t('users.deleteConfirm', { name: removing.username }) }}</strong>
            {{ isSelf(removing) ? $t('users.deleteSelfHint') : $t('users.deleteHint') }}
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
            {{ $t('users.delete') }}
          </button>
        </div>
      </div>
    </Modal>
  </section>
</template>

<style scoped>
.user-list {
  display: grid;
  gap: var(--space-1);
  margin: 0;
  padding: 0;
  list-style: none;
}
.user-list li {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--space-2) var(--space-3);
  padding: var(--space-2) 0;
}
.user-list .user-name {
  flex: 1 1 8rem;
}
.user-list .user-actions {
  margin-left: auto;
}
.user-actions .icon-button {
  width: 30px;
  height: 30px;
  font-size: var(--text-md);
}
.user-list li + li {
  box-shadow: 0 -1px 0 var(--line);
}
.user-symbol {
  width: 30px;
  height: 30px;
  display: grid;
  place-items: center;
  border-radius: var(--radius-md);
  color: var(--primary);
  background: var(--primary-soft);
}
.user-name {
  min-width: 0;
  display: grid;
}
.user-name strong {
  display: flex;
  align-items: baseline;
  gap: var(--space-2);
  font-size: var(--text-md);
  font-weight: 500;
}
.user-name em {
  color: var(--muted);
  font-size: var(--text-xs);
  font-style: normal;
}
.user-name small {
  color: var(--muted);
  font-size: var(--text-xs);
}
.user-actions {
  display: flex;
  gap: var(--space-1);
}
.capacity-note {
  color: var(--ink-soft);
  font-size: var(--text-sm);
}
</style>
