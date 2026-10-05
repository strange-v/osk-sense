<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { api, errorCode, sessionUser } from '../api/client'
import type { ApiToken, GatewayInfo, GatewayNode, Health, StreamClient } from '../api/types'
import Icon from '../components/Icon.vue'
import SignalBars from '../components/SignalBars.vue'
import { dateTime, lastSeen, megahertz, nodeName, signal, timeOfDay } from '../utils/format'
import { clientState, offersSetup } from '../utils/clients'

const { t, locale } = useI18n()
const health = ref<Health | null>(null)
const info = ref<GatewayInfo | null>(null)
const nodes = ref<GatewayNode[]>([])
const nodesAvailable = ref(false)
const failure = ref('')
const refreshing = ref(false)
const updatedAt = ref<Date | null>(null)
const uiVersion = __UI_VERSION__
// Null while unread: only an admin may list keys, and a viewer's card has to
// say less rather than guess. Keys change far too rarely to poll, so this is
// read once.
const tokens = ref<ApiToken[] | null>(null)
// Who is on the stream, as each client named itself at the handshake. Polled
// with the rest, because unlike the keys this changes whenever a client comes
// and goes.
const streamClients = ref<StreamClient[]>([])
let timer: number | undefined

// TimeService reports waiting_for_network | disabled | synchronizing |
// synchronized. Turning NTP off in settings is a deliberate choice, so it does
// not make the gateway unhealthy; the two transient states do, because until
// the clock is set telemetry carries a zero timestamp.
const timeHealthy = computed(() => health.value?.time.state === 'synchronized' || health.value?.time.state === 'disabled')
const storageReady = computed(() => health.value?.storage.ready && health.value?.registry.ready)
const healthy = computed(() => health.value?.status === 'ok' && health.value?.ethernet.has_ip && health.value?.radio.present && storageReady.value && timeHealthy.value)
const nodeProblem = computed(() => nodes.value.find((node) => node.state !== 'active'))
const needsAttention = computed(() => !healthy.value || Boolean(nodeProblem.value))
const seenNodes = computed(() => nodesAvailable.value ? nodes.value.filter((node) => node.has_telemetry === true).length : health.value?.telemetry.nodes_seen ?? 0)

// The widget answers "what needs attention", so it leads with nodes that are
// not active and then with the freshest contacts, rather than showing whichever
// four the registry happened to return first.
const overviewNodes = computed(() => [...nodes.value]
  .sort((left, right) => {
    const problem = Number(right.state !== 'active') - Number(left.state !== 'active')
    return problem !== 0 ? problem : (right.last_seen_at_ms ?? 0) - (left.last_seen_at_ms ?? 0)
  })
  .slice(0, 4))

const fmt = (value: unknown) => value === undefined || value === null || value === '' ? '—' : String(value)

const clients = computed(() => clientState(health.value, tokens.value))
const stream = computed(() => health.value?.websocket)
// The count comes from /health and the names from /ui/clients, and the two
// are read separately, so they can disagree for one tick. The count is what
// the card's state is built on; a row is only ever drawn for a client that
// actually named itself, and the rest are covered by the count.
const namedClients = computed(() => streamClients.value.filter((client) => client.name !== ''))
const unnamedClients = computed(() => Math.max(0, (stream.value?.clients ?? 0) - namedClients.value.length))
// The connection page creates a key, so it is admin-only. A viewer is not sent
// somewhere the router would only bounce it back from.
const canSetUpClients = computed(() => sessionUser.value?.role === 'admin')

async function load() {
  // A request may now take far longer than the polling interval, so skip a
  // tick rather than stacking calls on a gateway that is already struggling.
  if (refreshing.value) return
  refreshing.value = true
  const results = await Promise.allSettled([api.poll.status(), api.poll.info(), api.poll.nodes(), api.poll.clients()])
  if (results[0].status === 'fulfilled') {
    health.value = results[0].value
    failure.value = ''
    updatedAt.value = new Date()
  } else {
    failure.value = errorCode(results[0].reason)
  }
  if (results[1].status === 'fulfilled') info.value = results[1].value
  if (results[2].status === 'fulfilled') {
    nodes.value = results[2].value.nodes
    nodesAvailable.value = true
  }
  // A failure leaves the card with the count and no names, which is the same
  // thing it shows for clients that sent no identity at all.
  streamClients.value = results[3].status === 'fulfilled' ? results[3].value.clients : []
  refreshing.value = false
}

onMounted(() => {
  load()
  if (sessionUser.value?.role === 'admin') {
    // A failure leaves the card in its 'unknown' state, which is accurate: the
    // stream is still readable, the keys are not.
    api.tokens().then((list) => { tokens.value = list.tokens }).catch(() => { tokens.value = null })
  }
  timer = window.setInterval(load, 10000)
})
onBeforeUnmount(() => window.clearInterval(timer))
</script>

