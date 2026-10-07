<template>
  <svg ref="svgRef" class="alignment-overlay" :class="{ 'alignment-overlay--drawing': drawing }"
    xmlns="http://www.w3.org/2000/svg" aria-hidden="true" @pointerdown="onPointerDown" @pointermove="onPointerMove"
    @pointerup="onPointerUp" @pointercancel="cancelDraft" @lostpointercapture="cancelDraft">
    <g v-for="line in lines" :key="line.id" class="alignment-line"
      :class="{ 'alignment-line--selected': line.id === selectedId }">
      <line class="alignment-line__outline" v-bind="toSvgCoords(line)" />
      <line class="alignment-line__stroke" v-bind="toSvgCoords(line)" />
      <line class="alignment-line__hit" v-bind="toSvgCoords(line)" :data-line-id="line.id"
        @pointerdown="onLinePointerDown(line.id, $event)" />
    </g>
    <g v-if="draft" class="alignment-line alignment-line--draft">
      <line class="alignment-line__outline" v-bind="toSvgCoords(draft)" />
      <line class="alignment-line__stroke" v-bind="toSvgCoords(draft)" />
    </g>
  </svg>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import type { AlignmentLine } from '@/composables/useAlignmentLines'
import { screenToNormalized, unrotatedSize, type Point } from '@/utils/previewGeometry'

type LineCoords = Omit<AlignmentLine, 'id'>

// Drags shorter than this (in normalized image units) are treated as clicks.
const MIN_LINE_LENGTH = 0.01

const props = defineProps<{
  lines: AlignmentLine[],
  drawing: boolean,
  /** Preview rotation in degrees (multiple of 90), applied by the parent via CSS to this overlay's container. */
  rotation: number,
}>()

const emit = defineEmits<{
  add: [line: LineCoords],
  deleteSelected: [],
}>()

const selectedId = defineModel<string | null>('selectedId', { default: null })

const svgRef = ref<SVGSVGElement | null>(null)
const draft = ref<LineCoords | null>(null)

let activePointerId: number | null = null
let dragStart: Point | null = null
let lastPointer: Point | null = null
let pressedLineId: string | null = null

const clamp01 = (value: number) => Math.min(1, Math.max(0, value))

const toSvgCoords = (line: LineCoords) => ({
  x1: `${line.x1 * 100}%`,
  y1: `${line.y1 * 100}%`,
  x2: `${line.x2 * 100}%`,
  y2: `${line.y2 * 100}%`,
})

/** Unrotated rendered size of the image (the overlay is rotated together with it). */
const imageSize = () => unrotatedSize(svgRef.value!.getBoundingClientRect(), props.rotation)

/** Converts a screen position into normalized (0..1) unrotated image coordinates. */
const toNormalized = (clientX: number, clientY: number): Point => {
  const point = screenToNormalized(svgRef.value!.getBoundingClientRect(), props.rotation, clientX, clientY)
  return { x: clamp01(point.x), y: clamp01(point.y) }
}

const buildLine = (start: Point, end: Point, snap: boolean): LineCoords => {
  if (!snap) {
    return { x1: start.x, y1: start.y, x2: end.x, y2: end.y }
  }
  // Compare in pixel space so "closer" matches what the user sees.
  const { width, height } = imageSize()
  const horizontal = Math.abs((end.x - start.x) * width) >= Math.abs((end.y - start.y) * height)
  return horizontal
    ? { x1: start.x, y1: start.y, x2: end.x, y2: start.y }
    : { x1: start.x, y1: start.y, x2: start.x, y2: end.y }
}

const resetDrag = () => {
  activePointerId = null
  dragStart = null
  lastPointer = null
  pressedLineId = null
  draft.value = null
}

const cancelDraft = (event: PointerEvent) => {
  if (event.pointerId === activePointerId) {
    resetDrag()
  }
}

const onLinePointerDown = (id: string, event: PointerEvent) => {
  // In draw mode the press is handled by the overlay (a short click still selects the line).
  if (props.drawing || event.button !== 0) return
  selectedId.value = id
}

