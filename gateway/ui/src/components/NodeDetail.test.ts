// @vitest-environment jsdom
// The list refreshes the card while it is open, so the only draft it keeps is
// the name, and only while that is being edited in the header -- a refresh must
// never throw that edit away, and the wanted power level must never be passed
// off as the one the node uses.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import type { Ref } from 'vue'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { GatewayNode } from '../api/types'

const state = vi.hoisted(() => ({ admin: null as unknown as Ref<boolean> }))
const gatewayApi = vi.hoisted(() => ({
  renameNode: vi.fn(async () => ({ node_id: 7, display_name: '', registry_generation: 2 })),
  deleteNode: vi.fn(async () => undefined),
}))
vi.mock('../api/client', async () => {
  const { ref } = await import('vue')
  state.admin = ref(true)
  return { api: gatewayApi, errorCode: () => 'generic', isAdmin: state.admin }
})

import NodeDetail from './NodeDetail.vue'

const node = (over: Partial<GatewayNode> = {}): GatewayNode => ({
  node_id: 7,
  device_uid: '0F1E2D3C4B5A69788796',
  display_name: 'Hall',
  profile_id: 1,
  firmware: '2.1.0',
  state: 'active',
  last_seen_at_ms: Date.now(),
  has_telemetry: true,
  rssi: -70,
  max_power_level: 5,
  power_policy: 'auto',
  tx_power_target: 3,
  tx_power_level: 3,
  radio_fallback: false,
  downlink_rssi: -72,
  supply_mv: 2987,
  ...over,
})

async function mountCard(target = node()) {
  const wrapper = mount(NodeDetail, {
    props: { node: target },
    global: {
      plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })],
      stubs: { NodeRadio: true, NodeCommands: true },
    },
  })
  await flushPromises()
  return wrapper
}

type Card = Awaited<ReturnType<typeof mountCard>>
const heading = (wrapper: Card) => wrapper.get('.modal-header h2').text()
const startEditing = async (wrapper: Card) => {
  await wrapper.get('.title-view .icon-button').trigger('click')
}

beforeEach(() => {
  state.admin.value = true
  vi.clearAllMocks()
})

