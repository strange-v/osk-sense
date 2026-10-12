import { describe, expect, it } from 'vitest'
import {
  caretAfterHexDigits,
  groupHex,
  normalizeHexFragment,
  parsePairingPaste,
  parsePairingUri,
} from './hexCredentials'

describe('normalizeHexFragment', () => {
  it('strips separators, quotes, and whitespace', () => {
    expect(normalizeHexFragment('10-21-32-43-54-65-76-87-98-A9')).toBe('102132435465768798A9')
  })
  it('uppercases lowercase hex', () => {
    expect(normalizeHexFragment('a9f0')).toBe('A9F0')
  })
  it('maps look-alike Cyrillic letters to Latin hex digits', () => {
    expect(normalizeHexFragment('АВСЕавсе')).toBe('ABCEABCE')
  })
  it('drops characters outside 0-9A-F', () => {
    expect(normalizeHexFragment('10G21Z32')).toBe('102132')
  })
})

describe('parsePairingPaste', () => {
  it('reads device_uid and factory_key from JSON', () => {
    const json = JSON.stringify({
      device_uid: '102132435465768798A9',
      factory_key: '00112233445566778899AABBCCDDEEFF',
    })
    expect(parsePairingPaste(json)).toEqual({
      uid: '102132435465768798A9',
      key: '00112233445566778899AABBCCDDEEFF',
    })
  })
  it('reads camelCase JSON keys', () => {
    const json = JSON.stringify({
      deviceUid: '102132435465768798a9',
      factoryKey: '00112233445566778899aabbccddeeff',
    })
    expect(parsePairingPaste(json)).toEqual({
      uid: '102132435465768798A9',
      key: '00112233445566778899AABBCCDDEEFF',
    })
  })
  it('reads a loose "key: value" text block', () => {
    const text =
      'device_uid: 10-21-32-43-54-65-76-87-98-A9\nfactory_key: 00112233445566778899AABBCCDDEEFF'
    expect(parsePairingPaste(text)).toEqual({
      uid: '102132435465768798A9',
      key: '00112233445566778899AABBCCDDEEFF',
    })
  })
  it('assigns a bare 20-character hex blob to the UID', () => {
    expect(parsePairingPaste('10-21-32-43-54-65-76-87-98-A9')).toEqual({
      uid: '102132435465768798A9',
    })
  })
  it('assigns a bare 32-character hex blob to the key', () => {
    expect(parsePairingPaste('00112233445566778899AABBCCDDEEFF')).toEqual({
      key: '00112233445566778899AABBCCDDEEFF',
    })
  })
  it('splits a concatenated uid+key blob', () => {
    const combined = '102132435465768798A900112233445566778899AABBCCDDEEFF'
    expect(parsePairingPaste(combined)).toEqual({
      uid: '102132435465768798A9',
      key: '00112233445566778899AABBCCDDEEFF',
    })
  })
  it('reads a pasted canonical pairing URI', () => {
    const text =
      'web+opensmartkit:pair?v=1&family=sense&uid=102132435465768798a9&key=00112233445566778899aabbccddeeff'
    expect(parsePairingPaste(text)).toEqual({
      uid: '102132435465768798A9',
      key: '00112233445566778899AABBCCDDEEFF',
    })
  })
  it('returns nothing for unrelated text', () => {
    expect(parsePairingPaste('hello world')).toEqual({})
  })
})

describe('groupHex', () => {
  it('splits a value into fixed-size groups', () => {
    expect(groupHex('102132435465768798A9', 4)).toBe('1021-3243-5465-7687-98A9')
  })
  it('leaves no trailing separator on a partial group', () => {
    expect(groupHex('10213', 4)).toBe('1021-3')
  })
  it('returns an empty string for an empty value', () => {
    expect(groupHex('', 4)).toBe('')
  })
})

