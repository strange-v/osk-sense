<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, reactive, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { useRoute, useRouter } from 'vue-router'
import { api, errorCode } from '../api/client'
import type { GatewayNode, Health } from '../api/types'
import HexInput from '../components/HexInput.vue'
import Icon from '../components/Icon.vue'
import Modal from '../components/Modal.vue'
import NodeDetail from '../components/NodeDetail.vue'
import SignalBars from '../components/SignalBars.vue'
import { PAIRING_KEY_HEX_LENGTH, PAIRING_UID_HEX_LENGTH, parsePairingPaste } from '../utils/hexCredentials'
import { lastSeen, nodeName, signal, volts } from '../utils/format'

const route = useRoute()
const router = useRouter()
const { t, locale } = useI18n()
const nodes = ref<GatewayNode[]>([])
const health = ref<Health | null>(null)
const loading = ref(true)
const registryUnavailable = ref(false)
const failure = ref('')
const showPairing = ref(route.query.pair === '1')
const pairingFailure = ref('')
const pairingActive = ref(false)
const pairingRemaining = ref(0)
const pairingBusy = ref(false)
const credentials = reactive({ uid: '', factoryKey: '' })
// The UID handed to the gateway, and the registry generation just before it:
// together they say whether the window closed because a node joined.
const pairingUid = ref('')
const registryGeneration = ref(0)
let generationAtOpen = 0
const uidInput = ref<InstanceType<typeof HexInput> | null>(null)
const keyInput = ref<InstanceType<typeof HexInput> | null>(null)
const scanInput = ref<HTMLInputElement | null>(null)
// '' while idle; otherwise the i18n suffix of the message under the fields.
type ScanState = '' | 'scanning' | 'read' | 'no_code' | 'unsupported' | 'unreadable'
const scanState = ref<ScanState>('')
// jsQR decodes on the main thread, so the dialog is frozen while it runs:
// mask it rather than leaving a dead-looking dialog under a disabled button.
const scanning = computed(() => scanState.value === 'scanning')

// Which .notice variant each outcome earns. A missed code is retryable with a
// better photo, so it stays on the default warning; a code that is not ours
// and a file that is not an image are both "you scanned the wrong thing" and
// read as errors.
const scanNotice = computed(() => ({
  success: scanState.value === 'read',
  error: scanState.value === 'unsupported' || scanState.value === 'unreadable',
}))
const invalidCredentials = computed(() => credentials.uid.length !== PAIRING_UID_HEX_LENGTH || credentials.factoryKey.length !== PAIRING_KEY_HEX_LENGTH)
const duplicateUid = computed(() => nodes.value.some((node) => node.state !== 'pending' && node.device_uid.toUpperCase() === credentials.uid.toUpperCase()))
let pairingTimer: number | undefined
// Matches the overview, which says "automatically every 10 seconds".
const LIST_REFRESH_MS = 10_000
let listTimer: number | undefined

const selected = ref<GatewayNode | null>(null)

// Reload after a rename or removal, then drop the selection: the record the
// dialog was showing either changed generation or no longer exists.
async function afterNodeChange() {
  selected.value = null
  await load()
}

// Points an open card at the record just fetched, or closes it when the node
// has left the registry -- removed from another session, say.
function followSelected() {
  const nodeId = selected.value?.node_id
  if (nodeId !== undefined) selected.value = nodes.value.find((node) => node.node_id === nodeId) ?? null
}

// A rename or a radio policy change keeps the dialog open on the new record.
async function refreshSelected() {
  await fetchState(true)
  followSelected()
}

type SortKey = 'name' | 'rssi' | 'supply' | 'lastSeen'
const sortKey = ref<SortKey>('name')
const sortAscending = ref(true)

const named = (node: GatewayNode) => nodeName(t, node)

function sortValue(node: GatewayNode): number | undefined {
  if (sortKey.value === 'rssi') return node.rssi
  if (sortKey.value === 'supply') return node.supply_mv
  return node.last_seen_at_ms
}

const sortedNodes = computed(() => {
  const direction = sortAscending.value ? 1 : -1
  return [...nodes.value].sort((left, right) => {
    if (sortKey.value === 'name') return named(left).localeCompare(named(right), locale.value) * direction
    const a = sortValue(left)
    const b = sortValue(right)
    // Nodes that never reported have no value to compare, so they stay at the
    // bottom whichever direction is chosen instead of flipping to the top.
    if (a === undefined) return b === undefined ? 0 : 1
    if (b === undefined) return -1
    return (a - b) * direction
  })
})

