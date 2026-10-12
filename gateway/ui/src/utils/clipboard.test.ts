// @vitest-environment jsdom
import { afterEach, describe, expect, it, vi } from 'vitest'
import { copyText } from './clipboard'

describe('copyText', () => {
  const original = Object.getOwnPropertyDescriptor(document, 'execCommand')
  afterEach(() => {
    if (original) Object.defineProperty(document, 'execCommand', original)
    else Reflect.deleteProperty(document, 'execCommand')
  })

  function stubCommand(command: () => boolean) {
    const stub = vi.fn(command)
    Object.defineProperty(document, 'execCommand', { configurable: true, value: stub })
    return stub
  }

  it('copies the selected value and removes the field', () => {
    let selected = ''
    const command = stubCommand(() => {
      const field = document.querySelector('textarea')!
      selected = field.value.slice(field.selectionStart, field.selectionEnd)
      return true
    })
    expect(copyText('secret')).toBe(true)
    expect(command).toHaveBeenCalledWith('copy')
    expect(selected).toBe('secret')
    expect(document.querySelector('textarea')).toBeNull()
  })

  it('reports failure when the browser refuses to copy', () => {
    stubCommand(() => {
      throw new Error('denied')
    })
    expect(copyText('secret')).toBe(false)
    expect(document.querySelector('textarea')).toBeNull()
  })
})
