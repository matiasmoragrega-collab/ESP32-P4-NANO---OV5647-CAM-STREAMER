<template>
  <div class="pa-4">
    <div class="text-overline">Lens centre</div>
    <div class="text-caption text-medium-emphasis mb-2">
      Every mode reads its window from the sensor centred on the lens's optical centre (lossless, no resampling).
      Calibration source:
      <v-chip size="x-small" :color="applied.source === 'calibrated' ? 'success' : undefined" variant="tonal"
        class="ml-1">{{ applied.source }}</v-chip>
    </div>

    <!-- Sensor map -->
    <div class="d-flex align-center mb-1">
      <div class="text-subtitle-2">Sensor map</div>
      <v-spacer />
      <v-btn-toggle v-model="mapZoomed" density="compact" variant="outlined" divided mandatory>
        <v-btn :value="false" size="small" aria-label="Show the whole sensor">Full</v-btn>
        <v-btn :value="true" size="small" aria-label="Zoom in around the lens centre">×{{ ZOOM_FACTOR }}</v-btn>
      </v-btn-toggle>
    </div>
    <svg ref="mapRef" class="sensor-map" :style="{ aspectRatio: `${sensor.arrayWidth} / ${sensor.arrayHeight}` }" :viewBox="mapViewBox.join(' ')" xmlns="http://www.w3.org/2000/svg"
      role="img" :aria-label="`Sensor map; pending lens centre ${centerX}, ${centerY}`" @pointerdown="onMapPointerDown"
      @pointermove="onMapPointerMove" @pointerup="endMapDrag" @pointercancel="endMapDrag"
      @lostpointercapture="endMapDrag">
      <rect class="sensor-map__array" x="0" y="0" :width="sensor.arrayWidth" :height="sensor.arrayHeight" />
      <rect class="sensor-map__active" v-bind="sensor.activeArea" />
      <g v-for="mode in modes" :key="mode.key">
        <rect class="sensor-map__window" :class="windowClass(mode)" :x="mode.window.x" :y="mode.window.y"
          :width="mode.window.width" :height="mode.window.height" />
      </g>
      <!-- Achievable-centre range and predicted window of the highlighted mode(s). -->
      <g v-for="mode in highlightedModes" :key="`hl-${mode.key}`">
        <rect class="sensor-map__range" :class="windowClass(mode)" :x="mode.window.centerRange.xMin"
          :y="mode.window.centerRange.yMin" :width="mode.window.centerRange.xMax - mode.window.centerRange.xMin"
          :height="mode.window.centerRange.yMax - mode.window.centerRange.yMin" />
        <rect v-if="dirty" class="sensor-map__predicted" :class="windowClass(mode)" v-bind="mode.predicted" />
      </g>
      <text v-for="label in mapLabels" :key="`label-${label.mode.key}`" class="sensor-map__label"
        :class="windowClass(label.mode)" :x="label.x" :y="label.y" :text-anchor="label.anchor" :font-size="mapFont">
        {{ label.mode.label }}
      </text>
      <!-- Applied lens centre (grey) and pending lens centre (pink). -->
      <g v-if="dirty" class="sensor-map__applied-centre">
        <line :x1="applied.center.x - markerSize" :y1="applied.center.y" :x2="applied.center.x + markerSize"
          :y2="applied.center.y" />
        <line :x1="applied.center.x" :y1="applied.center.y - markerSize" :x2="applied.center.x"
          :y2="applied.center.y + markerSize" />
      </g>
      <g class="sensor-map__centre">
        <line :x1="mapViewBox[0]" :y1="centerY" :x2="mapViewBox[0] + mapViewBox[2]" :y2="centerY"
          class="sensor-map__centre-axis" />
        <line :x1="centerX" :y1="mapViewBox[1]" :x2="centerX" :y2="mapViewBox[1] + mapViewBox[3]"
          class="sensor-map__centre-axis" />
        <circle :cx="centerX" :cy="centerY" :r="markerSize" />
      </g>
    </svg>
    <div v-if="mirrorNote" class="text-caption text-warning mb-1">
      Map is in sensor coordinates; the camera image is {{ mirrorNote }} relative to it.
    </div>
    <div class="text-caption text-medium-emphasis mb-3">
      Click or drag to set the lens centre (0.5 px steps). Boxes: each mode's readout window
      (<span class="sensor-map-key sensor-map-key--current">current</span>); the shaded box is the range of lens
      centres the highlighted mode can be centred on exactly. Hover a mode in the list below to highlight it.
      <template v-if="dirty"> Dashed: predicted window after Apply (approximate).</template>
    </div>

    <SliderField v-model="centerX" :min="activeMinX" :max="activeMaxX" :step="0.5" label="Centre X" suffix="px"
      label-width="5.5em" :disabled="!!busy" class="mb-1" />
    <SliderField v-model="centerY" :min="activeMinY" :max="activeMaxY" :step="0.5" label="Centre Y" suffix="px"
      label-width="5.5em" :disabled="!!busy" class="mb-2" />
    <div class="text-caption text-medium-emphasis mb-2">
      Sensor pixel coordinates (active area x {{ activeMinX }}…{{ activeMaxX }}, y {{ activeMinY }}…{{ activeMaxY }}).
    </div>

    <v-switch v-model="distortionEnabled" :disabled="!!busy" color="primary" label="Distortion model" hide-details />
    <div class="text-caption text-medium-emphasis mb-2">
      OpenCV k1, k2, p1, p2, k3 with fx, fy in sensor px about the lens centre. Used only for the guides and the
      display-only undistorted view, never applied to the stream.
    </div>
    <div v-if="distortionEnabled" class="distortion-grid mb-2">
      <v-text-field v-for="key in DISTORTION_KEYS" :key="key" v-model="distortionDraft[key]" :label="key"
        :disabled="!!busy" :error="fieldErrors[key]" inputmode="decimal" density="compact" variant="outlined"
        hide-details spellcheck="false" autocomplete="off" />
    </div>
    <div v-if="distortionEnabled && draftDistortionError" class="text-caption text-error mb-2">
      {{ draftDistortionError }}
    </div>
    <div v-if="fovText" class="text-caption text-medium-emphasis mb-2">{{ fovText }}</div>

    <div class="text-subtitle-2 mt-2">Residual centre error per mode</div>
    <div class="text-caption text-medium-emphasis mb-1">
      Offset of the mode's readout centre from the lens centre (sensor px){{ dirty ? '; pending values are predicted' : ', as reported by the camera' }}.
      "Clamped" modes cannot move their window far enough.
    </div>
    <v-list density="compact" class="residual-list mb-3 py-0" bg-color="transparent">
      <v-list-item v-for="mode in modes" :key="mode.key" class="px-1" min-height="28"
        :class="{ 'residual-list__item--current': mode.current }" @mouseenter="hoveredKey = mode.key"
        @mouseleave="hoveredKey = null" @focusin="hoveredKey = mode.key" @focusout="hoveredKey = null" tabindex="0">
        <div class="text-caption">
          <span class="font-weight-bold">{{ mode.label }}</span><span v-if="mode.current"> (current)</span>:
          Δx {{ formatSigned(mode.residual.x) }}, Δy {{ formatSigned(mode.residual.y) }} px
          <span v-if="mode.clamped" class="text-warning font-weight-bold">— clamped</span>
        </div>
      </v-list-item>
    </v-list>

    <v-alert v-if="error" type="error" variant="tonal" density="compact" class="mb-2" closable
      @click:close="error = ''">{{ error }}</v-alert>
    <v-alert v-if="info" type="info" variant="tonal" density="compact" class="mb-2" closable
      @click:close="info = ''">{{ info }}</v-alert>

    <div class="d-flex ga-2 mb-2">
      <v-btn variant="tonal" color="primary" class="flex-grow-1" :loading="busy === 'apply'"
        :disabled="!!busy || !dirty || !!draftDistortionError" @click="apply">Apply</v-btn>
      <v-btn variant="tonal" :disabled="!!busy || !dirty" title="Discard unapplied changes"
        @click="loadFromCamera(); error = ''">Revert</v-btn>
    </div>
    <div class="d-flex flex-wrap ga-2 mb-2">
      <input ref="fileInput" type="file" accept=".json,application/json" class="d-none" @change="onFileChange" />
      <v-btn variant="tonal" size="small" class="flex-grow-1" :prepend-icon="mdiFileImportOutline" :disabled="!!busy"
        @click="fileInput?.click()">Import</v-btn>
      <v-btn variant="tonal" size="small" class="flex-grow-1" :prepend-icon="mdiFileExportOutline" :disabled="!!busy"
        title="Download the applied calibration as JSON" @click="exportCalibration">Export</v-btn>
      <v-btn variant="tonal" size="small" color="error" class="flex-grow-1" :prepend-icon="mdiRestore"
        :loading="busy === 'reset'" :disabled="!!busy" @click="resetDialog = true">Reset</v-btn>
    </div>
    <div class="text-caption text-medium-emphasis">
      {{ dirty ? 'Pending changes (previewed by the lens guides). Apply restarts the stream; closing the panel discards them.' : 'No pending changes.' }}
      Import loads a calibration.json from the calibration tool for review; Export saves the applied calibration.
    </div>

    <v-divider class="my-4" />

    <div class="text-subtitle-2">Lens guides</div>
    <div class="text-caption text-medium-emphasis mb-1">
      Drawn over the preview, centred on the lens centre (kept in this browser).
    </div>
    <v-switch v-model="crosshair" color="primary" label="Crosshair at lens centre" hide-details density="compact" />
    <v-switch v-model="rings" :disabled="!hasDistortion" color="primary"
      :label="`Field-angle rings (${RING_ANGLES_DEG[0]}°–${RING_ANGLES_DEG[RING_ANGLES_DEG.length - 1]}°)`"
      hide-details density="compact" />
    <v-switch v-model="grid" :disabled="!hasDistortion" color="primary"
      :label="`Distorted grid (${GRID_STEP_DEG}° spacing)`" hide-details density="compact" />
    <div v-if="!hasDistortion" class="text-caption text-medium-emphasis mb-2">
      Rings and grid need a distortion model (fx, fy, …); without one only the crosshair is shown.
    </div>

    <v-switch v-model="undistort" :disabled="!hasDistortion || !webglAvailable" color="primary"
      label="Undistorted view" hide-details density="compact" class="mt-2" />
    <div class="text-caption text-medium-emphasis">
      <strong>Display only</strong> — the camera stream and RAW downloads are unchanged. While on, the preview is
      remapped in the browser (WebGL) and <em>Download preview</em> saves that undistorted picture.
      <template v-if="!webglAvailable"> Not available: this browser has no WebGL.</template>
      <template v-else-if="!hasDistortion"> Needs a distortion model.</template>
    </div>
    <div v-if="undistortError" class="text-caption text-error">{{ undistortError }}</div>
  </div>

  <v-dialog v-model="resetDialog" max-width="420">
    <v-card>
      <v-card-title :prepend-icon="mdiRestore">Reset lens calibration?</v-card-title>
      <v-card-text>
        This restores the camera's default lens centre and removes the distortion model. The stream restarts.
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="tonal" @click="resetDialog = false">Cancel</v-btn>
        <v-btn variant="tonal" color="error" @click="confirmReset">Reset</v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>
