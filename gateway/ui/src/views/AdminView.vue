<script setup lang="ts">
// Everything that acts on the gateway the moment you press it, kept apart
// from Settings, whose single Save button owns its whole form.
import { onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { api, errorCode, forgetSession } from '../api/client'
import type { Health } from '../api/types'
import BackupCard from '../components/BackupCard.vue'
import Icon from '../components/Icon.vue'
import RadioResetDialog from '../components/RadioResetDialog.vue'
import TokensCard from '../components/TokensCard.vue'
import UpdateCard from '../components/UpdateCard.vue'
import UsersCard from '../components/UsersCard.vue'
import { vMasonry } from '../utils/masonry'

const router = useRouter()
const health = ref<Health | null>(null)
const failure = ref('')
const showRadioReset = ref(false)

// The gateway revoked this session while answering the change that caused it,
// so there is nothing to log out of -- drop the local state and start over.
async function signedOut() {
  forgetSession()
  await router.replace({ path: '/login', query: { reason: 'session_revoked' } })
}

onMounted(async () => {
  try {
    health.value = await api.status()
  } catch (error) {
    failure.value = errorCode(error)
  }
})
</script>

<template>
  <div class="page">
    <div class="page-heading">
      <div>
        <h1>{{ $t('admin.title') }}</h1>
        <p>{{ $t('admin.subtitle') }}</p>
      </div>
    </div>

    <div
      v-masonry
      class="admin-grid"
    >
      <UsersCard @signed-out="signedOut" />

      <TokensCard />

      <UpdateCard />

      <BackupCard />

      <section class="panel admin-card">
        <header class="panel-heading">
          <div>
            <h2>{{ $t('radioReset.card') }}</h2>
            <p>{{ $t('radioReset.cardHint') }}</p>
          </div>
        </header>
        <dl class="simple-details">
          <div>
            <dt>{{ $t('status.networkId') }}</dt>
            <dd>{{ health?.radio.network_id ?? '—' }}</dd>
          </div>
          <div>
            <dt>{{ $t('nodes.registered') }}</dt>
            <dd>{{ health?.registry.records ?? '—' }}</dd>
          </div>
        </dl>
        <div
          v-if="failure"
          class="notice error"
        >
          {{ $t(`error.${failure}`) }}
        </div>
        <button
          class="button danger-text"
          type="button"
          @click="showRadioReset = true"
        >
          {{ $t('radioReset.action') }}
        </button>
      </section>
    </div>

    <RadioResetDialog
      v-if="showRadioReset"
      :node-count="health?.registry.records ?? 0"
      @close="showRadioReset = false"
    />
  </div>
</template>
