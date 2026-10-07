<template>
  <!--
    Drawn in output image pixel units (viewBox = image size); the SVG box has the image's aspect ratio, so this is a
    uniform scale. Strokes are non-scaling, so they keep their screen width at any preview size.
  -->
  <svg class="lens-guides" xmlns="http://www.w3.org/2000/svg" :viewBox="`0 0 ${width} ${height}`"
    preserveAspectRatio="none" aria-hidden="true">
    <g v-if="gridPaths.length" class="lens-guides__grid">
      <polyline v-for="(points, i) in gridPaths" :key="`go${i}`" class="lens-guides__outline" :points="points" />
      <polyline v-for="(points, i) in gridPaths" :key="`gs${i}`" class="lens-guides__stroke" :points="points" />
    </g>
    <g v-if="ringPaths.length" class="lens-guides__rings">
      <polyline v-for="ring in ringPaths" :key="`ro${ring.angle}`" class="lens-guides__outline" :points="ring.points" />
      <polyline v-for="ring in ringPaths" :key="`rs${ring.angle}`" class="lens-guides__stroke" :points="ring.points" />
      <text v-for="ring in ringPaths" :key="`rl${ring.angle}`" class="lens-guides__label" :x="ring.label.x"
        :y="ring.label.y" :font-size="fontSize" :stroke-width="fontSize / 6" :transform="`rotate(${-rotation} ${ring.label.x} ${ring.label.y})`"
        text-anchor="start" dominant-baseline="auto">{{ ring.angle }}°</text>
    </g>
    <g v-if="center" class="lens-guides__crosshair">
      <line class="lens-guides__outline" :x1="0" :y1="center.y" :x2="width" :y2="center.y" />
      <line class="lens-guides__outline" :x1="center.x" :y1="0" :x2="center.x" :y2="height" />
      <line class="lens-guides__stroke lens-guides__stroke--dashed" :x1="0" :y1="center.y" :x2="width"
        :y2="center.y" />
      <line class="lens-guides__stroke lens-guides__stroke--dashed" :x1="center.x" :y1="0" :x2="center.x"
        :y2="height" />
      <circle class="lens-guides__outline" :cx="center.x" :cy="center.y" :r="markerRadius" />
      <circle class="lens-guides__stroke" :cx="center.x" :cy="center.y" :r="markerRadius" />
    </g>
  </svg>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { LensGuideGeometry, Point } from '@/utils/lensModel'

const props = defineProps<{
  geometry: LensGuideGeometry,
  /** Preview rotation (degrees, clockwise); labels are counter-rotated to stay upright. */
  rotation: number,
}>()

const width = computed(() => props.geometry.imageWidth)
const height = computed(() => props.geometry.imageHeight)
const fontSize = computed(() => Math.max(8, Math.min(width.value, height.value) / 32))
const markerRadius = computed(() => Math.min(width.value, height.value) / 60)

const toPx = (p: Point) => ({ x: p.x * width.value, y: p.y * height.value })
const toPoints = (points: Point[]) => points
  .map(p => `${(p.x * width.value).toFixed(2)},${(p.y * height.value).toFixed(2)}`).join(' ')

const center = computed(() => props.geometry.center ? toPx(props.geometry.center) : null)
const ringPaths = computed(() => props.geometry.rings.map(ring => ({
  angle: ring.angleDeg,
  points: toPoints(ring.points),
  label: toPx(ring.labelAt),
})))
const gridPaths = computed(() => props.geometry.grid.map(toPoints))
</script>

<style scoped>
.lens-guides {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  overflow: hidden;
  /* Above the overlay image (same z-index, later in the DOM), below the alignment lines (z-index 1). */
  z-index: 0;
  pointer-events: none;
}

.lens-guides polyline,
.lens-guides line,
.lens-guides circle {
  vector-effect: non-scaling-stroke;
  fill: none;
  stroke-linejoin: round;
}

.lens-guides__outline {
  stroke: rgba(0, 0, 0, 0.6);
  stroke-width: 3px;
}

.lens-guides__stroke {
  stroke: #00e5ff;
  stroke-width: 1.25px;
}

.lens-guides__stroke--dashed {
  stroke-dasharray: 8 5;
}

.lens-guides__grid .lens-guides__stroke {
  stroke: rgba(0, 229, 255, 0.65);
}

.lens-guides__label {
  fill: #00e5ff;
  stroke: rgba(0, 0, 0, 0.75);
  paint-order: stroke;
  font-family: Roboto, sans-serif;
  font-weight: 600;
  user-select: none;
}
</style>