</template>

<script setup lang="ts">
import { computed, reactive, ref } from 'vue'
import { mdiFileExportOutline, mdiFileImportOutline, mdiRestore } from '@mdi/js'
import type { Camera, LensCalibration, LensDistortion, SensorInfo, SensorWindow } from '@/camera'
import {
  DISTORTION_KEYS, GRID_STEP_DEG, RING_ANGLES_DEG, fieldOfViewDeg, isValidDistortion, snapHalf,
  type DistortionKey, type LensModel,
} from '@/utils/lensModel'
import { parseCalibrationFile, validateCenter, type LensCalibrationBody } from '@/utils/lensCalibrationFile'
import { useMainStore } from '@/store/mainstore'
import SliderField from '@/components/SliderField.vue'

/** Zoomed map: this many times the full view. */
const ZOOM_FACTOR = 8

const props = defineProps<{
  camera: Camera & { sensor: SensorInfo, lensCalibration: LensCalibration, window: SensorWindow },
  /** Whether the settings panel is open (opening / closing discards pending edits). */
  open: boolean,
  webglAvailable: boolean,
  undistortError: string,
}>()

const emit = defineEmits<{
  /** A request that restarts the camera stream is about to be sent. */
  requestStart: [],
  /** The request finished; `ok` false = failed (message is shown inline as well). */
  requestEnd: [ok: boolean, message: string],
}>()

