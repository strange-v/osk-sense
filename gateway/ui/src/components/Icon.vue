<script setup lang="ts">
// Thin wrapper around the inline MDI sprite defined in index.html (see the
// comment there for how to add new icons). Sizes itself at 1em so it follows
// the font-size of whatever container it sits in, matching how the previous
// Unicode-glyph icons were sized.
defineProps<{ name: string }>()
</script>

<template>
  <svg
    class="icon"
    aria-hidden="true"
    focusable="false"
  >
    <use :href="`#i-${name}`" />
  </svg>
</template>

<style scoped>
.icon {
  width: 1em;
  height: 1em;
  flex: none;
  display: inline-block;
  vertical-align: -0.125em;
  fill: currentColor;
}
</style>
