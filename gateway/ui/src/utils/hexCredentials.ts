// Cleanup and parsing helpers for manually entered pairing credentials
// (device UID and factory key). Both are fixed-length hex strings; see
// POST /ui/pairing/open in API.md.
export const PAIRING_UID_HEX_LENGTH = 20 // 10-byte device UID
export const PAIRING_KEY_HEX_LENGTH = 32 // 16-byte factory key

// Cyrillic letters that are visually identical to Latin hex digits on a
// standard keyboard layout switch (А/а, В/в, С/с, Е/е only — the remaining
// hex digits have no common Cyrillic look-alike).
const CYRILLIC_TO_LATIN: Record<string, string> = {
  А: 'A',
  а: 'A',
  В: 'B',
  в: 'B',
  С: 'C',
  с: 'C',
  Е: 'E',
  е: 'E',
}

// Strips everything but hex digits, mapping look-alike Cyrillic letters and
// separators/quotes/whitespace away first.
export function normalizeHexFragment(input: string): string {
  let result = ''
  for (const char of input) {
    const upper = (CYRILLIC_TO_LATIN[char] ?? char).toUpperCase()
    if (upper >= '0' && upper <= '9') result += upper
    else if (upper >= 'A' && upper <= 'F') result += upper
  }
  return result
}

export const HEX_GROUP_SEPARATOR = '-'

// Splits a normalized hex string into fixed-size groups for display.
export function groupHex(clean: string, groupSize: number): string {
  const groups: string[] = []
  for (let index = 0; index < clean.length; index += groupSize) {
    groups.push(clean.slice(index, index + groupSize))
  }
  return groups.join(HEX_GROUP_SEPARATOR)
}

// Caret position holding `digits` hex characters to its left, skipping past a
// separator so the next keystroke opens the following group.
export function caretAfterHexDigits(text: string, digits: number): number {
  if (digits <= 0) return 0
  let seen = 0
  for (let index = 0; index < text.length; index++) {
    if (text[index] !== HEX_GROUP_SEPARATOR) seen++
    if (seen < digits) continue
    return text[index + 1] === HEX_GROUP_SEPARATOR ? index + 2 : index + 1
  }
  return text.length
}

export interface ParsedPairingCredentials {
  uid?: string
  key?: string
}

function readField(source: unknown, keys: string[]): string | undefined {
  if (typeof source !== 'object' || source === null) return undefined
  const record = source as Record<string, unknown>
  for (const key of keys) {
    const value = record[key]
    if (typeof value === 'string') return value
  }
  return undefined
}

// Makes sense of a single pasted blob: full JSON from the pairing QR/label
// export, a loose "device_uid: ..., factory_key: ..." text, a single
// credential, or both concatenated back-to-back.
export function parsePairingPaste(text: string): ParsedPairingCredentials {
  const trimmed = text.trim()
  if (!trimmed) return {}

  // Someone with the QR text in hand (a desktop scanner, a chat message) can
  // paste it instead of photographing the label again.
  const uri = parsePairingUri(trimmed)
  if (uri) return { uid: uri.uid, key: uri.key }

  try {
    const parsed: unknown = JSON.parse(trimmed)
    const uidRaw = readField(parsed, ['device_uid', 'deviceUid', 'uid'])
    const keyRaw = readField(parsed, ['factory_key', 'factoryKey', 'key'])
    if (uidRaw !== undefined || keyRaw !== undefined) {
      return {
        uid: uidRaw ? normalizeHexFragment(uidRaw).slice(0, PAIRING_UID_HEX_LENGTH) : undefined,
        key: keyRaw ? normalizeHexFragment(keyRaw).slice(0, PAIRING_KEY_HEX_LENGTH) : undefined,
      }
    }
  } catch {
    // Not strict JSON — fall through to text heuristics below.
  }

  const uidMatch = trimmed.match(/device[_-]?uid["'\s:=]+([0-9a-fA-FА-Яа-я\-:\s]{10,})/i)
  const keyMatch = trimmed.match(/factory[_-]?key["'\s:=]+([0-9a-fA-FА-Яа-я\-:\s]{10,})/i)
  if (uidMatch || keyMatch) {
    return {
      uid: uidMatch
        ? normalizeHexFragment(uidMatch[1]).slice(0, PAIRING_UID_HEX_LENGTH)
        : undefined,
      key: keyMatch
        ? normalizeHexFragment(keyMatch[1]).slice(0, PAIRING_KEY_HEX_LENGTH)
        : undefined,
    }
  }

  const clean = normalizeHexFragment(trimmed)
  if (clean.length === PAIRING_UID_HEX_LENGTH) return { uid: clean }
  if (clean.length === PAIRING_KEY_HEX_LENGTH) return { key: clean }
  if (clean.length === PAIRING_UID_HEX_LENGTH + PAIRING_KEY_HEX_LENGTH) {
    return { uid: clean.slice(0, PAIRING_UID_HEX_LENGTH), key: clean.slice(PAIRING_UID_HEX_LENGTH) }
  }
  return {}
}

// --- Canonical pairing URI (printed QR code) -------------------------------
//
// web+opensmartkit:pair?v=1&family=sense&uid=<20 HEX>&key=<32 HEX>
//
// Parsed strictly rather than through normalizeHexFragment: a QR code is
// machine-produced, so anything that does not match exactly is a foreign or
// corrupted code and must be reported, not silently repaired.
export const PAIRING_URI_SCHEME = 'web+opensmartkit'
export const PAIRING_URI_ACTION = 'pair'
export const PAIRING_URI_VERSION = '1'
export const PAIRING_URI_FAMILY = 'sense'

export interface PairingUri {
  uid: string
  key: string
}

// Exactly one occurrence, or the code is ambiguous about which value applies.
function soleParam(params: URLSearchParams, name: string): string | null {
  const values = params.getAll(name)
  return values.length === 1 ? values[0] : null
}

function strictHex(value: string | null, length: number): string | null {
  if (value === null || value.length !== length) return null
  return /^[0-9a-fA-F]+$/.test(value) ? value.toUpperCase() : null
}

// Returns the credentials, or null when `text` is not a pairing URI this
// version understands. Unknown extra parameters are tolerated so a future
// optional hint does not break a v1 reader; every specified field must match.
export function parsePairingUri(text: string): PairingUri | null {
  const trimmed = text.trim()
  let url: URL
  try {
    url = new URL(trimmed)
  } catch {
    return null
  }

  // URL lowercases the scheme itself; schemes are case-insensitive per
  // RFC 3986, and QR alphanumeric mode can only carry uppercase.
  if (url.protocol !== `${PAIRING_URI_SCHEME}:`) return null
  // An opaque path: `web+opensmartkit://pair?...` would put "pair" in the
  // host and is a different, unsupported shape.
  if (url.host !== '' || url.pathname !== PAIRING_URI_ACTION) return null

  const params = url.searchParams
  if (soleParam(params, 'v') !== PAIRING_URI_VERSION) return null
  if (soleParam(params, 'family') !== PAIRING_URI_FAMILY) return null

  const uid = strictHex(soleParam(params, 'uid'), PAIRING_UID_HEX_LENGTH)
  const key = strictHex(soleParam(params, 'key'), PAIRING_KEY_HEX_LENGTH)
  if (!uid || !key) return null
  return { uid, key }
}
