// Theme preference. "auto" leaves the choice to prefers-color-scheme by
// removing the attribute; the stylesheet is dark-first, so that is also what
// an unconfigured browser gets. Applied before mount so the page never paints
// in the wrong theme first.
export const THEME_PREFERENCES = ['auto', 'light', 'dark'] as const

export type ThemePreference = (typeof THEME_PREFERENCES)[number]

const STORAGE_KEY = 'osk-sense.theme'
const THEME_COLORS: Record<'light' | 'dark', string> = { light: '#f4f6f9', dark: '#141a24' }

export function storedTheme(): ThemePreference {
  const saved = localStorage.getItem(STORAGE_KEY)
  return THEME_PREFERENCES.includes(saved as ThemePreference) ? (saved as ThemePreference) : 'auto'
}

export function applyTheme(preference: ThemePreference) {
  const root = document.documentElement
  if (preference === 'auto') root.removeAttribute('data-theme')
  else root.setAttribute('data-theme', preference)

  // Keep the browser chrome (mobile address bar) in step with the page.
  const resolved =
    preference === 'auto'
      ? matchMedia('(prefers-color-scheme: light)').matches
        ? 'light'
        : 'dark'
      : preference
  document
    .querySelector('meta[name="theme-color"]')
    ?.setAttribute('content', THEME_COLORS[resolved])
}

export function rememberTheme(preference: ThemePreference) {
  localStorage.setItem(STORAGE_KEY, preference)
  applyTheme(preference)
}
