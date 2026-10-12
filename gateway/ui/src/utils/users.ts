// Client-side mirror of the user rules the gateway enforces (see "Users and
// tokens" in API.md). Validating here is about telling someone why the button
// is disabled, not about trust: every rule is checked again on the gateway,
// and a race can still come back as last_admin_required or
// username_already_exists.
import type { GatewayUser } from '../api/types'
import { byteLength } from './format'

export const USER_LIMIT = 4
export const USERNAME_MAX = 32
export const PASSWORD_MIN_BYTES = 8
export const PASSWORD_MAX_BYTES = 128

// 1..32 lowercase ASCII letters, digits, dot, underscore or hyphen.
const USERNAME_PATTERN = /^[a-z0-9._-]{1,32}$/

export function isValidUsername(username: string): boolean {
  return USERNAME_PATTERN.test(username)
}

// The gateway counts UTF-8 bytes, not characters: a Cyrillic passphrase runs
// out of room at half the character count someone would expect.
export function isValidPassword(password: string): boolean {
  const bytes = byteLength(password)
  return bytes >= PASSWORD_MIN_BYTES && bytes <= PASSWORD_MAX_BYTES
}

// The gateway refuses to remove, disable, or demote the last enabled admin,
// so the UI has to know which record that is to explain itself.
export function isLastEnabledAdmin(users: GatewayUser[], user: GatewayUser): boolean {
  if (user.role !== 'admin' || !user.enabled) return false
  return users.filter((entry) => entry.role === 'admin' && entry.enabled).length === 1
}

export function canAddUser(users: GatewayUser[]): boolean {
  return users.length < USER_LIMIT
}

export type UserDraft = {
  username: string
  password: string
  role: 'admin' | 'viewer'
  enabled: boolean
}

// `existing` is undefined when adding. On edit an empty password means "keep
// the stored one", so it is only validated when something was typed.
export function userDraftError(
  draft: UserDraft,
  users: GatewayUser[],
  existing?: GatewayUser,
): 'invalid_username' | 'username_taken' | 'invalid_password' | 'last_admin_required' | '' {
  if (!isValidUsername(draft.username)) return 'invalid_username'
  if (users.some((entry) => entry.username === draft.username && entry.id !== existing?.id))
    return 'username_taken'
  const passwordRequired = existing === undefined
  if (passwordRequired || draft.password.length > 0) {
    if (!isValidPassword(draft.password)) return 'invalid_password'
  }
  if (
    existing &&
    isLastEnabledAdmin(users, existing) &&
    (draft.role !== 'admin' || !draft.enabled)
  ) {
    return 'last_admin_required'
  }
  return ''
}
