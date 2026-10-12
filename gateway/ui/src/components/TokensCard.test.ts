// @vitest-environment jsdom
// The card carries one irreversible thing: a secret the gateway returns once
// and can never produce again. These tests hold that behaviour in place --
// the value is shown in the page rather than in a dismissible dialog, and it
// survives everything except an explicit dismissal.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { ApiToken } from '../api/types'

const state = vi.hoisted(() => ({ tokens: [] as ApiToken[] }))
const gatewayApi = vi.hoisted(() => ({
  tokens: vi.fn(async () => ({ tokens: [...state.tokens] })),
  // Like the gateway: the key joins the list, and the secret is in the 201
  // body only.
  createToken: vi.fn(async (name: string) => {
    const created = {
      id: 9,
      name,
      created_at_ms: 1_700_000_000_000,
      scopes: ['telemetry:read' as const],
    }
    state.tokens = [...state.tokens, { ...created, enabled: true }]
    return { ...created, token: 'secret-value-shown-once' }
  }),
  deleteToken: vi.fn(async () => undefined),
}))
vi.mock('../api/client', () => ({ api: gatewayApi, errorCode: () => 'generic' }))

import TokensCard from './TokensCard.vue'

const token = (over: Partial<ApiToken> & { id: number }): ApiToken => ({
  name: `t${over.id}`,
  enabled: true,
  created_at_ms: 1_700_000_000_000,
  scopes: ['telemetry:read'],
  ...over,
})

const mountCard = async () => {
  const wrapper = mount(TokensCard, {
    global: { plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })] },
  })
  await flushPromises()
  return wrapper
}
const rows = (wrapper: Awaited<ReturnType<typeof mountCard>>) => wrapper.findAll('.token-list li')

// Create through the dialog the way someone would: open it, name the key,
// confirm.
const createThroughDialog = async (
  wrapper: Awaited<ReturnType<typeof mountCard>>,
  name = 'Grafana',
) => {
  await wrapper.get('.admin-card > .button.primary').trigger('click')
  await wrapper.get('.modal-body input[type="text"], .modal-body input:not([type])').setValue(name)
  await wrapper.get('.modal-actions .button.primary').trigger('click')
  await flushPromises()
}

beforeEach(() => {
  state.tokens = [token({ id: 1, name: 'Home Assistant' })]
  vi.clearAllMocks()
})
afterEach(() => vi.restoreAllMocks())

