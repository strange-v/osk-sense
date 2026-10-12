<script setup lang="ts">
// Fixed-length hex entry for the pairing device UID and factory key: one field
// that inserts group separators as you type, rather than a row of per-block
// inputs that could not fit a phone.
//
// It is a textarea, not an input, for one reason: an input never wraps. The
// 32-character factory key needs 39 columns once separated, which only fits a
// narrow screen at a font size too small to check against a printed label.
// Separators give the browser break opportunities, so the value wraps at group
// boundaries and stays legible.
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { caretAfterHexDigits, groupHex, normalizeHexFragment } from '../utils/hexCredentials'

const props = withDefaults(
  defineProps<{
    modelValue: string
    length: number
    groupSize?: number
    label: string
  }>(),
  { groupSize: 4 },
)

const emit = defineEmits<{ 'update:modelValue': [value: string]; complete: [] }>()

const field = ref<HTMLTextAreaElement | null>(null)

const group = (clean: string) => groupHex(clean, props.groupSize)
const display = computed(() => group(props.modelValue))
const complete = computed(() => props.modelValue.length === props.length)

function fitHeight() {
  const element = field.value
  if (!element) return
  element.style.height = 'auto'
  element.style.height = `${element.scrollHeight}px`
}

function onInput(event: Event) {
  const element = event.target as HTMLTextAreaElement
  const caret = element.selectionStart ?? element.value.length
  let digits = normalizeHexFragment(element.value.slice(0, caret)).length
  let clean = normalizeHexFragment(element.value).slice(0, props.length)

  // A delete that landed on a separator removes a character regrouping puts
  // straight back, so the value would not change and the field would look
  // stuck. Drop the hex digit the keystroke was aimed at instead. Derived from
  // the resulting value rather than from a caret nudged during keydown, which
  // depends on the browser's own delete handling running afterwards.
  const swallowedSeparator =
    clean === props.modelValue && element.value.length < group(props.modelValue).length
  if (swallowedSeparator) {
    const inputType = (event as InputEvent).inputType
    if (inputType === 'deleteContentForward')
      clean = clean.slice(0, digits) + clean.slice(digits + 1)
    else if (digits > 0) {
      clean = clean.slice(0, digits - 1) + clean.slice(digits)
      digits -= 1
    }
  }

  const text = group(clean)
  element.value = text
  const position = caretAfterHexDigits(text, Math.min(digits, clean.length))
  element.setSelectionRange(position, position)
  fitHeight()

  emit('update:modelValue', clean)
  if (clean.length === props.length) emit('complete')
}

// The field holds one value, never a newline.
function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Enter') event.preventDefault()
}

watch(
  () => props.modelValue,
  () => void nextTick(fitHeight),
)

// The value wraps, so its height depends on the field's width: fit it once the
// field exists (the dialog can open with credentials already pasted in) and
// again whenever the viewport changes, including on rotation.
onMounted(() => {
  fitHeight()
  window.addEventListener('resize', fitHeight)
})
onBeforeUnmount(() => window.removeEventListener('resize', fitHeight))

defineExpose({ focus: () => field.value?.focus() })
</script>

<template>
  <div class="hex-input">
    <textarea
      ref="field"
      rows="1"
      :value="display"
      :aria-label="label"
      inputmode="text"
      autocomplete="off"
      autocorrect="off"
      autocapitalize="characters"
      spellcheck="false"
      @input="onInput"
      @keydown="onKeydown"
    ></textarea>
    <small :class="{ complete }">{{ modelValue.length }} / {{ length }}</small>
  </div>
</template>

<style scoped>
.hex-input {
  display: grid;
  gap: var(--space-1);
  justify-items: end;
}
.hex-input textarea {
  width: 100%;
  min-height: 0;
  overflow: hidden;
  padding: var(--space-2) var(--space-3);
  border: 1px solid var(--line-strong);
  border-radius: var(--radius-md);
  color: var(--ink);
  background: var(--bg);
  font-family: var(--font-mono);
  font-size: var(--text-lg);
  line-height: 1.6;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  overflow-wrap: anywhere;
  resize: none;
}
.hex-input textarea:focus {
  border-color: var(--primary);
  outline: none;
}
.hex-input small {
  color: var(--muted);
  font-size: var(--text-xs);
  font-variant-numeric: tabular-nums;
}
.hex-input small.complete {
  color: var(--success);
}
</style>
