// @vitest-environment jsdom
// The node list shows "last seen", a relative time computed during render, so
// a page left open must keep re-fetching or it silently freezes at whatever it
// said when it was opened.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { createRouter, createWebHistory } from 'vue-router'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'

vi.mock('../utils/qrScan', () => ({ scanPairingQr: vi.fn() }))

const state = vi.hoisted(() => ({
  nodes: [] as Array<Record<string, unknown>>,
  generation: 1,
  pairing: { active: false, remaining_seconds: 0 },
}))
const registry = () => ({ registry_generation: state.generation, nodes: [...state.nodes] })
const health = () => ({
  registry: { records: state.nodes.length },
  telemetry: { nodes_seen: 0, updates: 0 },
  pairing: { ...state.pairing },
})

const gatewayApi = vi.hoisted(() => ({
  nodes: vi.fn(),
  status: vi.fn(),
  poll: { nodes: vi.fn(), status: vi.fn() },
  openPairing: vi.fn(),
  closePairing: vi.fn(async () => undefined),
}))
vi.mock('../api/client', () => ({
  api: gatewayApi,
  errorCode: () => 'generic',
  gatewayReachable: { value: true },
}))

import NodesView from './NodesView.vue'

const node = (over: Record<string, unknown> = {}) => ({
  node_id: 2,
  device_uid: 'A1B2C3D4E5F60718293A',
  display_name: 'Кухня',
  profile_id: 1,
  firmware: '2.1.0',
  state: 'active',
  last_seen_at_ms: Date.now(),
  rssi: -68,
  has_telemetry: true,
  ...over,
})

async function mountView() {
  const router = createRouter({
    history: createWebHistory(),
    routes: [
      { path: '/nodes', component: NodesView },
      { path: '/:rest(.*)*', component: NodesView },
    ],
  })
  await router.push('/nodes')
  await router.isReady()
  const wrapper = mount(NodesView, {
    global: {
      plugins: [router, createI18n({ legacy: false, locale: 'en', messages: { en } })],
      stubs: { NodeDetail: true },
    },
  })
  await flushPromises()
  return wrapper
}

const tick = async (ms = 10_000) => {
  await vi.advanceTimersByTimeAsync(ms)
  await flushPromises()
}
const signalText = (wrapper: Awaited<ReturnType<typeof mountView>>) =>
  wrapper.get('.node-row .node-signal').text()

beforeEach(() => {
  vi.useFakeTimers()
  state.nodes = [node()]
  state.generation = 1
  state.pairing = { active: false, remaining_seconds: 0 }
  vi.clearAllMocks()
  gatewayApi.nodes.mockImplementation(async () => registry())
  gatewayApi.status.mockImplementation(async () => health())
  gatewayApi.poll.nodes.mockImplementation(async () => registry())
  gatewayApi.poll.status.mockImplementation(async () => health())
})
afterEach(() => vi.useRealTimers())

describe('node list refresh', () => {
  it('re-fetches the registry on the polling interval', async () => {
    const wrapper = await mountView()
    expect(signalText(wrapper)).toContain('-68 dBm')

    state.nodes = [node({ rssi: -91 })]
    await tick()

    expect(signalText(wrapper)).toContain('-91 dBm')
  })

  it('uses the polling endpoints, not the ones that own the loading state', async () => {
    await mountView()
    expect(gatewayApi.nodes).toHaveBeenCalledTimes(1)
    await tick()
    // The periodic pass goes through api.poll, which carries the shorter
    // timeout; the initial load is the only full-timeout request.
    expect(gatewayApi.nodes).toHaveBeenCalledTimes(1)
    expect(gatewayApi.poll.nodes).toHaveBeenCalledTimes(1)
  })

  it('never shows the loading state again after the first load', async () => {
    const wrapper = await mountView()
    let seenLoader = false
    gatewayApi.poll.nodes.mockImplementation(async () => {
      seenLoader ||= wrapper.find('.loader').exists()
      return registry()
    })
    await tick()
    expect(seenLoader).toBe(false)
    expect(wrapper.find('.node-row').exists()).toBe(true)
  })

  it('keeps the last good list when a single poll fails', async () => {
    const wrapper = await mountView()
    gatewayApi.poll.nodes.mockRejectedValueOnce(new Error('timeout'))
    gatewayApi.poll.status.mockRejectedValueOnce(new Error('timeout'))
    await tick()

    expect(wrapper.find('.node-row').exists()).toBe(true)
    expect(wrapper.find('.empty-state').exists()).toBe(false)
    expect(wrapper.find('.notice.error').exists()).toBe(false)
  })

  it('keeps an open node card on the fresh record', async () => {
    const wrapper = await mountView()
    await wrapper.get('.node-row').trigger('click')

    state.nodes = [node({ rssi: -91 })]
    await tick()

    expect(wrapper.getComponent({ name: 'NodeDetail' }).props('node')).toMatchObject({
      node_id: 2,
      rssi: -91,
    })
  })

  it('closes the node card when the node leaves the registry', async () => {
    const wrapper = await mountView()
    await wrapper.get('.node-row').trigger('click')
    expect(wrapper.findComponent({ name: 'NodeDetail' }).exists()).toBe(true)

    state.nodes = []
    await tick()

    expect(wrapper.findComponent({ name: 'NodeDetail' }).exists()).toBe(false)
  })

  it('holds off while pairing, and resumes once the dialog closes', async () => {
    const wrapper = await mountView()
    await wrapper.get('.page-heading button').trigger('click') // open pairing
    await flushPromises()

    await tick()
    expect(gatewayApi.poll.nodes).not.toHaveBeenCalled()

    await wrapper.get('.modal-actions .button.secondary').trigger('click')
    await flushPromises()
    await tick()
    expect(gatewayApi.poll.nodes).toHaveBeenCalled()
  })

  it('stops polling once the view is gone', async () => {
    const wrapper = await mountView()
    await tick()
    const calls = gatewayApi.poll.nodes.mock.calls.length
    wrapper.unmount()
    await tick(60_000)
    expect(gatewayApi.poll.nodes.mock.calls).toHaveLength(calls)
  })
})
