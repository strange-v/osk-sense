<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { api, errorCode } from '../api/client'
import type { GatewaySettings } from '../api/types'
import Icon from '../components/Icon.vue'
import { HOSTNAME_MAX_BYTES, isValidHostname } from '../utils/format'
import { vMasonry } from '../utils/masonry'
const settings = ref<GatewaySettings | null>(null)
const failure = ref(''),
  saved = ref(false),
  saving = ref(false)
const form = reactive({
  hostname: '',
  mdnsEnabled: true,
  ntpEnabled: true,
  ntpServers: '',
  pairingSeconds: 120,
  setupSeconds: 600,
})
const servers = computed(() =>
  form.ntpServers
    .split('\n')
    .map((value) => value.trim())
    .filter(Boolean),
)
const invalid = computed(
  () =>
    !isValidHostname(form.hostname) ||
    servers.value.length > 3 ||
    (form.ntpEnabled && servers.value.length === 0) ||
    servers.value.some((value) => value.length > 63 || !/^[A-Za-z0-9.:-]+$/.test(value)) ||
    new Set(servers.value).size !== servers.value.length ||
    form.pairingSeconds < 30 ||
    form.pairingSeconds > 900 ||
    form.setupSeconds < 60 ||
    form.setupSeconds > 1800,
)
function apply(value: GatewaySettings) {
  settings.value = value
  form.hostname = value.hostname
  form.mdnsEnabled = value.mdns_enabled
  form.ntpEnabled = value.ntp_enabled
  form.ntpServers = value.ntp_servers.join('\n')
  form.pairingSeconds = value.pairing_window_seconds
  form.setupSeconds = value.setup_window_seconds
}
async function save() {
  if (invalid.value) return
  saving.value = true
  saved.value = false
  failure.value = ''
  try {
    apply(
      await api.updateSettings({
        hostname: form.hostname,
        mdns_enabled: form.mdnsEnabled,
        ntp_enabled: form.ntpEnabled,
        ntp_servers: servers.value,
        pairing_window_seconds: form.pairingSeconds,
        setup_window_seconds: form.setupSeconds,
      }),
    )
    saved.value = true
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    saving.value = false
  }
}
onMounted(async () => {
  try {
    apply(await api.settings())
  } catch (error) {
    failure.value = errorCode(error)
  }
})
</script>
<template>
  <div class="page">
    <div class="page-heading">
      <div>
        <h1>{{ $t('settings.title') }}</h1>
        <p>{{ $t('settings.subtitle') }}</p>
      </div>
    </div>
    <div
      v-if="failure"
      class="notice error"
    >
      {{ $t(`error.${failure}`) }}
    </div>
    <div
      v-if="saved"
      class="notice success"
    >
      {{ $t('settings.saved') }}
    </div>
    <form
      v-masonry
      class="settings-grid"
      @submit.prevent="save"
    >
      <section class="panel settings-card editable">
        <span
          class="settings-icon"
          aria-hidden="true"
          ><Icon name="tune"
        /></span>
        <div>
          <h2>{{ $t('settings.general') }}</h2>
          <p>{{ $t('settings.generalHint') }}</p>
          <label
            ><span>{{ $t('settings.hostname') }}</span
            ><input
              v-model.trim="form.hostname"
              autocapitalize="none"
              autocomplete="off"
              spellcheck="false"
              :maxlength="HOSTNAME_MAX_BYTES"
            /><small>{{ $t('settings.hostnameHint') }}</small></label
          ><label class="toggle-row"
            ><input
              v-model="form.mdnsEnabled"
              type="checkbox"
            /><span>{{ $t('settings.mdns') }}</span></label
          >
        </div>
      </section>
      <section class="panel settings-card editable">
        <span
          class="settings-icon"
          aria-hidden="true"
          ><Icon name="clock-outline"
        /></span>
        <div>
          <h2>{{ $t('settings.time') }}</h2>
          <p>{{ $t('settings.timeHint') }}</p>
          <label class="toggle-row"
            ><input
              v-model="form.ntpEnabled"
              type="checkbox"
            /><span>{{ $t('settings.ntp') }}</span></label
          ><label
            ><span>{{ $t('settings.ntpServers') }}</span
            ><textarea
              v-model="form.ntpServers"
              rows="3"
              placeholder="pool.ntp.org"
            ></textarea
            ><small>{{ $t('settings.ntpServersHint') }}</small></label
          >
        </div>
      </section>
      <section class="panel settings-card editable">
        <span
          class="settings-icon"
          aria-hidden="true"
          ><Icon name="timer-outline"
        /></span>
        <div>
          <h2>{{ $t('settings.windows') }}</h2>
          <p>{{ $t('settings.windowsHint') }}</p>
          <label
            ><span>{{ $t('settings.pairingSeconds') }}</span
            ><input
              v-model.number="form.pairingSeconds"
              type="number"
              min="30"
              max="900" /></label
          ><label
            ><span>{{ $t('settings.setupSeconds') }}</span
            ><input
              v-model.number="form.setupSeconds"
              type="number"
              min="60"
              max="1800"
          /></label>
        </div>
      </section>
      <div class="settings-actions">
        <small v-if="invalid">{{ $t('settings.invalid') }}</small
        ><button
          class="button primary"
          :disabled="saving || invalid || !settings"
          type="submit"
        >
          {{ saving ? $t('settings.saving') : $t('settings.save') }}
        </button>
      </div>
    </form>
  </div>
</template>