/** Pending lens model (null while there are no pending edits); the parent previews it with the guides. */
const draft = defineModel<LensModel | null>('draft', { default: null })
const crosshair = defineModel<boolean>('crosshair', { required: true })
const rings = defineModel<boolean>('rings', { required: true })
const grid = defineModel<boolean>('grid', { required: true })
const undistort = defineModel<boolean>('undistort', { required: true })

const sensor = computed(() => props.camera.sensor)
const applied = computed(() => props.camera.lensCalibration)
const activeMinX = computed(() => sensor.value.activeArea.x)
const activeMaxX = computed(() => sensor.value.activeArea.x + sensor.value.activeArea.width)
const activeMinY = computed(() => sensor.value.activeArea.y)
const activeMaxY = computed(() => sensor.value.activeArea.y + sensor.value.activeArea.height)

// ---- Staged (pending) values -------------------------------------------------------------------------------------

type DistortionDraft = Record<DistortionKey, string>

const centerX = ref<number>(applied.value.center.x)
const centerY = ref<number>(applied.value.center.y)
const distortionEnabled = ref<boolean>(false)
const distortionDraft = reactive<DistortionDraft>(Object.fromEntries(DISTORTION_KEYS.map(key => [key, ''])) as DistortionDraft)
const busy = ref<'' | 'apply' | 'reset'>('')
const error = ref<string>('')
const info = ref<string>('')
const resetDialog = ref<boolean>(false)
const fileInput = ref<HTMLInputElement | null>(null)

