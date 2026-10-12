<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useRouter } from 'vue-router'
import { api, errorCode } from '../api/client'
import Icon from '../components/Icon.vue'
const router = useRouter(),
  failure = ref('')
async function load() {
  failure.value = ''
  try {
    const status = await api.setupStatus()
    if (status.setup_required) await router.replace('/setup')
    else {
      try {
        await api.session()
        await router.replace('/status')
      } catch {
        await router.replace('/login')
      }
    }
  } catch (error) {
    failure.value = errorCode(error)
  }
}
onMounted(load)
</script>
<template>
  <section class="empty-state panel">
    <template v-if="failure"
      ><span class="state-icon warning"><Icon name="alert" /></span>
      <h1>{{ $t('error.title') }}</h1>
      <p>{{ $t(`error.${failure}`) }}</p>
      <button
        class="button primary"
        type="button"
        @click="load"
      >
        {{ $t('common.retry') }}
      </button></template
    ><template v-else
      ><span class="loader"></span>
      <p>{{ $t('common.loading') }}</p></template
    >
  </section>
</template>
