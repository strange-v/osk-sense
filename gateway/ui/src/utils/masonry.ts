import type { Directive } from 'vue'

// Grid rows this short let each card take exactly the height it needs, so a
// tall card no longer pushes its row neighbour down. Native CSS masonry is not
// available, and CSS columns would reorder the cards top to bottom.
export const MASONRY_ROW_PX = 4

// Rows a card spans: its height plus the gap below it, rounded up.
export function masonrySpan(height: number, gap: number, row = MASONRY_ROW_PX): number {
  return Math.max(1, Math.ceil((height + gap) / row))
}

interface MasonryState {
  resize: ResizeObserver
  mutation: MutationObserver
}
const states = new WeakMap<HTMLElement, MasonryState>()

function layout(grid: HTMLElement) {
  // The column gap doubles as the vertical gap, which the row spans supply.
  const gap = parseFloat(getComputedStyle(grid).columnGap) || 0
  for (const child of Array.from(grid.children) as HTMLElement[]) {
    child.style.gridRowEnd = `span ${masonrySpan(child.getBoundingClientRect().height, gap)}`
  }
}

function observeChildren(grid: HTMLElement, resize: ResizeObserver) {
  resize.disconnect()
  for (const child of Array.from(grid.children)) resize.observe(child)
}

// Lays a CSS grid's children out as masonry. Without ResizeObserver (tests,
// old browsers) the grid keeps its ordinary rows.
export const vMasonry: Directive<HTMLElement> = {
  mounted(grid) {
    if (typeof ResizeObserver === 'undefined') return
    grid.style.gridAutoRows = `${MASONRY_ROW_PX}px`
    grid.style.rowGap = '0'
    grid.style.alignItems = 'start'
    const resize = new ResizeObserver(() => layout(grid))
    const mutation = new MutationObserver(() => observeChildren(grid, resize))
    mutation.observe(grid, { childList: true })
    observeChildren(grid, resize)
    layout(grid)
    states.set(grid, { resize, mutation })
  },
  unmounted(grid) {
    const state = states.get(grid)
    state?.resize.disconnect()
    state?.mutation.disconnect()
    states.delete(grid)
  },
}