/** Neutral starting point when no model exists yet: no distortion, focal length ~ a 53° horizontal field of view. */
const defaultDistortion = (): LensDistortion => ({
  fx: sensor.value.activeArea.width, fy: sensor.value.activeArea.width, k1: 0, k2: 0, p1: 0, p2: 0, k3: 0,
})

const setDistortionDraft = (d: LensDistortion) => {
  for (const key of DISTORTION_KEYS) distortionDraft[key] = String(d[key])
}

const parseField = (text: string) => {
  const trimmed = text.trim().replace(',', '.')
  return trimmed === '' ? NaN : Number(trimmed)
}

const fieldErrors = computed(() => Object.fromEntries(DISTORTION_KEYS.map(key => {
  const value = parseField(distortionDraft[key])
  return [key, !Number.isFinite(value) || ((key === 'fx' || key === 'fy') && value <= 0)]
})) as Record<DistortionKey, boolean>)

const draftDistortion = computed<LensDistortion | null>(() => {
  if (!distortionEnabled.value) return null
  const d = Object.fromEntries(DISTORTION_KEYS.map(key => [key, parseField(distortionDraft[key])])) as LensDistortion
  return isValidDistortion(d) ? d : null
})

const draftDistortionError = computed(() => {
  if (!distortionEnabled.value || draftDistortion.value) return ''
  return 'Enter numbers for all parameters (fx and fy > 0).'
})

const loadFromCamera = () => {
  const cal = applied.value
  centerX.value = cal.center.x
  centerY.value = cal.center.y
  distortionEnabled.value = !!cal.distortion
  if (cal.distortion) {
    setDistortionDraft(cal.distortion)
  } else if (!DISTORTION_KEYS.every(key => Number.isFinite(parseField(distortionDraft[key])))) {
    setDistortionDraft(defaultDistortion())
  }
}
loadFromCamera()

const sameDistortion = (a: LensDistortion | null, b: LensDistortion | null) =>
  a === b || (!!a && !!b && DISTORTION_KEYS.every(key => a[key] === b[key]))

/** Whether the staged values equal the given calibration. */
const matchesDraft = (cal: LensCalibration) => {
  if (centerX.value !== cal.center.x || centerY.value !== cal.center.y) return false
  if (distortionEnabled.value !== !!cal.distortion) return false
  return !distortionEnabled.value || sameDistortion(draftDistortion.value, cal.distortion)
}

const dirty = computed(() => !matchesDraft(applied.value))

const hasDistortion = computed(() => !!(dirty.value ? draftDistortion.value : applied.value.distortion))

// Publish the pending model for the guide preview.
watchEffect(() => {
  draft.value = dirty.value
    ? { center: { x: centerX.value, y: centerY.value }, distortion: draftDistortion.value }
    : null
})

watch(() => props.open, () => {
  loadFromCamera()
  error.value = ''
  info.value = ''
})

// Pick up new values from the camera (e.g. after Apply / Reset, or changed elsewhere) unless the user is editing.
// (Camera info is re-polled every few seconds, so compare by value.)
watch(applied, (cal, old) => {
  if (!old || JSON.stringify(cal) === JSON.stringify(old)) return
  if (matchesDraft(old)) loadFromCamera()
})

