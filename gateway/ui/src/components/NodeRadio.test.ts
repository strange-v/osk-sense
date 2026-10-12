// @vitest-environment jsdom
// Power is requested, not set: a choice goes to the gateway the moment it is
// made, can never exceed the node's ceiling, and stays shown until the
// refreshed record carries it.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import type { Ref } from 'vue'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { GatewayNode } from '../api/types'

const state = vi.hoisted(() => ({ admin: null as unknown as Ref<boolean> }))
const gatewayApi = vi.hoisted(() => ({
  setPowerPolicy: vi.fn(async () => ({ node_id: 7, registry_generation: 2 })),
}))
vi.mock('../api/client', async () => {
  const { ref } = await import('vue')
  state.admin = ref(true)
  return { api: gatewayApi, errorCode: () => 'generic', isAdmin: state.admin }
})

import NodeRadio from './NodeRadio.vue'

const node = (over: Partial<GatewayNode> = {}): GatewayNode => ({
  node_id: 7,
  device_uid: '102132435465768798A9',
  display_name: 'Hall',
  profile_id: 5,
  firmware: '0.1.0',
  state: 'active',
  has_telemetry: true,
  rssi: -70,
  max_power_level: 2,
  power_policy: 'auto',
  tx_power_target: 2,
  tx_power_level: 2,
  radio_fallback: false,
  downlink_rssi: -72,
  ...over,
})

const mountCard = async (target = node()) => {
  const wrapper = mount(NodeRadio, {
    props: { node: target },
    global: { plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })] },
  })
  await flushPromises()
  return wrapper
}

type Card = Awaited<ReturnType<typeof mountCard>>
const choices = (wrapper: Card) => wrapper.findAll('.segmented button')
const chosen = (wrapper: Card) => wrapper.get('.segmented button.active').text()

beforeEach(() => {
  state.admin.value = true
  vi.clearAllMocks()
})

describe('NodeRadio', () => {
  it('offers automatic control and every level up to the ceiling side by side', async () => {
    const wrapper = await mountCard()
    expect(choices(wrapper).map((button) => button.text())).toEqual([en.radio.auto, '0', '1', '2'])
    expect(chosen(wrapper)).toBe(en.radio.auto)
    expect(wrapper.find('select').exists()).toBe(false)
  })

  it('applies a fixed level as soon as it is chosen, and keeps showing it until the record catches up', async () => {
    const wrapper = await mountCard()
    await choices(wrapper)[2].trigger('click')
    await flushPromises()

    expect(gatewayApi.setPowerPolicy).toHaveBeenCalledWith(7, {
      power_policy: 'fixed',
      fixed_power_level: 1,
    })
    expect(wrapper.emitted('updated')).toHaveLength(1)
    expect(chosen(wrapper)).toBe('1')
    expect(wrapper.get('.radio-hint').text()).toContain('2 is the maximum')

    await wrapper.setProps({ node: node({ power_policy: 'fixed', fixed_power_level: 1 }) })
    expect(chosen(wrapper)).toBe('1')
  })

  it('returns a fixed node to automatic control', async () => {
    const wrapper = await mountCard(node({ power_policy: 'fixed', fixed_power_level: 2 }))
    expect(chosen(wrapper)).toBe('2')
    await choices(wrapper)[0].trigger('click')
    await flushPromises()
    expect(gatewayApi.setPowerPolicy).toHaveBeenCalledWith(7, { power_policy: 'auto' })
  })

  it('does not resend the choice already in force', async () => {
    const wrapper = await mountCard()
    await choices(wrapper)[0].trigger('click')
    await flushPromises()
    expect(gatewayApi.setPowerPolicy).not.toHaveBeenCalled()
  })

  it('puts the choice back when the gateway refuses it', async () => {
    gatewayApi.setPowerPolicy.mockRejectedValueOnce(new Error('refused'))
    const wrapper = await mountCard()
    await choices(wrapper)[3].trigger('click')
    await flushPromises()

    expect(chosen(wrapper)).toBe(en.radio.auto)
    expect(wrapper.get('.notice.error').text()).toBe(en.error.generic)
    expect(wrapper.emitted('updated')).toBeUndefined()
  })

  it('lists the levels when there are too many to sit side by side', async () => {
    const wrapper = await mountCard(
      node({ max_power_level: 31, power_policy: 'fixed', fixed_power_level: 12 }),
    )
    expect(wrapper.find('.segmented').exists()).toBe(false)
    expect(wrapper.findAll('select option')).toHaveLength(33)
    expect((wrapper.get('select').element as HTMLSelectElement).value).toBe('12')

    await wrapper.get('select').setValue('20')
    await flushPromises()
    expect(gatewayApi.setPowerPolicy).toHaveBeenCalledWith(7, {
      power_policy: 'fixed',
      fixed_power_level: 20,
    })
  })

  it('shows a viewer the policy without controls', async () => {
    state.admin.value = false
    const wrapper = await mountCard(node({ power_policy: 'fixed', fixed_power_level: 2 }))
    expect(wrapper.find('.segmented').exists()).toBe(false)
    expect(wrapper.find('select').exists()).toBe(false)
    expect(wrapper.get('.power-value').text()).toBe('Fixed at level 2')
  })
})
