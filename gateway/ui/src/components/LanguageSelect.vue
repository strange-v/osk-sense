<script setup lang="ts">
// Language picker for the sign-in and setup screens, where the visitor has no
// account yet and no Settings page to reach. A combo box rather than the old
// two-button switcher so a third language costs nothing.
import { useI18n } from 'vue-i18n'
import { rememberLocale, SUPPORTED_LOCALES, type LocaleCode } from '../i18n/locales'

withDefaults(defineProps<{ compact?: boolean }>(), { compact: false })

const { locale } = useI18n()

function choose(event: Event) {
  const code = (event.target as HTMLSelectElement).value as LocaleCode
  locale.value = code
  rememberLocale(code)
}
</script>

<template>
  <select
    class="select"
    :class="{ compact }"
    :value="locale"
    :aria-label="$t('common.language')"
    @change="choose"
  >
    <option
      v-for="entry in SUPPORTED_LOCALES"
      :key="entry.code"
      :value="entry.code"
    >
      {{ entry.label }}
    </option>
  </select>
</template>
