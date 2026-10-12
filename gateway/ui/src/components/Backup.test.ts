// @vitest-environment jsdom
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'

const mocked = vi.hoisted(() => ({
  exportBackup: vi.fn(),
  previewBackup: vi.fn(),
  restoreBackup: vi.fn(),
  download: vi.fn(),
  encode: vi.fn(),
  setupStatus: vi.fn(),
}))
vi.mock('../api/client', () => ({ api: mocked, errorCode: (e: Error) => e.message }))
vi.mock('../utils/backup', async (original) => ({
  ...(await original<typeof import('../utils/backup')>()),
  downloadBackup: mocked.download,
  encodeBackupFile: mocked.encode,
}))
import BackupCard from './BackupCard.vue'
import RestoreBackup from './RestoreBackup.vue'
import SetupView from '../views/SetupView.vue'

const global = {
  plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })],
  stubs: { RouterLink: true },
}
const open = { setup_required: true, physical_window_active: true, remaining_seconds: 600 }
beforeEach(() => {
  vi.resetAllMocks()
  mocked.exportBackup.mockResolvedValue(new Blob(['encrypted']))
  mocked.encode.mockResolvedValue('encrypted-base64')
  mocked.previewBackup.mockResolvedValue({
    gateway_id: '1234',
    node_count: 5,
    created_at_ms: 0,
    hostname: 'restored',
    mdns_enabled: true,
  })
  mocked.restoreBackup.mockResolvedValue(undefined)
  mocked.setupStatus.mockResolvedValue(open)
})
afterEach(() => vi.useRealTimers())
async function select(wrapper: ReturnType<typeof mount>) {
  const input = wrapper.get('input[type=file]')
  Object.defineProperty(input.element, 'files', {
    configurable: true,
    value: [new File(['backup'], 'backup.oskbackup')],
  })
  await input.trigger('change')
  await flushPromises()
  await wrapper.get('input[type=password]').setValue('backup password')
}
describe('backup UI', () => {
  it('requires matching passwords and downloads only after successful export', async () => {
    const w = mount(BackupCard, { global })
    await w.findAll('input')[0].setValue('backup password')
    await w.findAll('input')[1].setValue('different password')
    await w.get('form').trigger('submit')
    expect(mocked.exportBackup).not.toHaveBeenCalled()
    await w.findAll('input')[1].setValue('backup password')
    await w.get('form').trigger('submit')
    await flushPromises()
    expect(mocked.download).toHaveBeenCalledOnce()
    expect((w.findAll('input')[0].element as HTMLInputElement).value).toBe('')
    w.unmount()
  })
  it('checks a file without physical presence but restores only with it', async () => {
    const w = mount(RestoreBackup, {
      global,
      props: { status: { ...open, physical_window_active: false } },
    })
    await select(w)
    expect(w.find('.physical-status').exists()).toBe(false)
    await w.get('form').trigger('submit')
    await flushPromises()
    expect(mocked.previewBackup).toHaveBeenCalledOnce()
    expect(w.text()).toContain('1234')
    await w.get('input[autocomplete=username]').setValue('admin')
    await w.get('input[autocomplete=new-password]').setValue('admin password')
    await w.get('input[type=checkbox]').setValue(true)
    expect(w.findAll('form')[1].get('button').attributes('disabled')).toBeDefined()
    await w.findAll('form')[1].trigger('submit')
    await flushPromises()
    expect(mocked.restoreBackup).not.toHaveBeenCalled()
    await w.setProps({ status: open })
    await w.findAll('form')[1].trigger('submit')
    await flushPromises()
    expect(mocked.restoreBackup).toHaveBeenCalledOnce()
    w.unmount()
  })
  it('discards the preview when the backup password changes', async () => {
    const w = mount(RestoreBackup, { global, props: { status: open } })
    await select(w)
    await w.get('form').trigger('submit')
    await flushPromises()
    await w.get('input[type=password]').setValue('other password')
    expect(w.find('dl').exists()).toBe(false)
    w.unmount()
  })
  it('shows a damaged-file error and never exposes a restore action', async () => {
    mocked.previewBackup.mockRejectedValue(new Error('invalid_backup'))
    const w = mount(RestoreBackup, { global, props: { status: open } })
    await select(w)
    await w.get('form').trigger('submit')
    await flushPromises()
    expect(w.get('[role=alert]').text()).toContain('Wrong backup password')
    expect(w.find('input[type=checkbox]').exists()).toBe(false)
    expect(mocked.restoreBackup).not.toHaveBeenCalled()
    w.unmount()
  })
  it('requires a new admin and explicit confirmation, then keeps the restart result', async () => {
    const w = mount(RestoreBackup, { global, props: { status: open } })
    await select(w)
    await w.get('form').trigger('submit')
    await flushPromises()
    // After the check, the confirmation moves down to the restore button.
    expect(w.findAll('.physical-status')).toHaveLength(1)
    expect(w.findAll('form')[1].find('.physical-status').exists()).toBe(true)
    await w.findAll('form')[1].trigger('submit')
    expect(mocked.restoreBackup).not.toHaveBeenCalled()
    await w.get('input[autocomplete=username]').setValue('admin')
    await w.get('input[autocomplete=new-password]').setValue('admin password')
    await w.get('input[type=checkbox]').setValue(true)
    await w.findAll('form')[1].trigger('submit')
    await flushPromises()
    expect(mocked.restoreBackup).toHaveBeenCalledWith(
      'encrypted-base64',
      'backup password',
      'admin',
      'admin password',
    )
    expect(w.text()).toContain('Backup restored')
    expect(w.get('a').attributes('href')).toBe('http://restored.local/login')
    expect(w.emitted('complete')).toHaveLength(1)
    w.unmount()
  })
  it('switches between a new installation and restore as tabs', async () => {
    vi.useFakeTimers()
    const w = mount(SetupView, { global, attachTo: document.body })
    await flushPromises()
    const tabs = () => w.findAll('[role=tab]')
    expect(tabs().map((tab) => tab.attributes('aria-selected'))).toEqual(['true', 'false'])
    expect(w.get('[role=tabpanel]').attributes('aria-labelledby')).toBe('setup-tab-new')
    await tabs()[1].trigger('click')
    expect(tabs().map((tab) => tab.attributes('tabindex'))).toEqual(['-1', '0'])
    expect(w.get('[role=tabpanel]').attributes('aria-labelledby')).toBe('setup-tab-restore')
    expect(w.find('input[type=file]').exists()).toBe(true)
    await w.get('[role=tablist]').trigger('keydown', { key: 'ArrowLeft' })
    expect(tabs()[0].attributes('aria-selected')).toBe('true')
    expect(document.activeElement).toBe(tabs()[0].element)
    w.unmount()
  })
  it('shows reset instructions instead of setup and restore controls after interrupted recovery', async () => {
    mocked.setupStatus.mockResolvedValue({
      ...open,
      recovery_required: true,
      physical_window_active: false,
    })
    vi.useFakeTimers()
    const w = mount(SetupView, { global })
    await flushPromises()
    expect(w.text()).toContain('Gateway reset required')
    expect(w.find('form').exists()).toBe(false)
    expect(w.find('[role=tablist]').exists()).toBe(false)
    w.unmount()
  })
})
