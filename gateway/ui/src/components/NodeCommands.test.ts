// @vitest-environment jsdom
// A command is fetched by a sleeping node on its own schedule, so the card has
// to keep asking until the node answers -- and stop once it has.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import type { Ref } from 'vue'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { CommandRequest, GatewayNode, NodeCommand } from '../api/types'

const state = vi.hoisted(() => ({
  commands: [] as NodeCommand[],
  admin: null as unknown as Ref<boolean>,
}))
const gatewayApi = vi.hoisted(() => ({
  commands: vi.fn(),
  poll: { commands: vi.fn() },
  queueCommand: vi.fn(),
  cancelCommand: vi.fn(),
}))
vi.mock('../api/client', async () => {
  const { ref } = await import('vue')
  state.admin = ref(true)
  return { api: gatewayApi, errorCode: () => 'generic', isAdmin: state.admin }
})

import NodeCommands from './NodeCommands.vue'

const node = (over: Partial<GatewayNode> = {}): GatewayNode => ({
  node_id: 6,
  device_uid: 'C0FFEE00112233445566',
  display_name: 'Gas',
  profile_id: 6,
  firmware: '0.1.0',
  state: 'active',
  has_telemetry: true,
  ...over,
})

const pending = (over: Partial<NodeCommand> = {}): NodeCommand => ({
  node_id: 6,
  command_id: 7,
  type: 'set_count',
  arguments: { count: 1234 },
  queued_at_ms: 1,
  state: 'pending',
  ...over,
})

async function mountCard(target = node()) {
  const wrapper = mount(NodeCommands, {
    props: { node: target },
    global: { plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })] },
  })
  await flushPromises()
  return wrapper
}

type Card = Awaited<ReturnType<typeof mountCard>>
const composeButton = (wrapper: Card) => wrapper.find('.commands-head .button')
const compose = async (wrapper: Card) => {
  await wrapper.get('.commands-head .button').trigger('click')
}
const tick = async (ms = 3_000) => {
  await vi.advanceTimersByTimeAsync(ms)
  await flushPromises()
}

beforeEach(() => {
  vi.useFakeTimers()
  state.commands = []
  state.admin.value = true
  vi.clearAllMocks()
  const list = async () => ({ commands: [...state.commands] })
  gatewayApi.commands.mockImplementation(list)
  gatewayApi.poll.commands.mockImplementation(list)
  gatewayApi.queueCommand.mockImplementation(async (request: CommandRequest) => {
    const created = pending({ type: request.type, arguments: request.arguments })
    state.commands = [created]
    return created
  })
  gatewayApi.cancelCommand.mockImplementation(async () => {
    state.commands = []
  })
})
afterEach(() => vi.useRealTimers())

