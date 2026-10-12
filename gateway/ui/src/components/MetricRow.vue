<script setup lang="ts">
interface Props {
  label: string
  value: string | number
}

defineProps<Props>()
</script>
<template>
  <div class="metric">
    <dt>{{ label }}</dt>
    <dd>{{ value }}</dd>
  </div>
</template>
