<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { api, errorCode } from '../api/client'
import type { BackupPreview, SetupStatus } from '../api/types'
import { encodeBackupFile, validBackupPassword } from '../utils/backup'
import PhysicalConfirmation from './PhysicalConfirmation.vue'

const props = defineProps<{ status: SetupStatus | null }>()
const emit = defineEmits<{ busy: [value: boolean]; refresh: []; complete: [] }>()
const file = ref(''),
  password = ref(''),
  username = ref(''),
  adminPassword = ref(''),
  confirmed = ref(false)
const preview = ref<BackupPreview | null>(null),
  failure = ref(''),
  working = ref(false),
  completed = ref(false)
// Checking a file writes nothing, so only the restore itself waits for the gateway button.
const allowed = computed(
  () => props.status?.physical_window_active && !props.status.recovery_required,
)
const validAdmin = computed(
  () =>
    /^[a-z0-9._-]{1,32}$/.test(username.value) &&
    new TextEncoder().encode(adminPassword.value).length >= 8 &&
    new TextEncoder().encode(adminPassword.value).length <= 128,
)
watch([file, password], () => {
  preview.value = null
  confirmed.value = false
})
function busy(value: boolean) {
  working.value = value
  emit('busy', value)
}
async function selectFile(event: Event) {
  file.value = ''
  failure.value = ''
  preview.value = null
  const selected = (event.target as HTMLInputElement).files?.[0]
  if (!selected) return
  busy(true)
  try {
    file.value = await encodeBackupFile(selected)
  } catch {
    failure.value = 'invalid_backup'
  } finally {
    busy(false)
  }
}
async function inspect() {
  if (working.value || !file.value || !validBackupPassword(password.value)) return
  busy(true)
  failure.value = ''
  preview.value = null
  try {
    preview.value = await api.previewBackup(file.value, password.value)
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    busy(false)
    emit('refresh')
  }
}
async function restore() {
  if (working.value || !allowed.value || !preview.value || !validAdmin.value || !confirmed.value)
    return
  busy(true)
  failure.value = ''
  try {
    await api.restoreBackup(file.value, password.value, username.value, adminPassword.value)
    completed.value = true
    file.value = ''
    password.value = ''
    adminPassword.value = ''
    emit('complete')
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    busy(false)
    if (!completed.value) emit('refresh')
  }
}
const restoredHostname = ref('')
watch(preview, (value) => {
  if (value) restoredHostname.value = value.mdns_enabled ? value.hostname : ''
})
</script>
<template>
  <section class="setup-card">
    <template v-if="completed">
      <h2>{{ $t('backup.restored') }}</h2>
      <p>{{ $t('backup.restoredHint') }}</p>
      <a
        class="button primary"
        :href="restoredHostname ? `http://${restoredHostname}.local/login` : '/login'"
        >{{ $t('login.submit') }}</a
      >
    </template>
    <template v-else>
      <p>{{ $t('backup.restoreIntro') }}</p>
      <form
        class="form-stack"
        @submit.prevent="inspect"
      >
        <label
          ><span>{{ $t('backup.file') }}</span
          ><input
            type="file"
            accept=".oskbackup"
            :disabled="working"
            @change="selectFile"
        /></label>
        <label
          ><span>{{ $t('backup.password') }}</span
          ><input
            v-model="password"
            type="password"
            autocomplete="off"
            :disabled="working"
        /></label>
        <button
          class="button secondary"
          :disabled="working || !file || !validBackupPassword(password)"
        >
          {{ working ? $t('backup.working') : $t('backup.inspect') }}
        </button>
      </form>
      <form
        v-if="preview"
        class="form-stack"
        @submit.prevent="restore"
      >
        <dl class="backup-summary">
          <dt>{{ $t('backup.gateway') }}</dt>
          <dd>{{ preview.gateway_id }}</dd>
          <dt>{{ $t('backup.created') }}</dt>
          <dd>
            {{
              preview.created_at_ms
                ? new Date(preview.created_at_ms).toLocaleString()
                : $t('common.unavailable')
            }}
          </dd>
          <dt>{{ $t('nodes.title') }}</dt>
          <dd>{{ preview.node_count }}</dd>
          <dt>{{ $t('setup.hostname') }}</dt>
          <dd>{{ preview.hostname || $t('backup.defaultHostname') }}</dd>
        </dl>
        <label
          ><span>{{ $t('setup.username') }}</span
          ><input
            v-model.trim="username"
            autocomplete="username"
            maxlength="32"
            :disabled="working"
          /><small>{{ $t('setup.usernameHint') }}</small></label
        >
        <label
          ><span>{{ $t('backup.newAdminPassword') }}</span
          ><input
            v-model="adminPassword"
            type="password"
            autocomplete="new-password"
            :disabled="working"
          /><small>{{ $t('setup.passwordHint') }}</small></label
        >
        <label class="toggle-row"
          ><input
            v-model="confirmed"
            type="checkbox"
            :disabled="working"
          /><span>{{ $t('backup.confirmRestore') }}</span></label
        >
        <PhysicalConfirmation
          id="restore-physical"
          :status="status"
        />
        <button
          class="button primary"
          :disabled="working || !allowed || !validAdmin || !confirmed"
          aria-describedby="restore-physical"
        >
          {{ working ? $t('backup.working') : $t('backup.restore') }}
        </button>
      </form>
      <div
        v-if="failure"
        class="notice error"
        role="alert"
      >
        {{ $t(`error.${failure}`) }}
      </div>
    </template>
  </section>
</template>