describe('NodeCommands', () => {
  it('offers the commands of the profile', async () => {
    const counter = await mountCard()
    expect(counter.find('input').exists()).toBe(false)
    await compose(counter)
    expect(counter.findAll('.segmented button').map((button) => button.text())).toEqual([
      en.commands.type.set_count,
      en.commands.type.read_info,
    ])
    expect(counter.get('label span').text()).toBe(en.commands.field.set_count)

    const climate = await mountCard(node({ profile_id: 2 }))
    await compose(climate)
    expect(climate.find('.segmented').exists()).toBe(false)
    expect(climate.find('input').exists()).toBe(false)

    const unknown = await mountCard(node({ profile_id: 99 }))
    expect(unknown.find('.node-commands').exists()).toBe(false)
  })

  it('reads node information without a value and shows what the node reported', async () => {
    const wrapper = await mountCard(node({ profile_id: 2 }))
    await compose(wrapper)
    expect(wrapper.get('.command-hint').text()).toBe(en.commands.hint.read_info)
    await wrapper.get('.button.primary').trigger('click')
    await flushPromises()
    expect(gatewayApi.queueCommand).toHaveBeenCalledWith({
      node_id: 6,
      type: 'read_info',
      arguments: {},
    })

    state.commands = [
      pending({
        type: 'read_info',
        arguments: {},
        state: 'completed',
        status: 'applied',
        completed_at_ms: 2,
        result: { profile_id: 6, firmware: '1.0.2', max_power_level: 20 },
      }),
    ]
    await tick()
    expect(wrapper.get('.command-line strong').text()).toBe(en.commands.describe.read_info)
    expect(wrapper.get('.command-hint').text()).toBe(
      'Firmware 1.0.2, profile 6, maximum radio level 20.',
    )
  })

  it('refuses a count the node cannot store', async () => {
    const wrapper = await mountCard()
    await compose(wrapper)
    await wrapper.get('input').setValue('4294967296')
    expect(wrapper.get('.button.primary').attributes('disabled')).toBeDefined()
    expect(wrapper.find('small.invalid').exists()).toBe(true)

    await wrapper.get('input').setValue('16')
    expect(wrapper.get('.button.primary').attributes('disabled')).toBeUndefined()
  })

  it('queues a command and follows it until the node answers', async () => {
    const wrapper = await mountCard()
    await compose(wrapper)
    await wrapper.get('input').setValue('1234')
    await wrapper.get('.button.primary').trigger('click')
    await flushPromises()

    expect(gatewayApi.queueCommand).toHaveBeenCalledWith({
      node_id: 6,
      type: 'set_count',
      arguments: { count: 1234 },
    })
    expect(wrapper.get('.command-state').text()).toContain(en.commands.state.pending)
    expect(wrapper.get('.command-hint').text()).toBe(en.commands.pendingHint)
    expect(wrapper.find('input').exists()).toBe(false)
    expect(composeButton(wrapper).exists()).toBe(false)

    state.commands = [
      pending({
        state: 'completed',
        status: 'applied',
        completed_at_ms: 2,
        result: { previous_count: 1200, count: 1234 },
      }),
    ]
    await tick()

    expect(wrapper.get('.command-state').text()).toContain(en.commands.status.applied)
    expect(wrapper.get('.command-hint').text()).toBe('The count went from 1200 to 1234.')
    expect(composeButton(wrapper).exists()).toBe(true)

    const polls = gatewayApi.poll.commands.mock.calls.length
    await tick(30_000)
    expect(gatewayApi.poll.commands.mock.calls).toHaveLength(polls)
  })

  it('sends from the keyboard, and not while the value is invalid', async () => {
    const wrapper = await mountCard()
    await compose(wrapper)
    await wrapper.get('input').setValue('-1')
    await wrapper.get('input').trigger('keydown', { key: 'Enter' })
    await flushPromises()
    expect(gatewayApi.queueCommand).not.toHaveBeenCalled()

    await wrapper.get('input').setValue('12')
    await wrapper.get('input').trigger('keydown', { key: 'Enter' })
    await flushPromises()
    expect(gatewayApi.queueCommand).toHaveBeenCalledWith({
      node_id: 6,
      type: 'set_count',
      arguments: { count: 12 },
    })
  })

  it('folds the form away without sending', async () => {
    const wrapper = await mountCard()
    await compose(wrapper)
    await wrapper.get('input').setValue('12')
    await wrapper.get('.compose-actions .button.secondary').trigger('click')

    expect(wrapper.find('input').exists()).toBe(false)
    expect(composeButton(wrapper).exists()).toBe(true)
    expect(gatewayApi.queueCommand).not.toHaveBeenCalled()
  })

  it('cancels a waiting command', async () => {
    state.commands = [pending()]
    const wrapper = await mountCard()
    await wrapper.get('.command-actions .button.secondary').trigger('click')
    await flushPromises()

    expect(gatewayApi.cancelCommand).toHaveBeenCalledWith(6)
    expect(wrapper.find('.command-state').exists()).toBe(false)
    expect(composeButton(wrapper).exists()).toBe(true)
  })

  it('warns that a delivered command may already be applied', async () => {
    state.commands = [pending({ state: 'delivered' })]
    const wrapper = await mountCard()
    expect(wrapper.get('.command-actions small').text()).toBe(en.commands.cancelDeliveredHint)
  })

  it('shows a viewer the state but nothing to change', async () => {
    state.admin.value = false
    state.commands = [pending()]
    const wrapper = await mountCard()
    expect(wrapper.get('.command-state').text()).toContain(en.commands.state.pending)
    expect(composeButton(wrapper).exists()).toBe(false)
    expect(wrapper.find('.command-actions').exists()).toBe(false)

    state.commands = []
    const empty = await mountCard()
    expect(empty.find('.node-commands').exists()).toBe(false)
  })

  it('stops polling once the card is gone', async () => {
    state.commands = [pending()]
    const wrapper = await mountCard()
    await tick()
    const polls = gatewayApi.poll.commands.mock.calls.length
    wrapper.unmount()
    await tick(30_000)
    expect(gatewayApi.poll.commands.mock.calls).toHaveLength(polls)
  })
})