function sortBy(key: SortKey) {
  if (sortKey.value === key) sortAscending.value = !sortAscending.value
  // The weakest supply is the one worth seeing first.
  else { sortKey.value = key; sortAscending.value = key === 'name' || key === 'supply' }
}

// `quiet` is the periodic refresh: no spinner, the shorter poll timeout, and a
// failure leaves the last good list on screen instead of replacing it with an
// error -- a single missed poll is not news.
async function fetchState(quiet = false) {
  if (!quiet) loading.value = true
  const source = quiet ? api.poll : api
  const [registryResult, healthResult] = await Promise.allSettled([source.nodes(), source.status()])
  if (registryResult.status === 'fulfilled') { nodes.value = registryResult.value.nodes; registryGeneration.value = registryResult.value.registry_generation }
  else if (!quiet) registryUnavailable.value = true
  if (healthResult.status === 'fulfilled') { health.value = healthResult.value; pairingActive.value = healthResult.value.pairing.active; pairingRemaining.value = healthResult.value.pairing.remaining_seconds }
  else if (!quiet) failure.value = errorCode(healthResult.reason)
  if (!quiet) loading.value = false
}

const load = () => fetchState()

// "Last seen" is a relative time computed during render, so without a refresh
// the whole list freezes at whatever it said when the page opened -- both the
// values and the ages. Polling replaces the array, which recomputes them.
let listPollInFlight = false

async function refreshList() {
  // Step aside while pairing: re-sorting the list under an open pairing window
  // is disruptive, and that dialog reloads on its own when it finishes. An
  // open node card is refreshed with the list, so its readings stay live.
  if (listPollInFlight || showPairing.value) return
  listPollInFlight = true
  try {
    await fetchState(true)
    followSelected()
  } finally { listPollInFlight = false }
}

// Leaves nothing behind: the factory key is a secret, and the picked photo
// keeps a file handle alive in the input until it is cleared.
function resetPairingInputs() {
  credentials.uid = ''
  credentials.factoryKey = ''
  pairingUid.value = ''
  scanState.value = ''
  if (scanInput.value) scanInput.value.value = ''
}

function openPairing() {
  pairingFailure.value = ''
  resetPairingInputs()
  showPairing.value = true
  router.replace({ query: { pair: '1' } })
}

// The scanner (jsQR plus this view's decoding helpers) is a deferred chunk:
// it only reaches the browser once someone taps "Scan QR".
async function onScanPicked(event: Event) {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return
  scanState.value = 'scanning'
  try {
    const { scanPairingQr } = await import('../utils/qrScan')
    const result = await scanPairingQr(file)
    scanState.value = result.status
    // Filling the fields is all a scan does -- the user still confirms with
    // "Add", so a misread code cannot start pairing on its own.
    if (result.status === 'read') {
      credentials.uid = result.credentials.uid
      credentials.factoryKey = result.credentials.key
    }
  } catch {
    scanState.value = 'unreadable'
  } finally {
    // Release the photo, and let the same file be picked again after a miss.
    input.value = ''
  }
}

// A full JSON/text paste (from the printed pairing label or QR export) can
// fill both fields in one action instead of forcing two separate pastes.
// Runs in the capture phase so it claims the event before the focused
// block's own fallback (sequential-fill) paste handler sees it.
function handleCredentialsPaste(event: ClipboardEvent) {
  const parsed = parsePairingPaste(event.clipboardData?.getData('text') ?? '')
  if (!parsed.uid && !parsed.key) return
  event.preventDefault()
  event.stopPropagation()
  scanState.value = ''
  if (parsed.uid) credentials.uid = parsed.uid
  if (parsed.key) credentials.factoryKey = parsed.key
  if (parsed.uid?.length === PAIRING_UID_HEX_LENGTH && !parsed.key) keyInput.value?.focus()
}

async function closePairing() {
  if (pairingActive.value) { try { await api.closePairing() } catch (error) { pairingFailure.value = errorCode(error); return } }
  resetPairingInputs()
  showPairing.value = false
  pairingActive.value = false
  router.replace({ query: {} })
}

async function beginPairing() {
  if (invalidCredentials.value || duplicateUid.value || pairingBusy.value) return
  pairingBusy.value = true
  pairingFailure.value = ''
  try {
    const state = await api.openPairing(credentials.uid, credentials.factoryKey)
    pairingUid.value = credentials.uid
    generationAtOpen = registryGeneration.value
    credentials.factoryKey = ''
    pairingActive.value = state.active
    pairingRemaining.value = state.remaining_seconds
  }
  catch (error) { pairingFailure.value = errorCode(error) }
  finally { pairingBusy.value = false }
}