describe('caretAfterHexDigits', () => {
  const text = '1021-3243-5465'
  it('places the caret at the start for no preceding digits', () => {
    expect(caretAfterHexDigits(text, 0)).toBe(0)
  })
  it('counts hex digits, not display characters', () => {
    expect(caretAfterHexDigits(text, 6)).toBe(7)
  })
  it('steps over a separator so the next keystroke opens the next group', () => {
    expect(caretAfterHexDigits(text, 4)).toBe(5)
  })
  it('clamps past the end of the value', () => {
    expect(caretAfterHexDigits(text, 99)).toBe(text.length)
  })
})

describe('parsePairingUri', () => {
  const UID = '102132435465768798A9'
  const KEY = '00112233445566778899AABBCCDDEEFF'
  const uri = (overrides: Record<string, string> = {}) => {
    const params = { v: '1', family: 'sense', uid: UID, key: KEY, ...overrides }
    return `web+opensmartkit:pair?${new URLSearchParams(params).toString()}`
  }

  it('reads a canonical pairing code', () => {
    expect(parsePairingUri(uri())).toEqual({ uid: UID, key: KEY })
  })
  it('normalizes lowercase hex to uppercase', () => {
    expect(parsePairingUri(uri({ uid: UID.toLowerCase(), key: KEY.toLowerCase() }))).toEqual({
      uid: UID,
      key: KEY,
    })
  })
  it('ignores surrounding whitespace', () => {
    expect(parsePairingUri(`\n  ${uri()}${'  '}\n`)).toEqual({ uid: UID, key: KEY })
  })
  it('accepts an unknown extra parameter within v1', () => {
    expect(parsePairingUri(`${uri()}&hint=kitchen`)).toEqual({ uid: UID, key: KEY })
  })

  it('rejects another scheme', () => {
    expect(parsePairingUri(uri().replace('web+opensmartkit:', 'https:'))).toBeNull()
    expect(parsePairingUri(uri().replace('web+opensmartkit:', 'web+opensmartkits:'))).toBeNull()
  })
  it('rejects another action', () => {
    expect(parsePairingUri(uri().replace(':pair?', ':claim?'))).toBeNull()
  })
  it('rejects an authority-style URI that puts the action in the host', () => {
    expect(parsePairingUri(uri().replace(':pair?', '://pair?'))).toBeNull()
  })
  it('rejects another version', () => {
    expect(parsePairingUri(uri({ v: '2' }))).toBeNull()
    expect(parsePairingUri(uri({ v: '01' }))).toBeNull()
  })
  it('rejects another device family', () => {
    expect(parsePairingUri(uri({ family: 'meter' }))).toBeNull()
  })
  it('rejects a UID that is not exactly 20 hex characters', () => {
    expect(parsePairingUri(uri({ uid: UID.slice(0, 19) }))).toBeNull()
    expect(parsePairingUri(uri({ uid: `${UID}A` }))).toBeNull()
    expect(parsePairingUri(uri({ uid: `${UID.slice(0, 19)}G` }))).toBeNull()
  })
  it('rejects a key that is not exactly 32 hex characters', () => {
    expect(parsePairingUri(uri({ key: KEY.slice(0, 31) }))).toBeNull()
    expect(parsePairingUri(uri({ key: `${KEY}0` }))).toBeNull()
    expect(parsePairingUri(uri({ key: `${KEY.slice(0, 31)}Z` }))).toBeNull()
  })
  it('rejects a separated UID rather than repairing it', () => {
    expect(parsePairingUri(uri({ uid: '10-21-32-43-54-65-76-87-98-A9' }))).toBeNull()
  })
  it('rejects a missing or duplicated parameter', () => {
    expect(parsePairingUri('web+opensmartkit:pair?v=1&family=sense&uid=' + UID)).toBeNull()
    expect(parsePairingUri(`${uri()}&uid=${UID}`)).toBeNull()
  })
  it('rejects text that is not a URI', () => {
    expect(parsePairingUri('hello world')).toBeNull()
    expect(parsePairingUri('')).toBeNull()
  })
})