describe('NodeDetail', () => {
  it('groups the device UID the way the pairing field and the label do', async () => {
    const wrapper = await mountCard()
    expect(wrapper.get('.node-uid').text()).toBe('0F1E-2D3C-4B5A-6978-8796')
  })

  it('shows the link in both directions and the reported level against the ceiling', async () => {
    const wrapper = await mountCard()
    const stats = wrapper.get('.simple-details').text()
    expect(stats).toContain('-70 dBm')
    expect(stats).toContain('-72 dBm')
    expect(stats).toContain('3 of 5')
    expect(wrapper.findAllComponents({ name: 'SignalBars' })).toHaveLength(2)
    expect(wrapper.find('.radio-target').exists()).toBe(false)
  })

  it('shows a wanted level apart from the one the node still uses', async () => {
    const wrapper = await mountCard(node({ tx_power_target: 5 }))
    expect(wrapper.get('.simple-details').text()).toContain('3 of 5')
    expect(wrapper.get('.radio-target').text()).toBe('Requested: 5')
  })

  it('shows every value it does not have yet the same way, as a dash', async () => {
    const wrapper = await mountCard(
      node({
        has_telemetry: false,
        last_seen_at_ms: undefined,
        tx_power_level: undefined,
        rssi: undefined,
        downlink_rssi: undefined,
        supply_mv: undefined,
      }),
    )
    const values = wrapper.findAll('.simple-details dd').map((value) => value.text())
    // Voltage, last seen, both directions of the link, and the transmit level.
    expect(values.filter((value) => value === '—')).toHaveLength(5)
    expect(wrapper.findAllComponents({ name: 'SignalBars' })).toHaveLength(0)
  })

  it('shows the supply voltage in volts', async () => {
    const wrapper = await mountCard()
    expect(wrapper.get('.simple-details').text()).toContain('2.99 V')
  })

  it('says when the node has fallen back', async () => {
    const wrapper = await mountCard(node({ radio_fallback: true }))
    expect(wrapper.get('.radio-fallback').text()).toBe(en.radio.fallback)
  })

  it('renames in the header and stays open on the new name', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    await wrapper.get('.title-edit input').setValue('Porch')
    await wrapper.get('.title-edit').trigger('submit')
    await flushPromises()

    expect(gatewayApi.renameNode).toHaveBeenCalledWith(7, 'Porch')
    expect(wrapper.emitted('updated')).toHaveLength(1)
    expect(wrapper.emitted('close')).toBeUndefined()
    expect(wrapper.find('.title-edit').exists()).toBe(false)
    expect(heading(wrapper)).toBe('Porch')
  })

  it('drops an edit on Escape', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    await wrapper.get('.title-edit input').setValue('Porch')
    await wrapper.get('.title-edit input').trigger('keydown', { key: 'Escape' })

    expect(wrapper.find('.title-edit').exists()).toBe(false)
    expect(heading(wrapper)).toBe('Hall')
    expect(gatewayApi.renameNode).not.toHaveBeenCalled()
  })

  it('closes an unchanged edit without a request', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    await wrapper.get('.title-edit').trigger('submit')
    await flushPromises()
    expect(wrapper.find('.title-edit').exists()).toBe(false)
    expect(gatewayApi.renameNode).not.toHaveBeenCalled()
  })

  it('stops taking text at the byte limit', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    // Cyrillic costs two bytes a letter: 30 letters would be 60 bytes.
    await wrapper.get('.title-edit input').setValue('я'.repeat(30))

    expect((wrapper.get('.title-edit input').element as HTMLInputElement).value).toBe(
      'я'.repeat(24),
    )
    expect(wrapper.get('.title-edit button').attributes('disabled')).toBeUndefined()
  })

  it('drops what is typed mid-name at the limit, not the end of the name', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    const full = 'я'.repeat(24)
    await wrapper.get('.title-edit input').setValue(full)

    const input = wrapper.get('.title-edit input').element as HTMLInputElement
    input.value = `яяab${'я'.repeat(22)}`
    input.setSelectionRange(4, 4)
    await wrapper.get('.title-edit input').trigger('input')

    expect(input.value).toBe(full)
    expect(input.selectionStart).toBe(2)
  })

  it('refuses control characters', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    await wrapper.get('.title-edit input').setValue('a\tb')

    expect(wrapper.get('.title-edit button').attributes('disabled')).toBeDefined()
    await wrapper.get('.title-edit').trigger('submit')
    await flushPromises()
    expect(gatewayApi.renameNode).not.toHaveBeenCalled()
  })

  it('offers the fallback name as the placeholder of an empty field', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    expect(wrapper.get('.title-edit input').attributes('placeholder')).toBe('Node 7')
  })

  it('keeps an edit in progress when the record is refreshed', async () => {
    const wrapper = await mountCard()
    await startEditing(wrapper)
    await wrapper.get('.title-edit input').setValue('Porc')
    await wrapper.setProps({ node: node({ rssi: -80 }) })

    expect((wrapper.get('.title-edit input').element as HTMLInputElement).value).toBe('Porc')
    expect(wrapper.get('.simple-details').text()).toContain('-80 dBm')
  })

  it('removes the node only after confirmation', async () => {
    const wrapper = await mountCard()
    await wrapper.get('.node-footer .button.danger-text').trigger('click')
    expect(gatewayApi.deleteNode).not.toHaveBeenCalled()

    await wrapper.get('.node-footer .button.danger').trigger('click')
    await flushPromises()
    expect(gatewayApi.deleteNode).toHaveBeenCalledWith(7)
    expect(wrapper.emitted('changed')).toHaveLength(1)
    expect(wrapper.emitted('close')).toHaveLength(1)
  })

  it('shows a viewer neither renaming nor removal', async () => {
    state.admin.value = false
    const wrapper = await mountCard()
    expect(wrapper.find('.title-view .icon-button').exists()).toBe(false)
    expect(wrapper.find('.node-footer').exists()).toBe(false)
    expect(wrapper.get('.coming-soon').text()).toBe(en.nodes.adminOnly)
  })
})
