// @vitest-environment jsdom
// Covers what happens after "Add": the gateway closes the pairing window both
// when a node joins and when the window expires, and the dialog has to tell
// those apart from the registry alone.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { createRouter, createWebHistory } from 'vue-router'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'

const scanPairingQr = vi.hoisted(() => vi.fn())
vi.mock('../utils/qrScan', () => ({ scanPairingQr }))

const UID = '102132435465768798A9'
const KEY = '00112233445566778899AABBCCDDEEFF'

// Stand-in gateway: the tests drive `registry` and `pairing` directly, the way
// the firmware would as a node joins or a window runs out.
const state = vi.hoisted(() => ({
  registry: { registry_generation: 1, nodes: [] as Array<Record<string, unknown>> },
  pairing: { active: false, remaining_seconds: 0 },
}))

const gatewayApi = vi.hoisted(() => ({
  nodes: vi.fn(async () => ({ registry_generation: state.registry.registry_generation, nodes: [...state.registry.nodes] })),
  status: vi.fn(async () => ({
    registry: { records: state.registry.nodes.length },
    telemetry: { nodes_seen: 0, updates: 0 },
    pairing: { ...state.pairing },
  })),
  poll: { status: vi.fn(async () => ({ pairing: { ...state.pairing } })) },
  openPairing: vi.fn(async () => { state.pairing = { active: true, remaining_seconds: 60 }; return { ...state.pairing } }),
  closePairing: vi.fn(async () => { state.pairing = { active: false, remaining_seconds: 0 }; return undefined }),
}))
vi.mock('../api/client', () => ({ api: gatewayApi, errorCode: (error: Error) => error.message, gatewayReachable: { value: true } }))

import NodesView from './NodesView.vue'

const node = (overrides: Record<string, unknown> = {}) => ({
  node_id: 7, device_uid: UID, display_name: '', profile_id: 1, firmware: '2.1.0',
  state: 'active', last_seen_at_ms: 0, rssi: -60, has_telemetry: true, ...overrides,
})

async function mountView() {
  const router = createRouter({ history: createWebHistory(), routes: [{ path: '/nodes', component: NodesView }, { path: '/:rest(.*)*', component: NodesView }] })
  await router.push('/nodes')
  await router.isReady()
  const wrapper = mount(NodesView, {
    global: {
      plugins: [router, createI18n({ legacy: false, locale: 'en', messages: { en } })],
      // The node card is a separate component with its own tests; here it only
      // has to prove it was opened on the right node.
      stubs: { NodeDetail: true },
    },
  })
  await flushPromises()
  return wrapper
}

// Opens the dialog, fills both fields through a scan, and presses "Add".
async function startPairing(wrapper: Awaited<ReturnType<typeof mountView>>) {
  await wrapper.get('.page-heading button').trigger('click')
  await flushPromises()
  scanPairingQr.mockResolvedValue({ status: 'read', credentials: { uid: UID, key: KEY } })
  const input = wrapper.get('input[type="file"]')
  Object.defineProperty(input.element, 'files', { configurable: true, value: [new File([''], 'q.png', { type: 'image/png' })] })
  await input.trigger('change')
  await flushPromises()
  await wrapper.get('.modal-actions .button.primary').trigger('click')
  await flushPromises()
}

// One tick of the 1s poll that watches the window.
async function pollOnce() {
  await vi.advanceTimersByTimeAsync(1000)
  await flushPromises()
}

const fieldValues = (wrapper: Awaited<ReturnType<typeof mountView>>) =>
  wrapper.findAll('.field-group textarea').map((f) => (f.element as HTMLTextAreaElement).value)

beforeEach(() => {
  vi.useFakeTimers()
  state.registry = { registry_generation: 1, nodes: [] }
  state.pairing = { active: false, remaining_seconds: 0 }
  scanPairingQr.mockReset()
  gatewayApi.openPairing.mockClear()
})
afterEach(() => vi.useRealTimers())

