import { describe, expect, it } from 'vitest'
import type { GatewayUser } from '../api/types'
import {
  canAddUser,
  isLastEnabledAdmin,
  isValidPassword,
  isValidUsername,
  userDraftError,
} from './users'

const user = (over: Partial<GatewayUser> & { id: number }): GatewayUser => ({
  username: `u${over.id}`,
  role: 'viewer',
  enabled: true,
  ...over,
})

const admin = user({ id: 1, username: 'admin', role: 'admin' })
const viewer = user({ id: 2, username: 'viewer' })

describe('isValidUsername', () => {
  it('accepts the documented character set', () => {
    for (const name of ['a', 'admin', 'home.assistant', 'a_b-c', 'user2', 'a'.repeat(32)]) {
      expect(isValidUsername(name)).toBe(true)
    }
  })
  it('rejects anything outside 1..32 lowercase ASCII, digits, . _ -', () => {
    for (const name of [
      '',
      'a'.repeat(33),
      'Admin',
      'user name',
      'user@host',
      'кирилиця',
      'user+1',
    ]) {
      expect(isValidUsername(name)).toBe(false)
    }
  })
})

describe('isValidPassword', () => {
  it('counts ASCII against the 8..128 byte limits', () => {
    expect(isValidPassword('1234567')).toBe(false)
    expect(isValidPassword('12345678')).toBe(true)
    expect(isValidPassword('a'.repeat(128))).toBe(true)
    expect(isValidPassword('a'.repeat(129))).toBe(false)
  })

  // Cyrillic is two bytes per character, so character count misleads in both
  // directions -- which is the whole reason this is measured in bytes.
  it('measures UTF-8 bytes rather than characters', () => {
    expect(isValidPassword('при')).toBe(false) // 3 chars, 6 bytes: too short
    expect(isValidPassword('парол')).toBe(true) // 5 chars, 10 bytes: long enough
    expect(isValidPassword('я'.repeat(64))).toBe(true) // 64 chars, 128 bytes
    expect(isValidPassword('я'.repeat(65))).toBe(false) // 65 chars, 130 bytes
  })
})

describe('isLastEnabledAdmin', () => {
  it('flags the only enabled admin', () => {
    expect(isLastEnabledAdmin([admin, viewer], admin)).toBe(true)
  })
  it('does not flag one of two enabled admins', () => {
    const second = user({ id: 3, role: 'admin' })
    expect(isLastEnabledAdmin([admin, second], admin)).toBe(false)
  })
  it('ignores disabled admins when counting', () => {
    const disabled = user({ id: 3, role: 'admin', enabled: false })
    expect(isLastEnabledAdmin([admin, disabled], admin)).toBe(true)
    expect(isLastEnabledAdmin([admin, disabled], disabled)).toBe(false)
  })
  it('never flags a viewer', () => {
    expect(isLastEnabledAdmin([viewer], viewer)).toBe(false)
  })
})

describe('canAddUser', () => {
  it('stops at the gateway capacity of four', () => {
    expect(canAddUser([admin, viewer])).toBe(true)
    expect(canAddUser([1, 2, 3].map((id) => user({ id })))).toBe(true)
    expect(canAddUser([1, 2, 3, 4].map((id) => user({ id })))).toBe(false)
  })
})

describe('userDraftError', () => {
  const draft = (over: Partial<Parameters<typeof userDraftError>[0]> = {}) => ({
    username: 'newuser',
    password: 'password1',
    role: 'viewer' as const,
    enabled: true,
    ...over,
  })

  it('accepts a valid new user', () => {
    expect(userDraftError(draft(), [admin])).toBe('')
  })
  it('rejects a duplicate username, but not the name the record already has', () => {
    expect(userDraftError(draft({ username: 'admin' }), [admin, viewer])).toBe('username_taken')
    expect(
      userDraftError(
        draft({ username: 'admin', password: '', role: 'admin' }),
        [admin, viewer],
        admin,
      ),
    ).toBe('')
  })
  it('requires a password when adding and not when editing', () => {
    expect(userDraftError(draft({ password: '' }), [admin])).toBe('invalid_password')
    expect(
      userDraftError(draft({ username: 'viewer', password: '' }), [admin, viewer], viewer),
    ).toBe('')
  })
  it('validates a password that was typed during an edit', () => {
    expect(
      userDraftError(draft({ username: 'viewer', password: 'short' }), [admin, viewer], viewer),
    ).toBe('invalid_password')
  })
  it('refuses to demote or disable the last enabled admin', () => {
    expect(
      userDraftError(
        draft({ username: 'admin', password: '', role: 'viewer' }),
        [admin, viewer],
        admin,
      ),
    ).toBe('last_admin_required')
    expect(
      userDraftError(
        draft({ username: 'admin', password: '', role: 'admin', enabled: false }),
        [admin, viewer],
        admin,
      ),
    ).toBe('last_admin_required')
  })
  it('allows demoting an admin while another enabled admin remains', () => {
    const second = user({ id: 3, username: 'admin2', role: 'admin' })
    expect(
      userDraftError(
        draft({ username: 'admin', password: '', role: 'viewer' }),
        [admin, second],
        admin,
      ),
    ).toBe('')
  })
})
