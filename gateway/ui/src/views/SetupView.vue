<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { api, errorCode } from '../api/client'
import type { SetupStatus } from '../api/types'
import Icon from '../components/Icon.vue'
import PhysicalConfirmation from '../components/PhysicalConfirmation.vue'
import RestoreBackup from '../components/RestoreBackup.vue'
import { HOSTNAME_MAX_BYTES, isValidHostname } from '../utils/format'
const { t } = useI18n(),
  status = ref<SetupStatus | null>(null),
  loadError = ref(''),
  submitError = ref(''),
  saving = ref(false),
  completed = ref(false),
  attempted = ref(false)
const form = reactive({ username: '', password: '', hostname: '', networkId: '' })
const restoring = ref(false)
const restoreCompleted = ref(false)
const configuredHostname = ref('')
const continueUrl = computed(() =>
  !configuredHostname.value
    ? '/status'
    : window.location.hostname.endsWith('.local')
      ? `http://${configuredHostname.value}.local/login`
      : '/login',
)
const modes = [
  { restore: false, id: 'setup-tab-new', label: 'backup.newInstallation' },
  { restore: true, id: 'setup-tab-restore', label: 'backup.restore' },
] as const
// Arrow keys move between the tabs; Tab itself goes on into the panel.
function onTabKey(event: KeyboardEvent) {
  if (!['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(event.key) || saving.value) return
  event.preventDefault()
  restoring.value = event.key === 'Home' ? false : event.key === 'End' ? true : !restoring.value
  ;(event.currentTarget as HTMLElement)
    .querySelectorAll<HTMLElement>('[role="tab"]')
    [restoring.value ? 1 : 0]?.focus()
}
let timer: number | undefined
const byteLength = (value: string) => new TextEncoder().encode(value).length
const errors = computed(() => ({
  username: !form.username
    ? t('validation.required')
    : !/^[a-z0-9._-]{1,32}$/.test(form.username)
      ? t('validation.username')
      : '',
  password: !form.password
    ? t('validation.required')
    : byteLength(form.password) < 8 || byteLength(form.password) > 128
      ? t('validation.password')
      : '',
  hostname: isValidHostname(form.hostname) ? '' : t('validation.hostname'),
  networkId:
    form.networkId &&
    (!/^\d+$/.test(form.networkId) || +form.networkId < 1 || +form.networkId > 255)
      ? t('validation.networkId')
      : '',
}))
const invalid = computed(() => Object.values(errors.value).some(Boolean))
async function refresh() {
  if (saving.value || restoreCompleted.value) return
  try {
    status.value = await api.setupStatus()
    loadError.value = ''
  } catch (error) {
    loadError.value = errorCode(error)
  }
}
async function submit() {
  attempted.value = true
  submitError.value = ''
  if (invalid.value || !status.value?.physical_window_active) return
  saving.value = true
  try {
    await api.setup({
      username: form.username,
      password: form.password,
      ...(form.hostname ? { hostname: form.hostname } : {}),
      ...(form.networkId ? { operational_network_id: Number(form.networkId) } : {}),
    })
    configuredHostname.value = form.hostname
    completed.value = true
  } catch (error) {
    submitError.value = errorCode(error)
    await refresh()
  } finally {
    saving.value = false
  }
}
onMounted(() => {
  refresh()
  timer = window.setInterval(refresh, 1000)
})
onBeforeUnmount(() => window.clearInterval(timer))
</script>
<template>
  <div class="page narrow">
    <div
      v-if="completed"
      class="success-panel"
    >
      <span class="state-icon success"><Icon name="check" /></span>
      <h1>{{ $t('setup.successTitle') }}</h1>
      <p>
        {{
          configuredHostname
            ? $t('setup.restarting', { hostname: configuredHostname })
            : $t('setup.success')
        }}
      </p>
      <div class="success-actions">
        <a
          class="button primary"
          :href="continueUrl"
          >{{ $t('setup.continue') }}</a
        >
      </div>
    </div>
    <template v-else
      ><div
        v-if="!status || status.setup_required"
        class="page-title"
      >
        <p class="eyebrow">{{ $t('setup.eyebrow') }}</p>
        <h1>{{ $t('setup.title') }}</h1>
        <p>{{ $t('setup.intro') }}</p>
      </div>
      <div
        v-if="loadError"
        class="notice error"
      >
        <strong>{{ $t('error.title') }}</strong
        ><span>{{ $t(`error.${loadError}`) }}</span>
      </div>
      <section
        v-else-if="status && !status.setup_required"
        class="empty-state panel"
      >
        <span
          class="state-icon info"
          aria-hidden="true"
          ><Icon name="check"
        /></span>
        <h1>{{ $t('setup.alreadyConfigured') }}</h1>
        <p>{{ $t('setup.alreadyConfiguredHint') }}</p>
        <RouterLink
          class="button primary"
          to="/status"
          >{{ $t('setup.continue') }}</RouterLink
        >
      </section>
      <section
        v-else-if="status?.recovery_required"
        class="setup-card"
      >
        <h2>{{ $t('backup.recoveryTitle') }}</h2>
        <p>{{ $t('backup.recoveryHint') }}</p>
        <p>{{ $t('backup.resetHint') }}</p>
        <p>{{ $t('backup.resetConsequence') }}</p>
      </section>
      <template v-else>
        <div
          v-if="!restoreCompleted"
          class="tabs"
          role="tablist"
          :aria-label="$t('backup.setupMode')"
          @keydown="onTabKey"
        >
          <button
            v-for="mode in modes"
            :id="mode.id"
            :key="mode.id"
            role="tab"
            type="button"
            aria-controls="setup-panel"
            :aria-selected="restoring === mode.restore"
            :tabindex="restoring === mode.restore ? 0 : -1"
            :disabled="saving"
            @click="restoring = mode.restore"
          >
            {{ $t(mode.label) }}
          </button>
        </div>
        <RestoreBackup
          v-if="restoring"
          id="setup-panel"
          role="tabpanel"
          aria-labelledby="setup-tab-restore"
          :status="status"
          @busy="saving = $event"
          @refresh="refresh"
          @complete="restoreCompleted = true"
        />
        <form
          v-else
          id="setup-panel"
          class="setup-card"
          role="tabpanel"
          aria-labelledby="setup-tab-new"
          novalidate
          @submit.prevent="submit"
        >
          <label
            ><span>{{ $t('setup.username') }}</span
            ><input
              v-model.trim="form.username"
              autocomplete="username"
              maxlength="32"
              placeholder="admin"
            /><small :class="{ invalid: attempted && errors.username }">{{
              attempted && errors.username ? errors.username : $t('setup.usernameHint')
            }}</small></label
          >
          <label
            ><span>{{ $t('setup.password') }}</span
            ><input
              v-model="form.password"
              type="password"
              autocomplete="new-password"
              maxlength="128"
            /><small :class="{ invalid: attempted && errors.password }">{{
              attempted && errors.password ? errors.password : $t('setup.passwordHint')
            }}</small></label
          >
          <label
            ><span>{{ $t('setup.hostname') }}</span
            ><input
              v-model.trim="form.hostname"
              autocapitalize="none"
              autocomplete="off"
              spellcheck="false"
              :maxlength="HOSTNAME_MAX_BYTES"
              :placeholder="$t('setup.hostnamePlaceholder')"
            /><small
              v-if="attempted && errors.hostname"
              class="invalid"
              >{{ errors.hostname }}</small
            ><small
              v-else
              class="availability-note"
              >{{ $t('setup.hostnameHint') }}</small
            ></label
          >
          <details>
            <summary>{{ $t('setup.advanced') }}</summary>
            <label
              ><span>{{ $t('setup.networkId') }}</span
              ><input
                v-model.trim="form.networkId"
                inputmode="numeric"
                placeholder="Auto"
              /><small :class="{ invalid: attempted && errors.networkId }">{{
                attempted && errors.networkId ? errors.networkId : $t('setup.networkHint')
              }}</small></label
            >
          </details>
          <PhysicalConfirmation
            id="setup-physical"
            :status="status"
          />
          <div
            v-if="submitError"
            class="notice error"
          >
            {{ $t(`error.${submitError}`) }}
          </div>
          <button
            class="button primary full"
            :disabled="saving || !status?.physical_window_active"
            aria-describedby="setup-physical"
            type="submit"
          >
            {{ saving ? $t('setup.saving') : $t('setup.submit') }}
          </button>
        </form>
      </template>
    </template>
  </div>
</template>
