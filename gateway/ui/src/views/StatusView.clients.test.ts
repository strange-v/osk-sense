// @vitest-environment jsdom
// The overview's clients card. What it may say depends on who is looking:
// the stream count is public, the list of keys is not, and a viewer must get
// the smaller, still-true version rather than a guess. The card names no
// product, because the gateway cannot tell which client is on the stream.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { createRouter, createWebHistory } from 'vue-router'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { ApiToken } from '../api/types'

const state = vi.hoisted(() => ({
  clients: 0,
  streamClients: [] as { id: number; name: string }[],
  tokens: [] as ApiToken[],
  role: 'admin' as 'admin' | 'viewer',
  tokensFail: false,
  registryReady: true,
  radioCounters: { failed_tags: 0, replay_frames: 0, activation_challenges: 0 },
}))
const health = () => ({
  status: 'ok', firmware: '2.1.0',
  ethernet: { has_ip: true, ip: '192.168.1.10', state: 'connected' },
  radio: { present: true, frequency_hz: 868_000_000, network_id: 137, counters: { ...state.radioCounters } },
  storage: { ready: true }, registry: { ready: state.registryReady, records: 1, generation: 4 },
  telemetry: { nodes_seen: 1, updates: 3 },
  time: { state: 'synchronized', unix_ms: 1_700_000_000_000, last_sync_ms: 1_700_000_000_000 },
  websocket: { clients: state.clients, connections: 2, messages_sent: 512, messages_dropped: 0 },
})

const gatewayApi = vi.hoisted(() => ({
  poll: {
    status: vi.fn(),
    info: vi.fn(async () => ({ firmware_version: '2.1.0', hostname: 'osk-hub', ui: { version: '0.1.0' } })),
    nodes: vi.fn(async () => ({ nodes: [] })),
    clients: vi.fn(async () => ({ clients: [...state.streamClients] })),
  },
  tokens: vi.fn(),
}))
vi.mock('../api/client', () => ({
  api: gatewayApi,
  errorCode: () => 'generic',
  get sessionUser() { return { value: { id: 1, username: 'admin', role: state.role } } },
}))

import StatusView from './StatusView.vue'

const token = (over: Partial<ApiToken> = {}): ApiToken =>
  ({ id: 1, name: 'Home Assistant', enabled: true, created_at_ms: 1_700_000_000_000, scopes: ['telemetry:read'], ...over })

async function mountView() {
  const router = createRouter({ history: createWebHistory(), routes: [{ path: '/:rest(.*)*', component: { template: '<div />' } }] })
  await router.push('/status')
  await router.isReady()
  const wrapper = mount(StatusView, {
    global: { plugins: [router, createI18n({ legacy: false, locale: 'en', messages: { en } })] },
  })
  await flushPromises()
  return wrapper
}
const card = (wrapper: Awaited<ReturnType<typeof mountView>>) => wrapper.get('.clients-card')

beforeEach(() => {
  vi.useFakeTimers()
  state.clients = 0
  state.streamClients = []
  state.tokens = []
  state.role = 'admin'
  state.tokensFail = false
  state.registryReady = true
  state.radioCounters = { failed_tags: 0, replay_frames: 0, activation_challenges: 0 }
  vi.clearAllMocks()
  gatewayApi.poll.status.mockImplementation(async () => health())
  gatewayApi.tokens.mockImplementation(async () => {
    if (state.tokensFail) throw new Error('admin_required')
    return { tokens: [...state.tokens] }
  })
})
afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks() })

