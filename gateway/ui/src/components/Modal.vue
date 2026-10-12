<script setup lang="ts">
// Reusable dialog chrome: backdrop + panel + a fixed-height header (title and
// close button only, no subtitle). Body content is left entirely to the
// caller via the default slot. The `title` slot replaces the heading when the
// title itself is editable (the node card renames in place); the `title` prop
// still names the dialog for assistive technology. Use this for every new
// popup instead of hand-rolling backdrop/panel markup.
//
// Work in progress has one house style, in two sizes:
//
//  * Button level -- `:disabled="busy || invalid"` plus a label that swaps to
//    the "-ing" string ("Save" -> "Saving…"). This is the default for a
//    request the user started from that button, and what every action button
//    in the app does.
//  * Dialog level -- the `busy` prop here, which masks the whole panel with a
//    spinner and that label. Reach for it when the rest of the dialog must
//    not be touched meanwhile, or when the work blocks the main thread and a
//    disabled button alone would leave a frozen-looking dialog (the pairing
//    QR decode is the case it was built for).
//
// Do not use the mask for a quick request that a disabled button already
// covers: a mask that appears and vanishes within a frame reads as a flicker.
import Icon from './Icon.vue'

// `busy`: the label to show over the mask, or undefined while idle.
const props = defineProps<{ title: string; busy?: string }>()
const emit = defineEmits<{ close: [] }>()

// A click is dispatched to the element that contains both the press and the
// release, so selecting text in a field and letting go past the panel's edge
// is a click on the backdrop. Only a press that began on the backdrop too
// dismisses the dialog.
let pressedBackdrop = false

function onBackdropPointerDown(event: PointerEvent) {
  pressedBackdrop = event.target === event.currentTarget
}

// A masked dialog cannot be dismissed: the operation would carry on behind a
// closed dialog and land on state nobody is watching.
function onBackdropClick(event: MouseEvent) {
  const dismiss = pressedBackdrop && event.target === event.currentTarget && !props.busy
  pressedBackdrop = false
  if (dismiss) emit('close')
}
</script>

<template>
  <div
    class="modal-backdrop"
    @pointerdown="onBackdropPointerDown"
    @click="onBackdropClick"
  >
    <section
      class="modal"
      role="dialog"
      aria-modal="true"
      :aria-label="title"
      :aria-busy="busy ? 'true' : undefined"
    >
      <header class="modal-header">
        <slot name="title"
          ><h2>{{ title }}</h2></slot
        >
        <button
          class="icon-button"
          type="button"
          :disabled="!!busy"
          :aria-label="$t('common.close')"
          @click="emit('close')"
        >
          <Icon name="close" />
        </button>
      </header>
      <div class="modal-body">
        <slot />
      </div>
      <div
        v-if="busy"
        class="modal-mask"
        role="status"
      >
        <span class="loader"></span>
        <p>{{ busy }}</p>
      </div>
    </section>
  </div>
</template>

<style scoped>
.modal-backdrop {
  position: fixed;
  inset: 0;
  z-index: 10;
  display: grid;
  place-items: center;
  padding: var(--space-4);
  background: #000000a6;
}
.modal {
  position: relative;
  width: min(500px, 100%);
  max-height: calc(100vh - 32px);
  overflow: auto;
  border: 1px solid var(--line);
  border-radius: var(--radius-lg);
  background: var(--surface);
  box-shadow: var(--shadow-lg);
}
.modal-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--space-4);
  height: 52px;
  flex: none;
  padding: 0 var(--space-5);
  border-bottom: 1px solid var(--line);
}
.modal-header h2 {
  font-size: var(--text-lg);
}
/* Pull the button out by half its own padding so the glyph, not the invisible
   hit area around it, lines up with the title's edge. */
.modal-header .icon-button {
  margin-right: calc(var(--space-2) * -1);
}
.modal-body {
  padding: var(--space-5);
}
/* Covers the panel rather than the viewport, so the title stays readable and
   the dialog does not appear to jump to a different surface. Opaque enough to
   read the label against, sheer enough to keep the context behind it. */
.modal-mask {
  position: absolute;
  inset: 0;
  z-index: 1;
  display: grid;
  align-content: center;
  justify-items: center;
  gap: var(--space-1);
  padding: var(--space-5);
  background: color-mix(in srgb, var(--surface) 88%, transparent);
  text-align: center;
}
.modal-mask p {
  color: var(--ink-soft);
  font-size: var(--text-sm);
}

@media (max-width: 640px) {
  .modal-header {
    padding-inline: var(--space-4);
  }
  .modal-body {
    padding: var(--space-4);
  }
}
</style>
