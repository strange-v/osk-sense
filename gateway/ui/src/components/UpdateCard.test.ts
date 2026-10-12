// @vitest-environment jsdom
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { UpdateStatus } from '../api/types'

const mocked = vi.hoisted(() => ({
  update: vi.fn(),
  checkUpdate: vi.fn(),
  installUpdate: vi.fn(),
  probe: vi.fn(),
  waitForRestart: vi.fn(),
  replace: vi.fn(),
}))
vi.mock('../api/client', () => ({
  api: {
    poll: { update: mocked.update },
    checkUpdate: mocked.checkUpdate,
    installUpdate: mocked.installUpdate,
    probe: mocked.probe,
  },
  errorCode: (e: Error) => e.message,
}))
vi.mock('../utils/restart', () => ({ waitForRestart: mocked.waitForRestart }))
vi.mock('vue-router', () => ({ useRouter: () => ({ replace: mocked.replace }) }))
import UpdateCard from './UpdateCard.vue'

const global = { plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })] }
const status = (over: Partial<UpdateStatus> = {}): UpdateStatus => ({
  state: 'idle',
  current_version: '0.9.2',
  available_version: '',
  progress: 0,
  error: '',
  pending_verify: false,
  ...over,
})

beforeEach(() => {
  vi.resetAllMocks()
  vi.useFakeTimers()
  mocked.update.mockResolvedValue(status())
  mocked.probe.mockResolvedValue({ status: 'ok', boot_id: 'before' })
  mocked.waitForRestart.mockResolvedValue(undefined)
})
afterEach(() => vi.useRealTimers())

async function mounted() {
  const wrapper = mount(UpdateCard, { global })
  await flushPromises()
  return wrapper
}

describe('update card', () => {
  it('checks, follows the check and offers the release with its notes', async () => {
    const w = await mounted()
    expect(w.text()).toContain('0.9.2')
    mocked.checkUpdate.mockResolvedValue(status({ state: 'checking' }))
    mocked.update.mockResolvedValue(status({ state: 'available', available_version: '0.9.3' }))
    await w.get('button').trigger('click')
    await flushPromises()
    expect(w.get('button').text()).toBe('Checking…')
    await vi.advanceTimersByTimeAsync(1_000)
    expect(w.text()).toContain('Version 0.9.3 is available.')
    expect(w.get('a').attributes('href')).toBe(
      'https://github.com/strange-v/RadioSensors/releases/tag/0.9.3',
    )
    expect(w.get('button').text()).toBe('Install and restart')
  })

  it('reports that the installed version is current', async () => {
    mocked.update.mockResolvedValue(status({ state: 'up_to_date' }))
    expect((await mounted()).text()).toContain('This is the latest release.')
  })

  it('shows progress, then waits for the restart and signs in again', async () => {
    mocked.update.mockResolvedValue(status({ state: 'available', available_version: '0.9.3' }))
    const w = await mounted()
    mocked.installUpdate.mockResolvedValue(status({ state: 'installing' }))
    mocked.update.mockResolvedValue(status({ state: 'installing', progress: 40 }))
    await w.get('button').trigger('click')
    await flushPromises()
    await vi.advanceTimersByTimeAsync(1_000)
    expect(w.get('progress').attributes('value')).toBe('40')
    expect(w.text()).toContain('Installing… 40%')

    // The gateway goes down to restart: the next read fails.
    mocked.update.mockRejectedValue(new Error('network'))
    await vi.advanceTimersByTimeAsync(1_000)
    expect(w.text()).toContain('Gateway is restarting')
    expect(mocked.waitForRestart).toHaveBeenCalledWith('before')
    expect(mocked.replace).toHaveBeenCalledWith('/login')
  })

  it('names the failed step, known or not', async () => {
    mocked.update.mockResolvedValue(status({ state: 'failed', error: 'hash_mismatch' }))
    expect((await mounted()).text()).toContain(
      'Update failed: a downloaded image does not match the release.',
    )
    mocked.update.mockResolvedValue(status({ state: 'failed', error: 'something_new' }))
    expect((await mounted()).text()).toContain('Update failed: error something_new.')
  })

  it('shows a refused request and the trial period', async () => {
    mocked.update.mockResolvedValue(
      status({ state: 'available', available_version: '0.9.3', pending_verify: true }),
    )
    const w = await mounted()
    expect(w.text()).toContain('On trial')
    mocked.installUpdate.mockRejectedValue(new Error('update_unavailable'))
    await w.get('button').trigger('click')
    await flushPromises()
    expect(w.text()).toContain('Check for updates first.')
    expect(mocked.replace).not.toHaveBeenCalled()
  })

  it('stops following once it is gone', async () => {
    mocked.update.mockResolvedValue(status({ state: 'checking' }))
    const w = await mounted()
    w.unmount()
    const reads = mocked.update.mock.calls.length
    await vi.advanceTimersByTimeAsync(5_000)
    expect(mocked.update.mock.calls.length).toBeLessThanOrEqual(reads + 1)
  })
})
