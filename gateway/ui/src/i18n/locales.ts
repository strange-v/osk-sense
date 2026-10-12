// Single source of truth for the languages the UI ships with. Adding one here
// (plus its message file) is all the language pickers need — they enumerate
// this list rather than hard-coding buttons.
export const SUPPORTED_LOCALES = [
  { code: 'en', label: 'English' },
  { code: 'uk', label: 'Українська' },
] as const

export type LocaleCode = (typeof SUPPORTED_LOCALES)[number]['code']

const STORAGE_KEY = 'osk-sense.locale'

export function isSupportedLocale(value: unknown): value is LocaleCode {
  return SUPPORTED_LOCALES.some((entry) => entry.code === value)
}

export function storedLocale(): LocaleCode | null {
  const saved = localStorage.getItem(STORAGE_KEY)
  return isSupportedLocale(saved) ? saved : null
}

export function preferredLocale(): LocaleCode {
  const browser = SUPPORTED_LOCALES.find((entry) =>
    navigator.language.toLowerCase().startsWith(entry.code),
  )
  return storedLocale() ?? browser?.code ?? 'en'
}

export function rememberLocale(code: LocaleCode) {
  localStorage.setItem(STORAGE_KEY, code)
  document.documentElement.lang = code
}