describe('StatusView clients card', () => {
  it('marks radio storage unavailable even when configuration storage is ready', async () => {
    state.registryReady = false
    const wrapper = await mountView()
    const storage = wrapper.findAll('.health-list > div').find((row) => row.get('dt').text() === en.status.storage)
    expect(storage?.get('strong').text()).toBe(en.common.attention)
    wrapper.unmount()
  })
  it('shows radio security counts, including zero, without treating past events as a current failure', async () => {
    state.radioCounters = { failed_tags: 7, replay_frames: 0, activation_challenges: 12 }
    const wrapper = await mountView()
    const rows = wrapper.get('.radio-security-card').findAll('dl > div')
    expect(rows.map((row) => row.get('dt').text())).toEqual([en.status.failedTags, en.status.replayFrames, en.status.activationChallenges])
    expect(rows.map((row) => row.get('dd').text())).toEqual(['7', '0', '12'])
    expect(wrapper.find('.attention-banner').exists()).toBe(false)
    wrapper.unmount()
  })

  it('refreshes radio counts and clears them when the gateway restarts', async () => {
    const wrapper = await mountView()
    state.radioCounters = { failed_tags: 2, replay_frames: 5, activation_challenges: 1 }
    await vi.advanceTimersByTimeAsync(10_000)
    await flushPromises()
    const values = () => wrapper.get('.radio-security-card').findAll('dd').map((cell) => cell.text())
    expect(values()).toEqual(['2', '5', '1'])
    state.radioCounters = { failed_tags: 0, replay_frames: 0, activation_challenges: 0 }
    await vi.advanceTimersByTimeAsync(10_000)
    await flushPromises()
    expect(values()).toEqual(['0', '0', '0'])
    wrapper.unmount()
  })

  it('invites setup only when nothing can stream', async () => {
    const wrapper = await mountView()
    expect(card(wrapper).get('.inline-status').text()).toBe(en.clients.state.unconfigured)
    expect(card(wrapper).get('.inline-status').classes()).toContain('warning')
    expect(card(wrapper).get('a').text()).toBe(en.clients.createKey)
  })

  // The instructions do not disappear with the prompt: a key can exist and the
  // setup still have failed, and that is exactly when they are wanted.
  it('drops the loud prompt for a quiet link once a key that can stream exists', async () => {
    state.tokens = [token()]
    const wrapper = await mountView()
    expect(card(wrapper).get('.inline-status').text()).toBe(en.clients.state.idle)
    expect(card(wrapper).get('a').text()).toContain(en.clients.instructions)
    expect(card(wrapper).find('a.button').exists()).toBe(false)
  })

  // The stream also accepts a browser session, so a socket on a gateway with
  // no keys is somebody's browser rather than a client anyone set up.
  it('still reports nothing set up while a session streams', async () => {
    state.clients = 1
    const wrapper = await mountView()
    expect(card(wrapper).get('.inline-status').text()).toBe(en.clients.state.unconfigured)
    expect(card(wrapper).get('a').text()).toBe(en.clients.createKey)
  })

  it('names a client that identified itself at the handshake', async () => {
    state.clients = 1
    state.tokens = [token()]
    state.streamClients = [{ id: 3, name: 'home-assistant/0.3.0' }]
    const wrapper = await mountView()
    expect(card(wrapper).text()).toContain('home-assistant/0.3.0')
    expect(card(wrapper).text()).not.toContain(en.clients.unidentifiedHint)
  })

  // A key's name is free text somebody typed, so a client that sends no
  // identity is counted rather than attributed to whatever key exists.
  it('counts a client that sent no identity instead of guessing one', async () => {
    state.clients = 2
    state.tokens = [token({ name: 'Home Assistant' })]
    state.streamClients = [{ id: 3, name: 'home-assistant/0.3.0' }, { id: 4, name: '' }]
    const wrapper = await mountView()

    const rows = card(wrapper).findAll('.client-list .integration-row')
    expect(rows).toHaveLength(2)
    expect(rows[0].text()).toContain('home-assistant/0.3.0')
    expect(rows[1].text()).toContain(en.clients.unidentifiedHint)
    expect(rows[1].text()).not.toContain('Home Assistant')
  })

  it('falls back to the plain state when no client named itself', async () => {
    state.clients = 1
    state.tokens = [token()]
    const wrapper = await mountView()
    expect(card(wrapper).find('.client-list').exists()).toBe(false)
    expect(card(wrapper).text()).toContain(en.clients.stateTitle.connected)
  })

  it('reports a live stream and its counters', async () => {
    state.clients = 1
    state.tokens = [token()]
    const wrapper = await mountView()
    expect(card(wrapper).get('.inline-status').text()).toBe(en.clients.state.connected)
    expect(card(wrapper).get('.inline-status').classes()).not.toContain('warning')
    const details = card(wrapper).findAll('.simple-details div')
    expect(details[0].text()).toContain(en.clients.streams)
    expect(details[1].text()).toContain('512')
    // Nothing was dropped, so the row that would alarm is not shown.
    expect(card(wrapper).text()).not.toContain(en.clients.messagesDropped)
  })

  it('shows dropped messages when there are any', async () => {
    state.clients = 1
    state.tokens = [token()]
    gatewayApi.poll.status.mockImplementation(async () => {
      const value = health()
      value.websocket.messages_dropped = 7
      return value
    })
    const wrapper = await mountView()
    expect(card(wrapper).text()).toContain(en.clients.messagesDropped)
  })

  it('never asks a viewer for the keys, and offers them nothing to press', async () => {
    state.role = 'viewer'
    state.tokens = [token()]
    const wrapper = await mountView()

    expect(gatewayApi.tokens).not.toHaveBeenCalled()
    expect(card(wrapper).get('.inline-status').text()).toBe(en.clients.state.unknown)
    expect(card(wrapper).find('a').exists()).toBe(false)
  })

  it('falls back to what the stream alone proves when the keys cannot be read', async () => {
    state.tokensFail = true
    const wrapper = await mountView()
    expect(card(wrapper).get('.inline-status').text()).toBe(en.clients.state.unknown)
    // Nothing is claimed about setup, but an admin can still reach the page:
    // the failure was reading the keys, not the right to make one.
    expect(card(wrapper).find('a.button').exists()).toBe(false)
    expect(card(wrapper).get('a').text()).toContain(en.clients.instructions)
  })

  it('reads the keys once, not on every poll', async () => {
    const wrapper = await mountView()
    await vi.advanceTimersByTimeAsync(30_000)
    await flushPromises()

    expect(gatewayApi.poll.status.mock.calls.length).toBeGreaterThan(1)
    expect(gatewayApi.tokens).toHaveBeenCalledTimes(1)
    wrapper.unmount()
  })
})
