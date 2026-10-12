// @vitest-environment jsdom
// The card's job is to keep the gateway's rules visible: a capacity of four,
// a last enabled admin that cannot be removed, and mutations that take effect
// without a Save button -- including on your own account, which ends the
// session that made the change.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { GatewayUser } from '../api/types'

const state = vi.hoisted(() => ({ users: [] as GatewayUser[], self: 1 }))
const gatewayApi = vi.hoisted(() => ({
  users: vi.fn(async () => ({ users: [...state.users] })),
  createUser: vi.fn(async () => ({
    id: 9,
    username: 'new',
    role: 'viewer' as const,
    enabled: true,
  })),
  updateUser: vi.fn(async () => state.users[0]),
  deleteUser: vi.fn(async () => undefined),
}))
vi.mock('../api/client', () => ({
  api: gatewayApi,
  errorCode: () => 'generic',
  sessionUser: { value: { id: state.self, username: 'admin', role: 'admin' } },
}))

import UsersCard from './UsersCard.vue'

const user = (over: Partial<GatewayUser> & { id: number }): GatewayUser => ({
  username: `u${over.id}`,
  role: 'viewer',
  enabled: true,
  ...over,
})
const admin = user({ id: 1, username: 'admin', role: 'admin' })
const viewer = user({ id: 2, username: 'olena' })

const mountCard = async () => {
  const wrapper = mount(UsersCard, {
    global: { plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })] },
  })
  await flushPromises()
  return wrapper
}
const rows = (wrapper: Awaited<ReturnType<typeof mountCard>>) => wrapper.findAll('.user-list li')
const rowFor = (wrapper: Awaited<ReturnType<typeof mountCard>>, name: string) =>
  rows(wrapper).find((row) => row.text().includes(name))!
// The row actions are icon-only, so they are addressed the way a screen
// reader would: by accessible name, not by visible text.
const deleteButton = (wrapper: Awaited<ReturnType<typeof mountCard>>, name: string) =>
  rowFor(wrapper, name).get(`[aria-label="${en.users.deleteUser.replace('{name}', name)}"]`)

beforeEach(() => {
  state.users = [admin, viewer]
  state.self = 1
  vi.clearAllMocks()
})
afterEach(() => vi.restoreAllMocks())

describe('UsersCard', () => {
  it('names the icon-only row actions for assistive technology', async () => {
    const wrapper = await mountCard()
    const row = rowFor(wrapper, 'olena')
    expect(row.find('[aria-label="Edit olena"]').exists()).toBe(true)
    expect(row.find('[aria-label="Delete olena"]').exists()).toBe(true)
    expect(row.findAll('.icon-button')).toHaveLength(2)
    // Hover text repeats the action; the accessible name carries the subject.
    expect(row.get('[aria-label="Edit olena"]').attributes('title')).toBe(en.users.edit)
  })

  it('lists the accounts and marks the signed-in one', async () => {
    const wrapper = await mountCard()
    expect(rows(wrapper)).toHaveLength(2)
    expect(rowFor(wrapper, 'admin').text()).toContain(en.users.you)
    expect(rowFor(wrapper, 'olena').text()).not.toContain(en.users.you)
    expect(wrapper.get('.panel-count').text()).toBe('2 / 4')
  })

  it('blocks deleting the last enabled admin', async () => {
    const wrapper = await mountCard()
    expect(deleteButton(wrapper, 'admin').attributes('disabled')).toBeDefined()
    expect(deleteButton(wrapper, 'olena').attributes('disabled')).toBeUndefined()
  })

  it('releases that block once a second admin can sign in', async () => {
    state.users = [admin, user({ id: 2, username: 'olena', role: 'admin' })]
    const wrapper = await mountCard()
    expect(deleteButton(wrapper, 'admin').attributes('disabled')).toBeUndefined()
  })

  it('keeps counting only enabled admins', async () => {
    state.users = [admin, user({ id: 2, username: 'olena', role: 'admin', enabled: false })]
    const wrapper = await mountCard()
    expect(deleteButton(wrapper, 'admin').attributes('disabled')).toBeDefined()
  })

  it('replaces the add button with an explanation at capacity', async () => {
    state.users = [1, 2, 3, 4].map((id) => user({ id, role: id === 1 ? 'admin' : 'viewer' }))
    const wrapper = await mountCard()
    expect(wrapper.get('.panel-count').text()).toBe('4 / 4')
    expect(wrapper.find('.admin-card > .button.primary').exists()).toBe(false)
    expect(wrapper.get('.capacity-note').text()).toBe(en.users.capacity.replace('{limit}', '4'))
  })

  it('reloads the list after deleting someone else', async () => {
    const wrapper = await mountCard()
    await deleteButton(wrapper, 'olena').trigger('click')
    await flushPromises()
    state.users = [admin]
    await wrapper.get('.modal-actions .button.danger').trigger('click')
    await flushPromises()

    expect(gatewayApi.deleteUser).toHaveBeenCalledWith(2)
    expect(rows(wrapper)).toHaveLength(1)
    expect(wrapper.emitted('signedOut')).toBeUndefined()
  })

  it('signs out instead of reloading when you delete your own account', async () => {
    // A second admin, so the row is not locked by the last-admin rule.
    state.users = [admin, user({ id: 2, username: 'olena', role: 'admin' })]
    const wrapper = await mountCard()
    await deleteButton(wrapper, 'admin').trigger('click')
    await flushPromises()
    expect(wrapper.get('.modal-body .notice').text()).toContain(en.users.deleteSelfHint)

    await wrapper.get('.modal-actions .button.danger').trigger('click')
    await flushPromises()

    expect(wrapper.emitted('signedOut')).toHaveLength(1)
    // No point reloading a list this session can no longer read.
    expect(gatewayApi.users).toHaveBeenCalledTimes(1)
  })

  it('surfaces a gateway refusal instead of pretending it worked', async () => {
    gatewayApi.deleteUser.mockRejectedValueOnce(new Error('last_admin_required'))
    const wrapper = await mountCard()
    await deleteButton(wrapper, 'olena').trigger('click')
    await flushPromises()
    await wrapper.get('.modal-actions .button.danger').trigger('click')
    await flushPromises()
    expect(wrapper.find('.notice.error').exists()).toBe(true)
  })
})
