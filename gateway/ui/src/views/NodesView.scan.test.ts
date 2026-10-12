// @vitest-environment jsdom
// Covers the pairing dialog's scan flow end to end at the component level:
// picking a photo, each reported outcome, and that a scan only fills the
// fields instead of starting pairing on its own.
import { flushPromises, mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { createRouter, createWebHistory } from 'vue-router'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import { en } from '../i18n/en'
import type { QrScanResult } from '../utils/qrScan'

const scanPairingQr = vi.hoisted(() => vi.fn())
vi.mock('../utils/qrScan', () => ({ scanPairingQr }))

const gatewayApi = vi.hoisted(() => ({
  nodes: vi.fn(async () => ({ registry_generation: 1, nodes: [] })),
  status: vi.fn(async () => ({
    registry: { records: 0 },
    telemetry: { nodes_seen: 0, updates: 0 },
    pairing: { active: false, remaining_seconds: 0 },
  })),
  poll: { status: vi.fn() },
  openPairing: vi.fn(async () => ({ active: true, remaining_seconds: 60 })),
  closePairing: vi.fn(async () => undefined),
}))
vi.mock('../api/client', () => ({
  api: gatewayApi,
  errorCode: () => 'generic',
  gatewayReachable: { value: true },
}))

import NodesView from './NodesView.vue'

const UID = '102132435465768798A9'
const KEY = '00112233445566778899AABBCCDDEEFF'

async function openDialog() {
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
    global: { plugins: [router, createI18n({ legacy: false, locale: 'en', messages: { en } })] },
  })
  await flushPromises()
  await wrapper.get('.page-heading button').trigger('click')
  await flushPromises()
  return wrapper
}

// jsdom cannot populate a real file input, so the picked file is attached to
// the element the change handler reads.
async function pickPhoto(wrapper: Awaited<ReturnType<typeof openDialog>>) {
  const input = wrapper.get('input[type="file"]')
  const element = input.element as HTMLInputElement
  Object.defineProperty(element, 'files', {
    configurable: true,
    value: [new File([''], 'label.jpg', { type: 'image/jpeg' })],
  })
  await input.trigger('change')
  await flushPromises()
}

const status = (wrapper: Awaited<ReturnType<typeof openDialog>>) =>
  wrapper.find('.scan-status').text()
const statusVariant = (wrapper: Awaited<ReturnType<typeof openDialog>>) =>
  wrapper
    .get('.scan-status')
    .classes()
    .filter((name) => name !== 'notice' && name !== 'scan-status')

beforeEach(() => {
  scanPairingQr.mockReset()
  gatewayApi.openPairing.mockClear()
})

describe('NodesView pairing QR scan', () => {
  it('masks the whole dialog while a decode holds the main thread', async () => {
    const wrapper = await openDialog()
    let finish: (result: QrScanResult) => void = () => {}
    scanPairingQr.mockReturnValue(
      new Promise<QrScanResult>((resolve) => {
        finish = resolve
      }),
    )
    await pickPhoto(wrapper)

    const mask = wrapper.get('.modal-mask')
    expect(mask.text()).toContain(en.pairing.scanState.scanning)
    expect(wrapper.get('[role="dialog"]').attributes('aria-busy')).toBe('true')
    // Nothing behind the mask is reachable: not the scan button, and not the
    // close button that would abandon the decode.
    expect(wrapper.get('.pairing-scan button').attributes('disabled')).toBeDefined()
    expect(wrapper.get('.modal-header .icon-button').attributes('disabled')).toBeDefined()
    // The mask carries the progress message, so the inline line stays quiet.
    expect(wrapper.find('.scan-status').exists()).toBe(false)

    finish({ status: 'no_code' })
    await flushPromises()
    expect(wrapper.find('.modal-mask').exists()).toBe(false)
    expect(wrapper.get('.pairing-scan button').attributes('disabled')).toBeUndefined()
    expect(status(wrapper)).toBe(en.pairing.scanState.no_code)
  })

  it('offers a camera-capable file input behind the scan button', async () => {
    const wrapper = await openDialog()
    const input = wrapper.get('input[type="file"]')
    expect(input.attributes('accept')).toBe('image/*')
    expect(input.attributes('capture')).toBe('environment')
    expect(wrapper.find('.pairing-scan button').text()).toContain(en.pairing.scan)
    expect(wrapper.find('.scan-status').exists()).toBe(false)
  })

  it('fills both credential fields from a scanned code without starting pairing', async () => {
    const wrapper = await openDialog()
    scanPairingQr.mockResolvedValue({
      status: 'read',
      credentials: { uid: UID, key: KEY },
    } satisfies QrScanResult)
    await pickPhoto(wrapper)

    expect(status(wrapper)).toBe(en.pairing.scanState.read)
    expect(statusVariant(wrapper)).toEqual(['success'])
    const fields = wrapper.findAll('.field-group textarea')
    expect(fields.map((field) => (field.element as HTMLTextAreaElement).value)).toEqual([
      '1021-3243-5465-7687-98A9',
      '0011-2233-4455-6677-8899-AABB-CCDD-EEFF',
    ])
    expect(gatewayApi.openPairing).not.toHaveBeenCalled()
    // "Add" is now enabled, but the user still has to press it.
    expect(wrapper.get('.modal-actions .button.primary').attributes('disabled')).toBeUndefined()
  })

  it('shows a distinct message for each failed outcome and leaves the fields alone', async () => {
    // A missed code is retryable, so it stays a warning; the other two mean
    // the wrong thing was scanned and are errors.
    for (const [outcome, variant] of [
      ['no_code', []],
      ['unsupported', ['error']],
      ['unreadable', ['error']],
    ] as const) {
      const wrapper = await openDialog()
      scanPairingQr.mockResolvedValue({ status: outcome } satisfies QrScanResult)
      await pickPhoto(wrapper)
      expect(status(wrapper)).toBe(en.pairing.scanState[outcome])
      expect(statusVariant(wrapper)).toEqual(variant)
      expect(
        wrapper
          .findAll('.field-group textarea')
          .every((field) => (field.element as HTMLTextAreaElement).value === ''),
      ).toBe(true)
      expect(wrapper.get('.modal-actions .button.primary').attributes('disabled')).toBeDefined()
    }
  })

  it('reports a decoder failure instead of leaving the dialog stuck on "scanning"', async () => {
    const wrapper = await openDialog()
    scanPairingQr.mockRejectedValue(new Error('decoder blew up'))
    await pickPhoto(wrapper)
    expect(status(wrapper)).toBe(en.pairing.scanState.unreadable)
  })

  it('clears the file input so the same photo can be picked again', async () => {
    const wrapper = await openDialog()
    scanPairingQr.mockResolvedValue({ status: 'no_code' } satisfies QrScanResult)
    await pickPhoto(wrapper)
    expect((wrapper.get('input[type="file"]').element as HTMLInputElement).value).toBe('')
  })

  it('clears the scanned factory key and the scan message when the dialog is closed', async () => {
    const wrapper = await openDialog()
    scanPairingQr.mockResolvedValue({
      status: 'read',
      credentials: { uid: UID, key: KEY },
    } satisfies QrScanResult)
    await pickPhoto(wrapper)

    await wrapper.get('.modal-actions .button.secondary').trigger('click')
    await flushPromises()
    await wrapper.get('.page-heading button').trigger('click')
    await flushPromises()

    expect(
      wrapper
        .findAll('.field-group textarea')
        .every((field) => (field.element as HTMLTextAreaElement).value === ''),
    ).toBe(true)
    expect(wrapper.find('.scan-status').exists()).toBe(false)
  })
})