<template>
  <div class="page dashboard-page">
    <div class="page-heading">
      <div><h1>{{ $t('overview.title') }}</h1><p>{{ $t('overview.subtitle') }}</p></div>
      <div class="heading-actions">
        <button class="button secondary compact" type="button" @click="load"><Icon name="refresh" /> {{ $t('common.refresh') }}</button>
        <RouterLink class="button primary" to="/nodes?pair=1"><Icon name="plus" /> {{ $t('nodes.add') }}</RouterLink>
      </div>
    </div>

    <div v-if="failure && !health" class="empty-state panel">
      <span class="state-icon warning"><Icon name="alert" /></span><h2>{{ $t('error.title') }}</h2><p>{{ $t(`error.${failure}`) }}</p>
      <button class="button primary" type="button" @click="load">{{ $t('common.retry') }}</button>
    </div>

    <template v-else-if="health">
      <div v-if="needsAttention" class="attention-banner">
        <div><span class="attention-icon" aria-hidden="true"><Icon name="alert" /></span><span><strong>{{ $t('overview.attentionTitle') }}</strong><small>{{ nodeProblem ? $t('overview.nodeAttention', { name: nodeName(t, nodeProblem) }) : $t('overview.gatewayAttention') }}</small></span></div>
        <RouterLink class="text-action" :to="nodeProblem ? '/nodes' : '/settings'">{{ $t('common.view') }} →</RouterLink>
      </div>

      <div class="dashboard-grid">
        <section class="panel nodes-panel">
          <header class="panel-heading"><div><h2>{{ $t('nodes.title') }}</h2><p>{{ $t('overview.nodesHint') }}</p></div><RouterLink class="text-action" to="/nodes">{{ $t('common.viewAll') }} →</RouterLink></header>
          <div class="summary-strip">
            <div><span>{{ $t('nodes.registered') }}</span><strong>{{ health.registry.records }}</strong></div>
            <div><span>{{ $t('nodes.seen') }}</span><strong>{{ seenNodes }}</strong></div>
            <div><span>{{ $t('nodes.updates') }}</span><strong>{{ health.telemetry.updates }}</strong></div>
          </div>

          <div v-if="nodesAvailable && nodes.length" class="node-list compact-list">
            <RouterLink v-for="node in overviewNodes" :key="node.node_id" class="node-row" to="/nodes">
              <span class="node-symbol" aria-hidden="true"><Icon name="access-point" /></span>
              <span class="node-name"><strong>{{ nodeName(t, node) }}</strong><small>{{ $t('nodes.profile', { id: node.profile_id }) }}</small></span>
              <span class="node-seen">
                <strong>{{ lastSeen(t, node.last_seen_at_ms) }}</strong>
                <small v-if="node.has_telemetry === false">{{ $t('nodes.noTelemetry') }}</small>
                <small v-else class="node-seen-signal"><SignalBars :rssi="node.rssi" />{{ signal(node.rssi) }}</small>
              </span>
              <span class="inline-status" :class="{ warning: node.state !== 'active' }">{{ $t(`nodes.state.${node.state}`) }}</span>
            </RouterLink>
          </div>
          <div v-else class="panel-message"><strong>{{ $t('overview.registrySummary', { count: health.registry.records }) }}</strong><p>{{ $t('overview.registryUnavailable') }}</p></div>
        </section>

        <div class="dashboard-side">
          <section class="panel">
            <header class="panel-heading"><div><h2>{{ $t('overview.gateway') }}</h2><p>{{ $t('overview.gatewayHint') }}</p></div><span class="inline-status" :class="{ warning: !healthy }">{{ healthy ? $t('common.working') : $t('common.attention') }}</span></header>
            <dl class="health-list">
              <div><dt><span aria-hidden="true"><Icon name="lan" /></span>{{ $t('status.network') }}</dt><dd><strong>{{ health.ethernet.has_ip ? $t('common.connected') : $t('common.disconnected') }}</strong><small>{{ fmt(health.ethernet.ip) }}<template v-if="info?.hostname"> · {{ info.hostname }}</template></small></dd></div>
              <div><dt><span aria-hidden="true"><Icon name="access-point" /></span>{{ $t('status.radio') }}</dt><dd><strong>{{ health.radio.present ? megahertz(health.radio.frequency_hz) : $t('common.unavailable') }}</strong><small>{{ $t('status.networkId') }} {{ health.radio.network_id }}</small></dd></div>
              <div><dt><span aria-hidden="true"><Icon name="clock-outline" /></span>{{ $t('status.time') }}</dt><dd><strong>{{ $t(`status.timeState.${health.time.state}`) }}</strong><small>{{ dateTime(locale, health.time.last_sync_ms) }}</small></dd></div>
              <div><dt><span aria-hidden="true"><Icon name="database" /></span>{{ $t('status.storage') }}</dt><dd><strong>{{ storageReady ? $t('common.ready') : $t('common.attention') }}</strong><small>{{ $t('status.registryGeneration') }} {{ health.registry.generation }}</small></dd></div>
            </dl>
          </section>

          <section class="panel radio-security-card">
            <header class="panel-heading"><div><h2>{{ $t('status.radioSecurity') }}</h2><p>{{ $t('status.radioSecurityHint') }}</p></div></header>
            <dl class="simple-details">
              <div><dt>{{ $t('status.failedTags') }}</dt><dd>{{ fmt(health.radio.counters?.failed_tags) }}</dd></div>
              <div><dt>{{ $t('status.replayFrames') }}</dt><dd>{{ fmt(health.radio.counters?.replay_frames) }}</dd></div>
              <div><dt>{{ $t('status.activationChallenges') }}</dt><dd>{{ fmt(health.radio.counters?.activation_challenges) }}</dd></div>
            </dl>
          </section>

          <!-- This card names nobody. The gateway cannot tell which client is
               on the stream -- see utils/clients.ts -- so it reports whether
               anything is reading it, and invites a key only when nothing
               could connect at all, and only to someone who can make one. -->
          <section class="panel clients-card">
            <header class="panel-heading">
              <div><h2>{{ $t('clients.title') }}</h2><p>{{ $t('clients.hint') }}</p></div>
              <span class="inline-status" :class="{ warning: clients === 'unconfigured' }">{{ $t(`clients.state.${clients}`) }}</span>
            </header>
            <!-- A row per client that named itself at the handshake. Anything
                 that did not is counted, not guessed at: the keys cannot say
                 who is on the socket. -->
            <div v-if="clients === 'connected' && namedClients.length" class="client-list">
              <div v-for="client in namedClients" :key="client.id" class="integration-row">
                <span class="integration-symbol" aria-hidden="true"><Icon name="lan" /></span>
                <div><strong>{{ client.name }}</strong><p>{{ $t('clients.streaming') }}</p></div>
              </div>
              <div v-if="unnamedClients" class="integration-row">
                <span class="integration-symbol" aria-hidden="true"><Icon name="key" /></span>
                <div><strong>{{ $t('clients.unidentified', { count: unnamedClients }) }}</strong><p>{{ $t('clients.unidentifiedHint') }}</p></div>
              </div>
            </div>
            <div v-else class="integration-row">
              <span class="integration-symbol" aria-hidden="true"><Icon name="key" /></span>
              <div><strong>{{ $t(`clients.stateTitle.${clients}`) }}</strong><p>{{ $t(`clients.stateHint.${clients}`) }}</p></div>
            </div>
            <dl v-if="clients === 'connected' && stream" class="simple-details">
              <div><dt>{{ $t('clients.streams') }}</dt><dd>{{ stream.clients }}</dd></div>
              <div><dt>{{ $t('clients.messagesSent') }}</dt><dd>{{ stream.messages_sent }}</dd></div>
              <div v-if="stream.messages_dropped > 0"><dt>{{ $t('clients.messagesDropped') }}</dt><dd>{{ stream.messages_dropped }}</dd></div>
            </dl>
            <!-- The loud call to action appears only where nothing is set up.
                 The same page stays reachable from the quiet link below, which
                 is the version that has to survive a setup that went wrong. -->
            <RouterLink v-if="offersSetup(clients)" class="button primary full" to="/home-assistant">{{ $t('clients.createKey') }}</RouterLink>
            <RouterLink v-else-if="canSetUpClients" class="text-action" to="/home-assistant">{{ $t('clients.instructions') }} →</RouterLink>
          </section>
          <!-- Read-only build facts. They lived in Settings, where nothing
               about them could be set, and the Save button appeared to own
               them. -->
          <section class="panel">
            <header class="panel-heading"><div><h2>{{ $t('overview.versions') }}</h2><p>{{ $t('overview.versionsHint') }}</p></div></header>
            <dl class="simple-details">
              <div><dt>{{ $t('status.firmware') }}</dt><dd>{{ info?.firmware_version || health.firmware || '—' }}</dd></div>
              <div><dt>Web UI</dt><dd>{{ info?.ui.version || uiVersion }}</dd></div>
            </dl>
          </section>
        </div>
      </div>

      <p v-if="updatedAt" class="updated-note">{{ $t('status.updated', { time: timeOfDay(locale, updatedAt) }) }} · {{ $t('status.autoRefresh') }}</p>
      <details class="diagnostics"><summary>{{ $t('common.details') }}</summary><pre>{{ JSON.stringify({ info, health }, null, 2) }}</pre></details>
    </template>
  </div>
</template>
