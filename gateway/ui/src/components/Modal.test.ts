// @vitest-environment jsdom
// A dialog closes on a click that begins and ends on its backdrop -- never on
// a text selection that started inside the panel and was released outside it.
import { mount } from '@vue/test-utils'
import { createI18n } from 'vue-i18n'
import { describe, expect, it } from 'vitest'
import { en } from '../i18n/en'
import Modal from './Modal.vue'

const mountModal = (busy?: string) =>
  mount(Modal, {
    props: { title: 'Dialog', busy },
    slots: { default: '<input class="field">' },
    global: { plugins: [createI18n({ legacy: false, locale: 'en', messages: { en } })] },
  })

describe('Modal', () => {
  it('closes on a click on the backdrop', async () => {
    const wrapper = mountModal()
    await wrapper.get('.modal-backdrop').trigger('pointerdown')
    await wrapper.get('.modal-backdrop').trigger('click')
    expect(wrapper.emitted('close')).toHaveLength(1)
  })

  it('stays open when a press inside the panel is released over the backdrop', async () => {
    const wrapper = mountModal()
    await wrapper.get('.field').trigger('pointerdown')
    await wrapper.get('.modal-backdrop').trigger('click')
    expect(wrapper.emitted('close')).toBeUndefined()

    // The press is forgotten once used: a later backdrop click needs its own.
    await wrapper.get('.modal-backdrop').trigger('click')
    expect(wrapper.emitted('close')).toBeUndefined()
  })

  it('stays open while busy', async () => {
    const wrapper = mountModal('Working…')
    await wrapper.get('.modal-backdrop').trigger('pointerdown')
    await wrapper.get('.modal-backdrop').trigger('click')
    expect(wrapper.emitted('close')).toBeUndefined()
  })

  it('closes from its close button', async () => {
    const wrapper = mountModal()
    await wrapper.get('.modal-header .icon-button').trigger('click')
    expect(wrapper.emitted('close')).toHaveLength(1)
  })
})
