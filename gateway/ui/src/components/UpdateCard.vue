<script setup lang="ts">
// Checks GitHub for a signed release and installs it. The gateway does the
// work in the background; this card polls it, and after an install waits for
// the restart and sends the user to sign in again, since sessions live in RAM.
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useRouter } from 'vue-router'
import { api, errorCode } from '../api/client'
import type { UpdateStatus } from '../api/types'
import { waitForRestart } from '../utils/restart'

const RELEASES = 'https://github.com/strange-v/RadioSensors/releases/tag/'
const POLL_MS = 1_000

const { t, te } = useI18n()
const router = useRouter()
const status = ref<UpdateStatus | null>(null)
const failure = ref('')
const restarting = ref(false)
let active = true

const wait = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms))
const busy = () => status.value?.state === 'checking' || status.value?.state === 'installing'

function reason(code: string) {
  return te(`updateError.${code}`) ? t(`updateError.${code}`) : t('updateError.other', { code })
}

// Follows a running task until it settles. A failed read while installing
// means the gateway already went down to restart.
async function follow(previousBootId = '') {
  while (active && busy()) {
    await wait(POLL_MS)
    try {
      status.value = await api.poll.update()
    } catch {
      if (status.value?.state !== 'installing') return
      status.value = { ...status.value, state: 'restarting' }
    }
  }
  if (active && status.value?.state === 'restarting') {
    restarting.value = true
    await waitForRestart(previousBootId)
    if (active) await router.replace('/login')
  }
}

async function check() {
  failure.value = ''
  try {
    status.value = await api.checkUpdate()
    await follow()
  } catch (error) {
    failure.value = errorCode(error)
  }
}

async function install() {
  failure.value = ''
  let previousBootId = ''
  try {
    previousBootId = (await api.probe()).boot_id
  } catch {
    /* any answer then counts as back */
  }
  try {
    status.value = await api.installUpdate()
    await follow(previousBootId)
  } catch (error) {
    failure.value = errorCode(error)
  }
}

onMounted(async () => {
  try {
    status.value = await api.poll.update()
    await follow()
  } catch (error) {
    failure.value = errorCode(error)
  }
})
onBeforeUnmount(() => {
  active = false
})
</script>

<template>
  <section class="panel admin-card">
    <header class="panel-heading">
      <div>
        <h2>{{ $t('update.title') }}</h2>
        <p>{{ $t('update.intro') }}</p>
      </div>
    </header>
    <dl class="simple-details">
      <div>
        <dt>{{ $t('update.installed') }}</dt>
        <dd>{{ status?.current_version ?? '—' }}</dd>
      </div>
    </dl>
    <p
      v-if="status?.pending_verify"
      class="update-hint"
    >
      {{ $t('update.onTrial') }}
    </p>

    <div
      v-if="restarting"
      class="panel-message"
    >
      <span class="loader"></span>
      <strong>{{ $t('update.restarting') }}</strong>
      <p>{{ $t('update.restartingHint') }}</p>
    </div>
    <template v-else-if="status?.state === 'installing'">
      <progress
        class="update-progress"
        max="100"
        :value="status.progress"
      ></progress>
      <p role="status">{{ $t('update.installing', { progress: status.progress }) }}</p>
    </template>
    <template v-else>
      <div
        v-if="failure"
        class="notice error"
        role="alert"
      >
        {{ $t(`error.${failure}`) }}
      </div>
      <div
        v-else-if="status?.state === 'failed'"
        class="notice error"
        role="alert"
      >
        {{ $t('update.failed', { reason: reason(status.error) }) }}
      </div>
      <div
        v-else-if="status?.state === 'up_to_date'"
        class="notice success"
        role="status"
      >
        {{ $t('update.upToDate') }}
      </div>
      <template v-if="status?.state === 'available'">
        <div
          class="notice success"
          role="status"
        >
          {{ $t('update.available', { version: status.available_version }) }}
          <a
            :href="RELEASES + status.available_version"
            target="_blank"
            rel="noopener"
            >{{ $t('update.notes') }}</a
          >
        </div>
        <p class="update-hint">{{ $t('update.installHint') }}</p>
        <button
          class="button primary"
          type="button"
          @click="install"
        >
          {{ $t('update.install') }}
        </button>
      </template>
      <button
        v-else
        class="button secondary"
        type="button"
        :disabled="status?.state === 'checking'"
        @click="check"
      >
        {{ status?.state === 'checking' ? $t('update.checking') : $t('update.check') }}
      </button>
    </template>
  </section>
</template>