watch(hasDistortion, (has) => {
  if (!has) undistort.value = false
})

// ---- Modes / map ---------------------------------------------------------------------------------------------------

type ModeEntry = {
  key: string;
  label: string;
  current: boolean;
  window: SensorWindow;
  /** Predicted readout rectangle for the pending centre (simple clamp model). */
  predicted: { x: number, y: number, width: number, height: number };
  residual: { x: number, y: number };
  clamped: boolean;
}

const hoveredKey = ref<string | null>(null)

const clamp = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value))

const modeLabel = (w: SensorWindow) => {
  const label = `${Math.round(w.width / w.scale)}x${Math.round(w.height / w.scale)}`
  return w.scale !== 1 ? `${label} bin${w.scale}` : label
}

const modes = computed<ModeEntry[]>(() => {
  const cam = props.camera
  const pending = { x: centerX.value, y: centerY.value }
  const entries: ModeEntry[] = []
  const formats = cam.imageFormats.filter(format => !!format.window)
  const list = formats.length > 0
    ? formats.map(format => ({ key: String(format.id), window: format.window!, current: format.id === cam.currentImageFormat }))
    : [{ key: 'current', window: cam.window, current: true }]
  for (const { key, window: w, current } of list) {
    // The current mode's live window is the authoritative one.
    const win = current ? cam.window : w
    const r = win.centerRange
    const target = { x: clamp(pending.x, r.xMin, r.xMax), y: clamp(pending.y, r.yMin, r.yMax) }
    const tolerance = 0.01
    const reference = dirty.value ? pending : applied.value.center
    entries.push({
      key,
      label: modeLabel(win),
      current,
      window: win,
      predicted: { x: target.x - win.width / 2, y: target.y - win.height / 2, width: win.width, height: win.height },
      residual: dirty.value ? { x: target.x - pending.x, y: target.y - pending.y } : win.centerError,
      clamped: reference.x < r.xMin - tolerance || reference.x > r.xMax + tolerance
        || reference.y < r.yMin - tolerance || reference.y > r.yMax + tolerance,
    })
  }
  return entries
})

const highlightedModes = computed(() => modes.value.filter(mode => mode.current || mode.key === hoveredKey.value))

const windowClass = (mode: ModeEntry) => ({
  'sensor-map--current': mode.current,
  'sensor-map--hovered': mode.key === hoveredKey.value && !mode.current,
})

const formatSigned = (value: number) => {
  const rounded = Math.round(value * 10) / 10
  if (rounded === 0) return '0.0'
  return `${rounded > 0 ? '+' : '−'}${Math.abs(rounded).toFixed(1)}`
}

const mapZoomed = ref<boolean>(false)
const zoomCenter = ref<{ x: number, y: number }>({ x: centerX.value, y: centerY.value })

const mapViewBox = computed<[number, number, number, number]>(() => {
  const { arrayWidth, arrayHeight } = sensor.value
  if (!mapZoomed.value) return [0, 0, arrayWidth, arrayHeight]
  const width = arrayWidth / ZOOM_FACTOR
  const height = arrayHeight / ZOOM_FACTOR
  const x = clamp(zoomCenter.value.x - width / 2, 0, arrayWidth - width)
  const y = clamp(zoomCenter.value.y - height / 2, 0, arrayHeight - height)
  return [x, y, width, height]
})
const mapFont = computed(() => mapViewBox.value[2] / 30)
const markerSize = computed(() => mapViewBox.value[2] / 70)

/** Mode labels, each in a corner of its window chosen so labels don't overlap (greedy). */
const mapLabels = computed(() => {
  const font = mapFont.value
  const pad = font * 0.3
  const placed: { x0: number, y0: number, x1: number, y1: number }[] = []
  return modes.value.map(mode => {
    const w = mode.window
    const textWidth = mode.label.length * font * 0.58
    const corners = [
      { x: w.x + pad, y: w.y + font * 1.05, anchor: 'start' },
      { x: w.x + w.width - pad, y: w.y + font * 1.05, anchor: 'end' },
      { x: w.x + pad, y: w.y + w.height - pad, anchor: 'start' },
      { x: w.x + w.width - pad, y: w.y + w.height - pad, anchor: 'end' },
    ]
    const boxOf = (c: typeof corners[number]) => {
      const x0 = c.anchor === 'start' ? c.x : c.x - textWidth
      return { x0, y0: c.y - font, x1: x0 + textWidth, y1: c.y + font * 0.2 }
    }
    const free = corners.find(c => {
      const b = boxOf(c)
      return placed.every(p => b.x1 < p.x0 || b.x0 > p.x1 || b.y1 < p.y0 || b.y0 > p.y1)
    }) ?? corners[0]
    placed.push(boxOf(free))
    return { mode, ...free }
  })
})