// The gateway closes the window on success and on expiry alike, and the API
// reports no outcome, so the registry is the only witness: our UID has to be
// present *and* the registry has to have been written since we opened. The
// generation check prevents an unchanged registry from counting as success.
async function finishPairing() {
  const uid = pairingUid.value
  await load()
  const joined = registryGeneration.value !== generationAtOpen
    ? nodes.value.find((node) => node.state === 'active' && node.device_uid.toUpperCase() === uid.toUpperCase())
    : undefined

  resetPairingInputs()
  if (!joined) {
    // Timed out. Both fields are cleared -- the factory key was wiped at POST
    // and cannot be reused -- so the next attempt starts from a fresh scan.
    pairingFailure.value = 'pairing_timeout'
    return
  }
  showPairing.value = false
  router.replace({ query: {} })
  // Straight to the new node: it has no name yet and shows as "Node <id>".
  selected.value = joined
}

let pairingPollInFlight = false

async function refreshPairing() {
  // The countdown ticks every second while a health request may take thirty,
  // so one poll at a time.
  if (!pairingActive.value || pairingPollInFlight) return
  pairingPollInFlight = true
  try {
    const state = await api.poll.status()
    pairingRemaining.value = state.pairing.remaining_seconds
    if (state.pairing.active) return
    pairingActive.value = false
    await finishPairing()
  }
  catch { /* Keep the last visible state. */ }
  finally { pairingPollInFlight = false }
}
onMounted(() => {
  load()
  pairingTimer = window.setInterval(refreshPairing, 1000)
  listTimer = window.setInterval(refreshList, LIST_REFRESH_MS)
})
onBeforeUnmount(() => { window.clearInterval(pairingTimer); window.clearInterval(listTimer) })
</script>

