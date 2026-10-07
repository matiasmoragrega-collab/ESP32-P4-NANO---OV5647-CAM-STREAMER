<template>
  <!-- Display-only undistorted copy of the live frame, covering the camera picture (under overlay image / guides). -->
  <canvas ref="canvasRef" class="undistort-canvas" aria-hidden="true" />
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { UndistortRenderer, type UndistortParams } from '@/utils/undistortRenderer'

const props = defineProps<{
  /** Returns the `<img>` currently showing the live stream, or null (then nothing is drawn). */
  getSource: () => HTMLImageElement | null,
  params: UndistortParams,
}>()

const emit = defineEmits<{
  /** Rendering failed (no WebGL, tainted image, ...); the parent should turn the view off. */
  error: [message: string],
}>()

const canvasRef = ref<HTMLCanvasElement | null>(null)
let renderer: UndistortRenderer | null = null
let frameId: number | null = null

const stop = () => {
  if (frameId !== null) cancelAnimationFrame(frameId)
  frameId = null
}

const tick = () => {
  frameId = requestAnimationFrame(tick)
  const source = props.getSource()
  const canvas = canvasRef.value
  if (!renderer || !canvas) return
  if (!source || source.naturalWidth === 0 || source.naturalHeight === 0) {
    canvas.style.visibility = 'hidden'
    return
  }
  try {
    // An MJPEG <img> exposes no "new frame" event, so upload once per animation frame (cheap at 1280x960).
    renderer.render(source, source.naturalWidth, source.naturalHeight, props.params)
    canvas.style.visibility = 'visible'
  } catch (error) {
    stop()
    console.warn('Undistorted view failed', error)
    emit('error', error instanceof DOMException && error.name === 'SecurityError'
      ? 'The camera stream cannot be read by the browser (cross-origin).'
      : 'Undistorted view failed to render.')
  }
}

onMounted(() => {
  try {
    renderer = new UndistortRenderer(canvasRef.value!)
  } catch (error) {
    console.warn(error)
    emit('error', 'WebGL is not available in this browser.')
    return
  }
  frameId = requestAnimationFrame(tick)
})

onUnmounted(() => {
  stop()
  renderer?.dispose()
  renderer = null
})
</script>

<style scoped>
.undistort-canvas {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  z-index: 0;
  pointer-events: none;
  visibility: hidden;
}
</style>
