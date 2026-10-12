// Client-side mirror of the API-token rules the gateway enforces (see "Users
// and tokens" in API.md). As with users.ts, validating here is about telling
// someone why a button is disabled -- the gateway checks all of it again and a
// race still comes back as token_capacity_reached or mutation_busy.
import type { ApiToken } from '../api/types'
import { byteLength } from './format'

export const TOKEN_LIMIT = 8

// The name is stored in a fixed 32-byte field (kCredentialNameSize in
// GatewayStorage.h) and counted in UTF-8 bytes, not characters, so a Cyrillic
// name runs out of room at half the length someone would expect. Anything
// longer is refused with 422 invalid_token_values.
export const TOKEN_NAME_MAX_BYTES = 32

export function isValidTokenName(name: string): boolean {
  const bytes = byteLength(name)
  return bytes > 0 && bytes <= TOKEN_NAME_MAX_BYTES
}

export function canAddToken(tokens: ApiToken[]): boolean {
  return tokens.length < TOKEN_LIMIT
}

export function findTokenNamed(tokens: ApiToken[], name: string): ApiToken | undefined {
  return tokens.find((token) => token.name === name)
}

// A name is the whole draft. The gateway has one scope and grants it when the
// field is absent, so there is nothing else to choose and nothing else to get
// wrong.
export type TokenDraft = { name: string }

// `duplicate_name` is deliberately not an error: the gateway accepts two keys
// with the same name, and refusing here would invent a rule the API does not
// have. The card warns about it instead.
export function tokenDraftError(
  draft: TokenDraft,
  tokens: ApiToken[],
): 'invalid_name' | 'capacity_reached' | '' {
  if (!isValidTokenName(draft.name)) return 'invalid_name'
  if (!canAddToken(tokens)) return 'capacity_reached'
  return ''
}