<template>
  <div class="page">
    <div class="page-heading"><div><h1>{{ $t('nodes.title') }}</h1><p>{{ $t('nodes.subtitle') }}</p></div><button class="button primary" type="button" @click="openPairing"><Icon name="plus" /> {{ $t('nodes.add') }}</button></div>

    <div v-if="loading" class="empty-state panel"><span class="loader"></span><p>{{ $t('common.loading') }}</p></div>
    <div v-else-if="failure && !health" class="empty-state panel"><span class="state-icon warning"><Icon name="alert" /></span><h2>{{ $t('error.title') }}</h2><p>{{ $t(`error.${failure}`) }}</p></div>
    <template v-else>
      <div class="summary-strip standalone-summary">
        <div><span>{{ $t('nodes.registered') }}</span><strong>{{ health?.registry.records ?? nodes.length }}</strong></div>
        <div><span>{{ $t('nodes.seen') }}</span><strong>{{ health?.telemetry.nodes_seen ?? '—' }}</strong></div>
        <div><span>{{ $t('nodes.updates') }}</span><strong>{{ health?.telemetry.updates ?? '—' }}</strong></div>
      </div>

      <section v-if="nodes.length" class="panel nodes-panel">
        <header class="panel-heading"><div><h2>{{ $t('nodes.title') }}</h2><p>{{ $t('overview.nodesHint') }}</p></div><span class="panel-count">{{ nodes.length }}</span></header>
        <div class="node-list detailed">
          <div class="node-list-head">
            <span></span>
            <button type="button" :class="{ active: sortKey === 'name' }" @click="sortBy('name')">{{ $t('nodes.columnName') }}<span v-if="sortKey === 'name'" aria-hidden="true">{{ sortAscending ? '↑' : '↓' }}</span></button>
            <span>{{ $t('nodes.columnFirmware') }}</span>
            <button type="button" :class="{ active: sortKey === 'rssi' }" @click="sortBy('rssi')">{{ $t('nodes.columnSignal') }}<span v-if="sortKey === 'rssi'" aria-hidden="true">{{ sortAscending ? '↑' : '↓' }}</span></button>
            <button type="button" :class="{ active: sortKey === 'supply' }" @click="sortBy('supply')">{{ $t('nodes.columnSupply') }}<span v-if="sortKey === 'supply'" aria-hidden="true">{{ sortAscending ? '↑' : '↓' }}</span></button>
            <button type="button" :class="{ active: sortKey === 'lastSeen' }" @click="sortBy('lastSeen')">{{ $t('nodes.columnLastSeen') }}<span v-if="sortKey === 'lastSeen'" aria-hidden="true">{{ sortAscending ? '↑' : '↓' }}</span></button>
            <span>{{ $t('nodes.columnState') }}</span>
          </div>
          <button v-for="node in sortedNodes" :key="node.node_id" class="node-row" type="button" @click="selected = node">
            <span class="node-symbol" aria-hidden="true"><Icon name="access-point" /></span>
            <span class="node-name"><strong>{{ named(node) }}</strong><small>{{ $t('nodes.profile', { id: node.profile_id }) }} · ID {{ node.node_id }}</small></span>
            <span class="node-firmware">{{ node.firmware || '—' }}</span>
            <span class="node-signal">
              <template v-if="node.has_telemetry === false"><small>{{ $t('nodes.noTelemetry') }}</small></template>
              <template v-else><SignalBars :rssi="node.rssi" /><span class="node-rssi">{{ signal(node.rssi) }}</span></template>
            </span>
            <span class="node-supply">{{ volts(node.supply_mv) }}</span>
            <span class="node-seen"><strong>{{ lastSeen(t, node.last_seen_at_ms) }}</strong></span>
            <span class="inline-status" :class="{ warning: node.state !== 'active' }">{{ $t(`nodes.state.${node.state}`) }}</span>
          </button>
        </div>
      </section>
      <section v-else class="panel panel-message large">
        <span class="large-symbol" aria-hidden="true"><Icon name="access-point" /></span>
        <h2>{{ registryUnavailable ? $t('nodes.registryUnavailableTitle') : $t('nodes.emptyTitle') }}</h2>
        <p>{{ registryUnavailable ? $t('nodes.registryUnavailable') : $t('nodes.emptyHint') }}</p>
        <button class="button primary" type="button" @click="openPairing"><Icon name="plus" /> {{ $t('nodes.addFirst') }}</button>
      </section>
    </template>

    <NodeDetail v-if="selected" :node="selected" @close="selected = null" @changed="afterNodeChange" @updated="refreshSelected" />

    <Modal v-if="showPairing" :title="$t('pairing.title')" :busy="scanning ? $t('pairing.scanState.scanning') : undefined" @close="closePairing">
      <div class="form-stack">
        <template v-if="!pairingActive">
          <div class="field-group" @paste.capture="handleCredentialsPaste">
            <div class="field">
              <span>{{ $t('pairing.uid') }}</span>
              <HexInput ref="uidInput" v-model="credentials.uid" :length="PAIRING_UID_HEX_LENGTH" :label="$t('pairing.uid')" @complete="keyInput?.focus()" />
            </div>
            <div class="field">
              <span>{{ $t('pairing.key') }}</span>
              <HexInput ref="keyInput" v-model="credentials.factoryKey" :length="PAIRING_KEY_HEX_LENGTH" :label="$t('pairing.key')" />
            </div>
          </div>
          <div class="pairing-scan">
            <button class="button secondary compact" :disabled="scanning" type="button" @click="scanInput?.click()"><Icon name="qr-code" /> {{ $t('pairing.scan') }}</button>
            <input ref="scanInput" class="visually-hidden" type="file" accept="image/*" capture="environment" :aria-label="$t('pairing.scan')" @change="onScanPicked" />
          </div>
          <div v-if="scanState && !scanning" class="notice scan-status" :class="scanNotice">{{ $t(`pairing.scanState.${scanState}`) }}</div>
          <p class="pairing-instructions">{{ $t('pairing.instructions') }}</p>
        </template>
        <div v-if="pairingActive" class="physical-status open"><span class="pulse" aria-hidden="true"></span><div><strong>{{ $t('pairing.waiting') }}</strong><p>{{ $t('pairing.active', { seconds: pairingRemaining }) }}</p></div></div>
        <div v-if="!pairingActive && duplicateUid" class="notice error">{{ $t('error.node_already_exists') }}</div>
        <div v-else-if="pairingFailure" class="notice error">{{ $t(`error.${pairingFailure}`) }}</div>
        <div class="modal-actions"><button class="button secondary" type="button" @click="closePairing">{{ pairingActive ? $t('pairing.stop') : $t('common.cancel') }}</button><button v-if="!pairingActive" class="button primary" :disabled="pairingBusy || invalidCredentials || duplicateUid" type="button" @click="beginPairing">{{ pairingBusy ? $t('pairing.starting') : $t('pairing.start') }}</button></div>
      </div>
    </Modal>
  </div>
</template>
