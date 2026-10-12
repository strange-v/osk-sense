<script setup lang="ts">
import { computed, ref } from 'vue'
import { api, errorCode } from '../api/client'
import { downloadBackup, validBackupPassword } from '../utils/backup'

const password = ref(''),
  confirmation = ref(''),
  failure = ref(''),
  working = ref(false),
  saved = ref(false)
const valid = computed(
  () => validBackupPassword(password.value) && password.value === confirmation.value,
)
async function save() {
  if (!valid.value || working.value) return
  working.value = true
  failure.value = ''
  saved.value = false
  try {
    downloadBackup(await api.exportBackup(password.value))
    saved.value = true
    password.value = ''
    confirmation.value = ''
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    working.value = false
  }
}
</script>
<template>
  <section class="panel admin-card">
    <header class="panel-heading">
      <div>
        <h2>{{ $t('backup.title') }}</h2>
        <p>{{ $t('backup.intro') }}</p>
      </div>
    </header>
    <form
      class="form-stack"
      @submit.prevent="save"
    >
      <label
        ><span>{{ $t('backup.password') }}</span
        ><input
          v-model="password"
          type="password"
          autocomplete="new-password"
          :disabled="working"
        /><small>{{ $t('backup.passwordHint') }}</small></label
      >
      <label
        ><span>{{ $t('backup.confirmPassword') }}</span
        ><input
          v-model="confirmation"
          type="password"
          autocomplete="new-password"
          :disabled="working"
      /></label>
      <div
        v-if="failure"
        class="notice error"
        role="alert"
      >
        {{ $t(`error.${failure}`) }}
      </div>
      <div
        v-if="saved"
        class="notice success"
        role="status"
      >
        {{ $t('backup.downloaded') }}
      </div>
      <button
        class="button primary"
        :disabled="working || !valid"
      >
        {{ working ? $t('backup.working') : $t('backup.download') }}
      </button>
    </form>
    <details>
      <summary>{{ $t('backup.resetTitle') }}</summary>
      <p>{{ $t('backup.resetHint') }}</p>
      <p>{{ $t('backup.resetConsequence') }}</p>
    </details>
  </section>
</template>
