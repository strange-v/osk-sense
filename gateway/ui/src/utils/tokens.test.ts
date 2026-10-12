import { describe, expect, it } from 'vitest'
import type { ApiToken } from '../api/types'
import {
  TOKEN_LIMIT,
  canAddToken,
  findTokenNamed,
  isValidTokenName,
  tokenDraftError,
} from './tokens'

const token = (over: Partial<ApiToken> & { id: number }): ApiToken => ({
  name: `t${over.id}`,
  enabled: true,
  created_at_ms: 1_700_000_000_000,
  scopes: ['telemetry:read'],
  ...over,
})

const fill = (count: number) =>
  Array.from({ length: count }, (_, index) => token({ id: index + 1 }))

describe('isValidTokenName', () => {
  it('accepts 1..32 UTF-8 bytes', () => {
    expect(isValidTokenName('a')).toBe(true)
    expect(isValidTokenName('Home Assistant')).toBe(true)
    expect(isValidTokenName('a'.repeat(32))).toBe(true)
    expect(isValidTokenName('')).toBe(false)
    expect(isValidTokenName('a'.repeat(33))).toBe(false)
  })

  it('counts bytes, not characters, so Cyrillic runs out at half the length', () => {
    // The gateway stores the name in a 32-byte field and answers 422 past it.
    expect(isValidTokenName('я'.repeat(16))).toBe(true)
    expect(isValidTokenName('я'.repeat(17))).toBe(false)
  })
})

describe('canAddToken', () => {
  it('stops at the gateway capacity', () => {
    expect(canAddToken(fill(TOKEN_LIMIT - 1))).toBe(true)
    expect(canAddToken(fill(TOKEN_LIMIT))).toBe(false)
  })
})

describe('findTokenNamed', () => {
  // Used to warn about a duplicate name; matching is exact, because two names
  // that differ only in case are two different names to the gateway.
  it('matches an existing name exactly', () => {
    const list = [token({ id: 1, name: 'Grafana' }), token({ id: 2, name: 'Home Assistant' })]
    expect(findTokenNamed(list, 'Home Assistant')?.id).toBe(2)
    expect(findTokenNamed(list, 'home assistant')).toBeUndefined()
    expect(findTokenNamed([], 'Home Assistant')).toBeUndefined()
  })
})

describe('tokenDraftError', () => {
  it('accepts a valid draft', () => {
    expect(tokenDraftError({ name: 'Grafana' }, [])).toBe('')
  })

  // A name is the whole draft: the gateway has one scope and grants it when
  // the field is absent, so there is nothing else the dialog can get wrong.
  it('reports the name and the capacity', () => {
    expect(tokenDraftError({ name: '' }, [])).toBe('invalid_name')
    expect(tokenDraftError({ name: 'ok' }, fill(TOKEN_LIMIT))).toBe('capacity_reached')
  })

  it('allows a duplicate name, because the gateway does', () => {
    const list = [token({ id: 1, name: 'Home Assistant' })]
    expect(tokenDraftError({ name: 'Home Assistant' }, list)).toBe('')
  })
})
