// @vitest-environment jsdom
import { mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { describe, expect, it } from 'vitest'
import { en } from '../i18n/en'
import PhysicalConfirmation from './PhysicalConfirmation.vue'

const global = { plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })] }
const status = (active: boolean, remaining = 0) => ({
  setup_required: true,
  physical_window_active: active,
  remaining_seconds: remaining,
})

describe('PhysicalConfirmation', () => {
  it('asks for the gateway button while the window is closed', () => {
    const w = mount(PhysicalConfirmation, { global, props: { status: status(false) } })
    expect(w.text()).toContain(en.setup.physicalTitle)
    expect(w.text()).toContain(en.setup.physicalClosed)
    expect(w.classes()).not.toContain('open')
  })
  it('counts down once the button was pressed', () => {
    const w = mount(PhysicalConfirmation, { global, props: { status: status(true, 125) } })
    expect(w.text()).toContain(en.setup.physicalOpenTitle)
    expect(w.text()).toContain('2:05')
    expect(w.classes()).toContain('open')
    expect(w.classes()).not.toContain('ending')
  })
  it('warns during the last half-minute', () => {
    const w = mount(PhysicalConfirmation, { global, props: { status: status(true, 30) } })
    expect(w.classes()).toContain('ending')
  })
  it('stays closed when recovery blocks setup', () => {
    const w = mount(PhysicalConfirmation, {
      global,
      props: { status: { ...status(true, 60), recovery_required: true } },
    })
    expect(w.text()).toContain(en.setup.physicalClosed)
  })
})