const mapRef = ref<SVGSVGElement | null>(null)
let mapPointerId: number | null = null

watch(mapZoomed, (zoomed) => {
  if (zoomed) zoomCenter.value = { x: centerX.value, y: centerY.value }
})

// Keep the pending centre in view while zoomed (but don't move the map under an active drag).
watch([centerX, centerY], ([x, y]) => {
  if (!mapZoomed.value || mapPointerId !== null) return
  const [vx, vy, vw, vh] = mapViewBox.value
  if (x < vx + vw * 0.1 || x > vx + vw * 0.9 || y < vy + vh * 0.1 || y > vy + vh * 0.9) zoomCenter.value = { x, y }
})

const setCenterFromPointer = (event: PointerEvent) => {
  const svg = mapRef.value
  const matrix = svg?.getScreenCTM()
  if (!svg || !matrix) return
  const point = new DOMPoint(event.clientX, event.clientY).matrixTransform(matrix.inverse())
  centerX.value = clamp(snapHalf(point.x), activeMinX.value, activeMaxX.value)
  centerY.value = clamp(snapHalf(point.y), activeMinY.value, activeMaxY.value)
}

const onMapPointerDown = (event: PointerEvent) => {
  if (event.button !== 0 || busy.value || mapPointerId !== null) return
  event.preventDefault()
  mapPointerId = event.pointerId
  try {
    mapRef.value?.setPointerCapture(event.pointerId)
  } catch {
    // Dragging still works inside the map without capture.
  }
  setCenterFromPointer(event)
}

const onMapPointerMove = (event: PointerEvent) => {
  if (event.pointerId === mapPointerId) setCenterFromPointer(event)
}

const endMapDrag = (event: PointerEvent) => {
  if (event.pointerId === mapPointerId) mapPointerId = null
}

const mirrorNote = computed(() => [
  props.camera.window.mirror ? 'mirrored horizontally' : '',
  props.camera.window.flip ? 'flipped vertically' : '',
].filter(Boolean).join(' and '))

const fovText = computed(() => {
  const d = dirty.value ? draftDistortion.value : applied.value.distortion
  if (!d) return ''
  const w = props.camera.window
  return `Current mode ≈ ${fieldOfViewDeg(w.width, d.fx).toFixed(1)}° × ${fieldOfViewDeg(w.height, d.fy).toFixed(1)}° `
    + '(pinhole estimate, horizontal × vertical).'
})

// ---- Requests ------------------------------------------------------------------------------------------------------

const mainStore = useMainStore()