describe('TokensCard', () => {
  it('lists the keys against the gateway capacity', async () => {
    const wrapper = await mountCard()
    expect(rows(wrapper)).toHaveLength(1)
    expect(wrapper.get('.panel-count').text()).toBe('1 / 8')
    expect(rows(wrapper)[0].text()).toContain('Home Assistant')
  })

  it('names the icon-only revoke action for assistive technology', async () => {
    const wrapper = await mountCard()
    expect(rows(wrapper)[0].find('[aria-label="Revoke Home Assistant"]').exists()).toBe(true)
  })

  it('replaces the create button with an explanation at capacity', async () => {
    state.tokens = Array.from({ length: 8 }, (_, index) => token({ id: index + 1 }))
    const wrapper = await mountCard()
    expect(wrapper.get('.panel-count').text()).toBe('8 / 8')
    expect(wrapper.find('.admin-card > .button.primary').exists()).toBe(false)
    expect(wrapper.get('.capacity-note').text()).toBe(en.tokens.capacity.replace('{limit}', '8'))
  })

  it('shows the new secret in the page, not in the dialog that made it', async () => {
    const wrapper = await mountCard()
    await createThroughDialog(wrapper)

    expect(gatewayApi.createToken).toHaveBeenCalledWith('Grafana')
    // The dialog is gone, so no backdrop click can take the secret with it.
    expect(wrapper.find('.modal-backdrop').exists()).toBe(false)
    const revealed = wrapper.get('.revealed-token')
    expect(revealed.get('.generated-token').text()).toBe('secret-value-shown-once')
    expect(revealed.text()).toContain(en.tokens.createdWarning)
    expect(revealed.find('.button.secondary').text()).toBe(en.tokens.copy)
  })

  it('keeps the secret until it is explicitly dismissed', async () => {
    const wrapper = await mountCard()
    await createThroughDialog(wrapper)
    // A reload of the list must not take the one value that cannot be re-read.
    expect(gatewayApi.tokens).toHaveBeenCalledTimes(2)
    expect(wrapper.find('.generated-token').exists()).toBe(true)

    await wrapper.get('.revealed-token .button.primary').trigger('click')
    expect(wrapper.find('.generated-token').exists()).toBe(false)
  })

  async function clickCopy(result: boolean) {
    let copiedValue = ''
    Object.defineProperty(document, 'execCommand', {
      configurable: true,
      value: () => {
        copiedValue = document.querySelector('textarea')?.value ?? ''
        return result
      },
    })
    const wrapper = await mountCard()
    await createThroughDialog(wrapper)
    try {
      await wrapper.get('.revealed-token .button.secondary').trigger('click')
    } finally {
      Reflect.deleteProperty(document, 'execCommand')
    }
    return { wrapper, copiedValue }
  }

  it('copies the secret to the clipboard', async () => {
    const { wrapper, copiedValue } = await clickCopy(true)

    expect(copiedValue).toBe('secret-value-shown-once')
    expect(wrapper.get('.revealed-token .button.secondary').text()).toBe(en.tokens.copied)
    expect(wrapper.find('[role="alert"]').exists()).toBe(false)
  })

  it('asks for a manual copy when the browser refuses', async () => {
    const { wrapper } = await clickCopy(false)

    expect(wrapper.get('.revealed-token .button.secondary').text()).toBe(en.tokens.copy)
    expect(wrapper.get('[role="alert"]').text()).toBe(en.tokens.copyFailed)
  })

  it('reloads the list after revoking a key', async () => {
    const wrapper = await mountCard()
    await rows(wrapper)[0].get('[aria-label="Revoke Home Assistant"]').trigger('click')
    await flushPromises()
    state.tokens = []
    await wrapper.get('.modal-actions .button.danger').trigger('click')
    await flushPromises()

    expect(gatewayApi.deleteToken).toHaveBeenCalledWith(1)
    expect(rows(wrapper)).toHaveLength(0)
    expect(wrapper.get('.capacity-note').text()).toBe(en.tokens.empty)
  })

  it('drops the revealed secret when that very key is revoked', async () => {
    const wrapper = await mountCard()
    await createThroughDialog(wrapper)
    expect(wrapper.find('.generated-token').exists()).toBe(true)

    await rows(wrapper)[1].get('[aria-label="Revoke Grafana"]').trigger('click')
    await flushPromises()
    state.tokens = state.tokens.filter((entry) => entry.id !== 9)
    await wrapper.get('.modal-actions .button.danger').trigger('click')
    await flushPromises()

    expect(gatewayApi.deleteToken).toHaveBeenCalledWith(9)
    expect(wrapper.find('.generated-token').exists()).toBe(false)
  })

  it('keeps a revealed secret when a different key is revoked', async () => {
    const wrapper = await mountCard()
    await createThroughDialog(wrapper)
    await rows(wrapper)[0].get('[aria-label="Revoke Home Assistant"]').trigger('click')
    await flushPromises()
    state.tokens = []
    await wrapper.get('.modal-actions .button.danger').trigger('click')
    await flushPromises()

    expect(wrapper.get('.generated-token').text()).toBe('secret-value-shown-once')
  })

  it('surfaces a gateway refusal instead of pretending it worked', async () => {
    gatewayApi.deleteToken.mockRejectedValueOnce(new Error('mutation_busy'))
    const wrapper = await mountCard()
    await rows(wrapper)[0].get('[aria-label="Revoke Home Assistant"]').trigger('click')
    await flushPromises()
    await wrapper.get('.modal-actions .button.danger').trigger('click')
    await flushPromises()

    expect(wrapper.find('.notice.error').exists()).toBe(true)
    expect(rows(wrapper)).toHaveLength(1)
  })
})
