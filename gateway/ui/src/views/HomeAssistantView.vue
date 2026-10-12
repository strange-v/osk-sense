<script setup lang="ts">
// The shortcut for the one client this gateway is actually built for. It does
// nothing the administration page cannot do -- it creates an API key -- but it
// creates it already named, next to the address and the steps that make the
// key useful, so nobody has to work out what to do with a bare secret.
//
// It stays reachable whether or not a key exists. The overview card decides
// when to point here loudly; this page is the documentation, and documentation
// that disappears once you have made one key is documentation you cannot reach
// on the day the setup did not work.
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, errorCode } from '../api/client'
import type { ApiToken, CreatedApiToken, GatewayInfo } from '../api/types'
import Icon from '../components/Icon.vue'
import { canAddToken, findTokenNamed } from '../utils/tokens'
import { copyText } from '../utils/clipboard'

const TOKEN_NAME = 'Home Assistant'

const info = ref<GatewayInfo | null>(null)
const tokens = ref<ApiToken[] | null>(null)
const created = ref<CreatedApiToken | null>(null)
const busy = ref(false)
const copied = ref(false)
const copyFailed = ref(false)
const failure = ref('')

// The gateway accepts duplicate names, so an existing key is a warning rather
// than a block: it cannot be read back, and nothing here can tell whether it
// still works. Saying so is more useful than either hiding the button or
// pretending the setup is done.
const existing = computed(() =>
  tokens.value === null ? undefined : findTokenNamed(tokens.value, TOKEN_NAME),
)
const full = computed(() => tokens.value !== null && !canAddToken(tokens.value))
const address = computed(() => info.value?.hostname || 'osk-hub.local')

async function create() {
  busy.value = true
  failure.value = ''
  copied.value = false
  copyFailed.value = false
  try {
    created.value = await api.createToken(TOKEN_NAME)
    tokens.value = (await api.tokens()).tokens
  } catch (error) {
    failure.value = errorCode(error)
  } finally {
    busy.value = false
  }
}

function copy() {
  if (!created.value) return
  copied.value = copyText(created.value.token)
  copyFailed.value = !copied.value
}

onMounted(async () => {
  // Both are optional: the steps and the fallback address still read correctly
  // without them, and an admin who cannot list keys can still make one.
  const [gatewayInfo, list] = await Promise.allSettled([api.info(), api.tokens()])
  if (gatewayInfo.status === 'fulfilled') info.value = gatewayInfo.value
  if (list.status === 'fulfilled') tokens.value = list.value.tokens
})
// The secret is never carried off this screen.
onUnmounted(() => {
  created.value = null
})
</script>

<template>
  <div class="page">
    <div class="page-heading">
      <div>
        <p class="eyebrow">Home Assistant</p>
        <h1>{{ $t('ha.title') }}</h1>
        <p>{{ $t('ha.subtitle') }}</p>
      </div>
      <RouterLink
        class="button secondary compact"
        to="/status"
        >{{ $t('ha.back') }}</RouterLink
      >
    </div>

    <ol class="workflow">
      <li class="panel">
        <span class="step-number">1</span>
        <div class="step-body">
          <h2>{{ $t('ha.stepTokenTitle') }}</h2>
          <p>{{ $t('ha.stepTokenHint') }}</p>

          <div
            v-if="failure"
            class="notice error"
          >
            {{ $t(`error.${failure}`) }}
          </div>

          <div
            v-if="created"
            class="revealed-token notice"
          >
            <div>
              <strong>{{ $t('ha.saveToken') }}</strong>
              <code class="generated-token">{{ created.token }}</code>
              <div class="revealed-actions">
                <button
                  class="button secondary"
                  type="button"
                  @click="copy"
                >
                  {{ copied ? $t('ha.copied') : $t('ha.copyToken') }}
                </button>
                <span
                  v-if="copyFailed"
                  class="notice error"
                  role="alert"
                  >{{ $t('ha.copyFailed') }}</span
                >
                <button
                  class="button primary"
                  type="button"
                  @click="created = null"
                >
                  {{ $t('tokens.saved') }}
                </button>
              </div>
            </div>
          </div>

          <template v-else>
            <p
              v-if="existing"
              class="notice"
            >
              {{ $t('ha.alreadyExists') }}
            </p>
            <p
              v-if="full"
              class="step-note"
            >
              {{ $t('ha.capacity') }}
            </p>
            <button
              v-else
              class="button primary"
              :disabled="busy"
              type="button"
              @click="create"
            >
              <Icon name="key" />
              {{
                busy
                  ? $t('ha.creatingToken')
                  : existing
                    ? $t('ha.createAnother')
                    : $t('ha.createToken')
              }}
            </button>
          </template>
        </div>
      </li>

      <li class="panel">
        <span class="step-number">2</span>
        <div class="step-body">
          <h2>{{ $t('ha.stepAddTitle') }}</h2>
          <p>{{ $t('ha.stepAddHint') }}</p>
          <dl class="simple-details">
            <div>
              <dt>{{ $t('ha.address') }}</dt>
              <dd>{{ address }}</dd>
            </div>
            <div>
              <dt>{{ $t('ha.integration') }}</dt>
              <dd>OSK Sense</dd>
            </div>
          </dl>
        </div>
      </li>

      <li class="panel">
        <span class="step-number">3</span>
        <div class="step-body">
          <h2>{{ $t('ha.stepVerifyTitle') }}</h2>
          <p>{{ $t('ha.stepVerifyHint') }}</p>
          <RouterLink
            class="text-action"
            to="/status"
            >{{ $t('ha.openOverview') }} →</RouterLink
          >
        </div>
      </li>
    </ol>
  </div>
</template>

<style scoped>
.workflow {
  display: grid;
  gap: var(--space-3);
  margin: 0;
  padding: 0;
  max-width: 46rem;
  list-style: none;
}
/* `.panel` is only a border and a background; every context sets its own
   padding (see .nodes-panel and .dashboard-side > .panel). */
.workflow > li {
  display: flex;
  align-items: flex-start;
  gap: var(--space-3);
  padding: var(--space-5);
}
.step-number {
  flex: none;
  width: 28px;
  height: 28px;
  display: grid;
  place-items: center;
  border-radius: 50%;
  color: var(--primary);
  background: var(--primary-soft);
  font-size: var(--text-sm);
  font-weight: 600;
}
.step-body {
  flex: 1 1 auto;
  min-width: 0;
  display: grid;
  gap: var(--space-2);
}
.step-body h2 {
  font-size: var(--text-md);
}
.step-body > p {
  color: var(--ink-soft);
  font-size: var(--text-sm);
}
.step-note {
  color: var(--ink-soft);
  font-size: var(--text-sm);
}
.step-body .button {
  justify-self: start;
}
/* The step body is a grid, so a block's own bottom margin would stack on top
   of the row gap. Spacing here is the gap's job alone. */
.step-body > .notice {
  margin-bottom: 0;
}
/* Not a dialog, for the same reason as in TokensCard: the gateway cannot show
   this secret twice, so it must not be dismissable by a stray click. */
.revealed-token {
  margin-bottom: 0;
}
.revealed-token > div {
  display: grid;
  gap: var(--space-2);
}
.revealed-token strong {
  color: var(--warning);
}
.revealed-actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2);
}
</style>
