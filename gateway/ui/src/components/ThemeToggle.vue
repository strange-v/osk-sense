<script setup lang="ts">
// Three-way theme control: following the system is a real choice, not the
// absence of one, so it gets its own segment instead of being lost after the
// first click of a two-state toggle.
import { ref } from 'vue'
import Icon from './Icon.vue'
import { rememberTheme, storedTheme, THEME_PREFERENCES, type ThemePreference } from '../theme'

const preference = ref<ThemePreference>(storedTheme())
const icons: Record<ThemePreference, string> = {
  auto: 'theme-light-dark',
  light: 'weather-sunny',
  dark: 'weather-night',
}

function choose(value: ThemePreference) {
  preference.value = value
  rememberTheme(value)
}
</script>

<template>
  <div
    class="segmented"
    role="group"
    :aria-label="$t('theme.label')"
  >
    <button
      v-for="value in THEME_PREFERENCES"
      :key="value"
      :class="{ active: preference === value }"
      :aria-pressed="preference === value"
      :aria-label="$t(`theme.${value}`)"
      :title="$t(`theme.${value}`)"
      type="button"
      @click="choose(value)"
    >
      <Icon :name="icons[value]" />
    </button>
  </div>
</template>
