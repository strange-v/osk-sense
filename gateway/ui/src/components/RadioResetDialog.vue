<script setup lang="ts">
// Regenerates the radio network. The gateway restarts as part of the call, so
// the dialog owns the whole episode: confirm, fire, then wait for the gateway
// to come back and send the user to sign in again -- sessions live in RAM and
// do not survive the reboot.
import { computed, ref } from 'vue'
import { useRouter } from 'vue-router'
import { api, errorCode } from '../api/client'
import { waitForRestart } from '../utils/restart'
import Modal from './Modal.vue'

const props = defineProps<{ nodeCount: number }>()
const emit = defineEmits<{ close: [] }>()

const router = useRouter()

const networkId = ref('')
const working = ref(false)
const restarting = ref(false)
const failure = ref('')

const invalidId = computed(() => {
  if (!networkId.value) return false
  return (
    !/^\d+$/.test(networkId.value) || Number(networkId.value) < 1 || Number(networkId.value) > 255
  )
})

async function reset() {
  if (invalidId.value || working.value) return
  working.value = true
  failure.value = ''
  let previousBootId = ''
  try {
    previousBootId = (await api.probe()).boot_id
  } catch {
    // Without a boot id the wait falls back to "answers again at all".
  }
  try {
    await api.resetRadioNetwork(networkId.value ? Number(networkId.value) : undefined)
  } catch (error) {
    failure.value = errorCode(error)
    working.value = false
    return
  }
  restarting.value = true
  await waitForRestart(previousBootId)
  await router.replace('/login')
}
</script>

<template>
  <Modal
    :title="$t('radioReset.title')"
    @close="restarting ? undefined : emit('close')"
  >
    <div
      v-if="restarting"
      class="panel-message"
    >
      <span class="loader"></span>
      <strong>{{ $t('radioReset.restarting') }}</strong>
      <p>{{ $t('radioReset.restartingHint') }}</p>
    </div>

    <div
      v-else
      class="form-stack"
    >
      <p class="pairing-instructions">{{ $t('radioReset.intro') }}</p>

      <div class="notice error">
        <span>{{
          nodeCount
            ? $t('radioReset.consequence', { count: nodeCount })
            : $t('radioReset.consequenceEmpty')
        }}</span>
      </div>

      <label>
        <span>{{ $t('radioReset.networkId') }}</span>
        <input
          v-model.trim="networkId"
          inputmode="numeric"
          maxlength="3"
          placeholder="Auto"
        />
        <small :class="{ invalid: invalidId }">{{
          invalidId ? $t('validation.networkId') : $t('radioReset.networkIdHint')
        }}</small>
      </label>

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
          class="button danger"
          :disabled="working || invalidId"
          type="button"
          @click="reset"
        >
          {{ working ? $t('radioReset.working') : $t('radioReset.confirm') }}
        </button>
      </div>
    </div>
  </Modal>
</template>
