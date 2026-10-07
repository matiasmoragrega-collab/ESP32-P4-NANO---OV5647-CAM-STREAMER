<template>
  <!-- Only rendered in move mode; covers the (rotated) camera image and captures drag / wheel input. -->
  <div ref="surfaceRef" class="overlay-move-surface" @pointerdown="onPointerDown" @pointermove="onPointerMove"
    @pointerup="endDrag" @pointercancel="endDrag" @lostpointercapture="endDrag" @wheel="onWheel" />
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { screenVectorToImage, unrotatedSize, type Point } from '@/utils/previewGeometry'

const props = defineProps<{
  /** Preview rotation in degrees (multiple of 90), applied by the parent via CSS to this surface's container. */
  rotation: number,
}>()

const emit = defineEmits<{
  /** A drag started; `drag` deltas are relative to this moment. */
  dragStart: [],
  /** Total pointer movement since `dragStart`, as fractions of the unrotated image width / height. */
  drag: [delta: Point],
  /** One wheel notch: +1 = zoom in, -1 = zoom out; `fine` while Shift is held. */
  zoom: [direction: 1 | -1, fine: boolean],
}>()

const surfaceRef = ref<HTMLDivElement | null>(null)

let activePointerId: number | null = null
let start: { clientX: number, clientY: number, width: number, height: number } | null = null

const onPointerDown = (event: PointerEvent) => {
  if (event.button !== 0 || activePointerId !== null || !surfaceRef.value) return
  event.preventDefault()
  const { width, height } = unrotatedSize(surfaceRef.value.getBoundingClientRect(), props.rotation)
  if (!(width > 0 && height > 0)) return
  activePointerId = event.pointerId
  try {
    surfaceRef.value.setPointerCapture(event.pointerId)
  } catch {
    // Capture only keeps the drag alive outside the preview; moving still works without it.
  }
  start = { clientX: event.clientX, clientY: event.clientY, width, height }
  emit('dragStart')
}

const onPointerMove = (event: PointerEvent) => {
  if (event.pointerId !== activePointerId || !start) return
  const delta = screenVectorToImage(event.clientX - start.clientX, event.clientY - start.clientY, props.rotation)
  emit('drag', { x: delta.x / start.width, y: delta.y / start.height })
}

const endDrag = (event: PointerEvent) => {
  if (event.pointerId !== activePointerId) return
  activePointerId = null
  start = null
}

const onWheel = (event: WheelEvent) => {
  // Shift+wheel is reported as horizontal scrolling by some browsers.
  const delta = event.deltaY !== 0 ? event.deltaY : event.deltaX
  if (delta === 0) return
  event.preventDefault()
  emit('zoom', delta < 0 ? 1 : -1, event.shiftKey)
}
</script>

<style scoped>
.overlay-move-surface {
  position: absolute;
  inset: 0;
  /* Above the overlay image and the alignment lines while moving. */
  z-index: 2;
  cursor: move;
  touch-action: none;
}
</style>
