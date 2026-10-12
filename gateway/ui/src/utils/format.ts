// Display formatting shared by the overview and the node list so both read the
// same way. Takes the translator rather than importing i18n, to
// stay usable from any component and testable without a live app instance.
type Translate = (key: string, named?: Record<string, unknown>) => string

export function lastSeen(t: Translate, atMs: number | undefined): string {
  if (!atMs) return '—'
  const minutes = Math.max(0, Math.floor((Date.now() - atMs) / 60_000))
  if (minutes < 1) return t('time.underMinute')
  if (minutes < 60) return t('time.minutes', { value: minutes })
  const hours = Math.floor(minutes / 60)
  if (hours < 24) return t('time.hours', { value: hours })
  return t('time.days', { value: Math.floor(hours / 24) })
}

// RFM69 RSSI domain. The driver reports -RegRssiValue/2 from an 8-bit register
// (SX1231 6.4: RssiValue is twice the absolute dBm), so a genuine reading is
// -128..0 dBm and is negative for any real link -- 0 dBm at the antenna is a
// milliwatt. The driver also parks the value at 0 when nothing has been
// received yet (RFM69::receiveBegin), and the gateway's own lastRssi starts
// there, so 0 means "no measurement", not "perfect signal".
export const RSSI_MIN_DBM = -128
export const RSSI_MAX_DBM = -1

export function isMeasuredRssi(rssi: number | undefined): rssi is number {
  return rssi !== undefined && Number.isFinite(rssi) && rssi >= RSSI_MIN_DBM && rssi <= RSSI_MAX_DBM
}

export function signal(rssi: number | undefined): string {
  return isMeasuredRssi(rssi) ? `${rssi} dBm` : '—'
}

// Two decimals: a CR2032 spends its useful life between about 3.0 and 2.7 V,
// so tenths would hide most of the discharge.
export function volts(millivolts: number | undefined): string {
  return millivolts === undefined ? '—' : `${(millivolts / 1000).toFixed(2)} V`
}

export function megahertz(hz: number): string {
  // 868.00 MHz claims a precision the radio does not have. Trailing zeros go,
  // but genuine fractions must survive: 868.3 and 433.92 are real band centres.
  return `${Number((hz / 1e6).toFixed(3))} MHz`
}

// A node may legitimately have no name: the API accepts an empty display_name
// to clear one. Fall back to the radio id so a row is never blank.
export function nodeName(t: Translate, node: { display_name?: string; node_id: number }): string {
  return node.display_name || t('nodes.unnamed', { id: node.node_id })
}

// display_name limits are counted in UTF-8 bytes, not characters: Cyrillic
// costs two bytes apiece, so 48 bytes is about 24 Ukrainian letters.
export function byteLength(value: string): number {
  return new TextEncoder().encode(value).length
}

// The longest start of `value` that fits in `max` UTF-8 bytes, cut between
// characters, never inside one. A lone surrogate counts the 3 bytes of the
// U+FFFD that TextEncoder writes in its place.
export function truncateToBytes(value: string, max: number): string {
  let used = 0
  let end = 0
  for (const char of value) {
    const code = char.codePointAt(0) ?? 0
    const size = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4
    if (used + size > max) break
    used += size
    end += char.length
  }
  return value.slice(0, end)
}

// A DNS label, mirroring validHostname() in GatewayStorage.cpp. Empty is valid
// and means the gateway keeps its `osk-hub-<mac>` default.
export const HOSTNAME_MAX_BYTES = 32

export function isValidHostname(value: string): boolean {
  return (
    value === '' ||
    (value.length <= HOSTNAME_MAX_BYTES && /^[a-z0-9]([a-z0-9-]*[a-z0-9])?$/.test(value))
  )
}

// Dates follow the language chosen in the UI rather than the browser default.
// Passing undefined as the locale, as this used to, resolves to whatever the
// browser is set to -- commonly en-US, which prints 12-hour times with AM/PM no
// matter how the operating system is configured. The hour cycle is pinned to
// h23 because a device console reads better with unambiguous 24-hour times.
// Whole seconds as m:ss, for a window that closes on its own.
export function countdown(seconds: number): string {
  const whole = Math.max(0, Math.floor(seconds))
  return `${Math.floor(whole / 60)}:${String(whole % 60).padStart(2, '0')}`
}

export function dateTime(locale: string, atMs: number | undefined): string {
  if (!atMs) return '—'
  return new Intl.DateTimeFormat(locale, {
    dateStyle: 'medium',
    timeStyle: 'short',
    hourCycle: 'h23',
  }).format(atMs)
}

export function timeOfDay(locale: string, at: Date): string {
  return new Intl.DateTimeFormat(locale, { timeStyle: 'medium', hourCycle: 'h23' }).format(at)
}