const onPointerDown = (event: PointerEvent) => {
  if (!props.drawing || event.button !== 0 || activePointerId !== null || !svgRef.value) return
  event.preventDefault()
  activePointerId = event.pointerId
  try {
    svgRef.value.setPointerCapture(event.pointerId)
  } catch {
    // Capture is a nicety (keeps the drag alive outside the overlay); drawing still works without it.
  }
  pressedLineId = (event.target as Element | null)?.getAttribute?.('data-line-id') ?? null
  dragStart = toNormalized(event.clientX, event.clientY)
  lastPointer = dragStart
}

const onPointerMove = (event: PointerEvent) => {
  if (event.pointerId !== activePointerId || !dragStart) return
  lastPointer = toNormalized(event.clientX, event.clientY)
  draft.value = buildLine(dragStart, lastPointer, event.shiftKey)
}

const onPointerUp = (event: PointerEvent) => {
  if (event.pointerId !== activePointerId || !dragStart) return
  const end = toNormalized(event.clientX, event.clientY)
  const line = buildLine(dragStart, end, event.shiftKey)
  const clickedLineId = pressedLineId
  resetDrag()
  if (Math.hypot(line.x2 - line.x1, line.y2 - line.y1) < MIN_LINE_LENGTH) {
    // Tiny drag: treat as a click (select the line under the pointer, or deselect).
    selectedId.value = clickedLineId
    return
  }
  emit('add', line)
}

const isEditableTarget = (target: EventTarget | null) => {
  if (!(target instanceof HTMLElement)) return false
  return target.isContentEditable || ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName)
}

const onKeyDown = (event: KeyboardEvent) => {
  if (event.key === 'Shift' && dragStart && lastPointer) {
    draft.value = buildLine(dragStart, lastPointer, true)
    return
  }
  if ((event.key === 'Delete' || event.key === 'Backspace') && selectedId.value && !isEditableTarget(event.target)) {
    event.preventDefault()
    emit('deleteSelected')
  }
}

const onKeyUp = (event: KeyboardEvent) => {
  if (event.key === 'Shift' && dragStart && lastPointer) {
    draft.value = buildLine(dragStart, lastPointer, false)
  }
}

/** Clicking anywhere outside this overlay's lines deselects (except controls marked to keep the selection). */
const onDocumentPointerDown = (event: PointerEvent) => {
  if (!selectedId.value) return
  const target = event.target as Element | null
  if (target?.closest?.('[data-alignment-keep-selection]')) return
  const lineHit = target?.closest?.('[data-line-id]')
  if (lineHit && svgRef.value?.contains(lineHit)) return
  selectedId.value = null
}

watch(() => props.drawing, (drawing) => {
  if (!drawing) resetDrag()
})

onMounted(() => {
  document.addEventListener('pointerdown', onDocumentPointerDown, true)
  window.addEventListener('keydown', onKeyDown)
  window.addEventListener('keyup', onKeyUp)
})

onUnmounted(() => {
  document.removeEventListener('pointerdown', onDocumentPointerDown, true)
  window.removeEventListener('keydown', onKeyDown)
  window.removeEventListener('keyup', onKeyUp)
})
</script>

<style scoped>
.alignment-overlay {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  overflow: visible;
  z-index: 1;
  /* Let clicks/scrolling pass through to the image except on lines. */
  pointer-events: none;
}

.alignment-overlay--drawing {
  pointer-events: all;
  touch-action: none;
  cursor: crosshair;
}

.alignment-overlay line {
  vector-effect: non-scaling-stroke;
  stroke-linecap: round;
  fill: none;
}

.alignment-line__outline {
  stroke: rgba(0, 0, 0, 0.75);
  stroke-width: 4px;
  pointer-events: none;
}

.alignment-line__stroke {
  stroke: #ffeb3b;
  stroke-width: 2px;
  pointer-events: none;
}

.alignment-line__hit {
  stroke: transparent;
  stroke-width: 18px;
  pointer-events: stroke;
  cursor: pointer;
}

.alignment-overlay--drawing .alignment-line__hit {
  cursor: crosshair;
}

.alignment-line--selected .alignment-line__outline {
  stroke-width: 6px;
}

.alignment-line--selected .alignment-line__stroke {
  stroke: #ff4081;
  stroke-width: 3px;
}

.alignment-line--draft .alignment-line__stroke {
  stroke-dasharray: 6 4;
}
</style>