const post = async (body: object) => {
  const response = await fetch('/api/set_lens_calibration', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (!response.ok) {
    const text = (await response.text().catch(() => '')).trim()
    throw new Error(text || `Request failed (HTTP ${response.status}).`)
  }
}

const send = async (kind: 'apply' | 'reset', body: object, successMessage: string) => {
  busy.value = kind
  error.value = ''
  info.value = ''
  emit('requestStart')
  let ok = false
  let message = successMessage
  try {
    await post(body)
    ok = true
  } catch (e) {
    message = e instanceof Error ? e.message : String(e)
    error.value = message
  }
  try {
    await mainStore.updateCameraStatus()
  } finally {
    busy.value = ''
    if (ok) loadFromCamera()
    emit('requestEnd', ok, message)
  }
}

const apply = async () => {
  const center = { x: centerX.value, y: centerY.value }
  try {
    validateCenter(center, sensor.value.activeArea)
  } catch (e) {
    error.value = (e as Error).message
    return
  }
  if (distortionEnabled.value && !draftDistortion.value) {
    error.value = draftDistortionError.value
    return
  }
  const body: LensCalibrationBody = { index: props.camera.index, center, distortion: draftDistortion.value }
  await send('apply', body, 'Lens calibration applied')
}

const confirmReset = async () => {
  resetDialog.value = false
  await send('reset', { index: props.camera.index, reset: true }, 'Lens calibration reset to default')
}

const onFileChange = async (event: Event) => {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (!file) return
  error.value = ''
  info.value = ''
  try {
    if (file.size > 1024 * 1024) throw new Error('The file is too large for a calibration file.')
    const parsed = parseCalibrationFile(await file.text(), sensor.value.activeArea)
    centerX.value = parsed.center.x
    centerY.value = parsed.center.y
    distortionEnabled.value = !!parsed.distortion
    if (parsed.distortion) setDistortionDraft(parsed.distortion)
    const otherCamera = parsed.index !== null && String(parsed.index) !== String(props.camera.index)
      ? ` Note: the file was made for camera ${parsed.index}.` : ''
    info.value = `Imported ${file.name}. Review the values, then Apply.${otherCamera}`
  } catch (e) {
    error.value = `Import failed: ${e instanceof Error ? e.message : String(e)}`
  }
}

const exportCalibration = () => {
  const cal = applied.value
  const body: LensCalibrationBody & { source: string } = {
    index: props.camera.index,
    center: { ...cal.center },
    distortion: cal.distortion ? { ...cal.distortion } : null,
    source: cal.source,
  }
  const blob = new Blob([JSON.stringify(body, null, 2) + '\n'], { type: 'application/json' })
  const url = URL.createObjectURL(blob)
  const link = document.createElement('a')
  link.href = url
  link.download = `camera_${props.camera.index}_lens_calibration.json`
  document.body.appendChild(link)
  link.click()
  document.body.removeChild(link)
  setTimeout(() => URL.revokeObjectURL(url), 10000)
}
</script>

<style scoped>
.sensor-map {
  display: block;
  width: 100%;
  height: auto;
  background: #111;
  border-radius: 4px;
  cursor: crosshair;
  touch-action: none;
  user-select: none;
}

.sensor-map rect,
.sensor-map line,
.sensor-map circle {
  vector-effect: non-scaling-stroke;
}

.sensor-map__array {
  fill: #1c1c1c;
  stroke: #555;
  stroke-width: 1px;
}

.sensor-map__active {
  fill: #2a2a2a;
  stroke: #888;
  stroke-width: 1px;
  stroke-dasharray: 3 3;
}

.sensor-map__window {
  fill: none;
  stroke: rgba(255, 255, 255, 0.35);
  stroke-width: 1px;
}

.sensor-map__label {
  fill: rgba(255, 255, 255, 0.55);
  font-family: Roboto, sans-serif;
  pointer-events: none;
}

.sensor-map__range {
  fill: rgba(255, 255, 255, 0.12);
  stroke: rgba(255, 255, 255, 0.6);
  stroke-width: 1px;
}

.sensor-map__predicted {
  fill: none;
  stroke: rgba(255, 255, 255, 0.6);
  stroke-width: 1px;
  stroke-dasharray: 5 4;
}

.sensor-map__window.sensor-map--current,
.sensor-map__predicted.sensor-map--current {
  stroke: #00e5ff;
  stroke-width: 2px;
}

.sensor-map__range.sensor-map--current {
  fill: rgba(0, 229, 255, 0.2);
  stroke: #00e5ff;
}

.sensor-map__label.sensor-map--current {
  fill: #00e5ff;
  font-weight: 600;
}

.sensor-map__window.sensor-map--hovered,
.sensor-map__predicted.sensor-map--hovered,
.sensor-map__range.sensor-map--hovered {
  stroke: #ffeb3b;
  stroke-width: 2px;
}

.sensor-map__range.sensor-map--hovered {
  fill: rgba(255, 235, 59, 0.2);
}

.sensor-map__label.sensor-map--hovered {
  fill: #ffeb3b;
  font-weight: 600;
}

.sensor-map__applied-centre line {
  stroke: rgba(255, 255, 255, 0.7);
  stroke-width: 1.5px;
}

.sensor-map__centre circle {
  fill: none;
  stroke: #ff4081;
  stroke-width: 2px;
}

.sensor-map__centre-axis {
  stroke: rgba(255, 64, 129, 0.7);
  stroke-width: 1px;
  stroke-dasharray: 4 3;
}

.sensor-map-key--current {
  color: #00acc1;
  font-weight: 600;
}

.distortion-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px;
}

.residual-list :deep(.v-list-item__content) {
  overflow: visible;
}

.residual-list__item--current {
  background: rgba(0, 229, 255, 0.08);
}
</style>