describe('pairing window outcome', () => {
  it('hides the credential fields and says what to do while the window is open', async () => {
    const wrapper = await mountView()
    await startPairing(wrapper)

    expect(gatewayApi.openPairing).toHaveBeenCalledWith(UID, KEY)
    expect(wrapper.find('.field-group').exists()).toBe(false)
    expect(wrapper.get('.physical-status').text()).toContain(en.pairing.waiting)
    expect(wrapper.find('node-detail-stub').exists()).toBe(false)
  })

  it('closes the dialog and opens the new node when one joins', async () => {
    const wrapper = await mountView()
    await startPairing(wrapper)

    // The node joined: the gateway wrote the registry and closed the window.
    state.registry = { registry_generation: 2, nodes: [node()] }
    state.pairing = { active: false, remaining_seconds: 0 }
    await pollOnce()

    expect(wrapper.find('.modal-backdrop').exists()).toBe(false)
    const card = wrapper.findComponent({ name: 'NodeDetail' })
    expect(card.exists()).toBe(true)
    expect(card.props('node')).toMatchObject({ device_uid: UID, node_id: 7 })
  })

  it('clears the fields and explains the timeout when nothing joined', async () => {
    const wrapper = await mountView()
    await startPairing(wrapper)

    // The window ran out: nothing was written to the registry.
    state.pairing = { active: false, remaining_seconds: 0 }
    await pollOnce()

    expect(wrapper.find('.modal-backdrop').exists()).toBe(true)
    expect(wrapper.find('node-detail-stub').exists()).toBe(false)
    expect(wrapper.get('.notice.error').text()).toBe(en.error.pairing_timeout)
    // A fresh scan is required: the factory key was wiped when it was posted.
    expect(fieldValues(wrapper)).toEqual(['', ''])
    expect(wrapper.get('.modal-actions .button.primary').attributes('disabled')).toBeDefined()
  })

  it('reports timeout when a transaction is pending even after the registry changes', async () => {
    const wrapper = await mountView()
    await startPairing(wrapper)
    state.registry = { registry_generation: 2, nodes: [node({ state: 'pending' })] }
    state.pairing = { active: false, remaining_seconds: 0 }
    await pollOnce()

    expect(wrapper.get('.notice.error').text()).toBe(en.error.pairing_timeout)
    expect(wrapper.find('node-detail-stub').exists()).toBe(false)
    wrapper.unmount()
  })

  it('opens the node when a resumed pending transaction becomes active', async () => {
    state.registry = { registry_generation: 5, nodes: [node({ state: 'pending', device_uid: UID.toLowerCase() })] }
    const wrapper = await mountView()
    await startPairing(wrapper)
    state.registry = { registry_generation: 6, nodes: [node({ state: 'active', device_uid: UID.toLowerCase() })] }
    state.pairing = { active: false, remaining_seconds: 0 }
    await pollOnce()

    const card = wrapper.findComponent({ name: 'NodeDetail' })
    expect(card.exists()).toBe(true)
    expect(card.props('node')).toMatchObject({ state: 'active', node_id: 7 })
    wrapper.unmount()
  })

  it('resumes an existing pending UID after a scan', async () => {
    state.registry = { registry_generation: 5, nodes: [node({ state: 'pending', device_uid: UID.toLowerCase() })] }
    const wrapper = await mountView()
    await startPairing(wrapper)

    expect(gatewayApi.openPairing).toHaveBeenCalledWith(UID, KEY)
    expect(wrapper.find('.notice.error').exists()).toBe(false)
    expect(wrapper.get('.physical-status').text()).toContain(en.pairing.waiting)
    wrapper.unmount()
  })

  it.each(['active', 'disabled'])('blocks an existing %s UID after a scan, regardless of case', async (nodeState) => {
    state.registry = { registry_generation: 5, nodes: [node({ state: nodeState, device_uid: UID.toLowerCase() })] }
    const wrapper = await mountView()
    await startPairing(wrapper)

    expect(gatewayApi.openPairing).not.toHaveBeenCalled()
    expect(wrapper.get('.modal-actions .button.primary').attributes('disabled')).toBeDefined()
    expect(wrapper.find('node-detail-stub').exists()).toBe(false)
    expect(wrapper.get('.notice.error').text()).toBe(en.error.node_already_exists)

    await wrapper.get('.field-group textarea').setValue('AAAAAAAAAAAAAAAAAAAA')
    expect(wrapper.find('.notice.error').exists()).toBe(false)
    expect(wrapper.get('.modal-actions .button.primary').attributes('disabled')).toBeUndefined()

    await wrapper.get('.field-group textarea').setValue(UID)
    expect(wrapper.get('.notice.error').text()).toBe(en.error.node_already_exists)
    expect(wrapper.get('.modal-actions .button.primary').attributes('disabled')).toBeDefined()
    wrapper.unmount()
  })

  it('shows the API conflict when the local list does not contain the UID yet', async () => {
    gatewayApi.openPairing.mockRejectedValueOnce(new Error('node_already_exists'))
    const wrapper = await mountView()
    await startPairing(wrapper)

    expect(wrapper.get('.notice.error').text()).toBe(en.error.node_already_exists)
    expect(wrapper.find('.physical-status').exists()).toBe(false)
    expect(wrapper.find('.modal-backdrop').exists()).toBe(true)
    wrapper.unmount()
  })
})
