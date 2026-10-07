<template>
  <v-card class="mb-3">
    <div ref="viewportRef" class="camera-preview-viewport" :style="previewViewportStyle">
      <v-img :src="imgSrc" :aspect-ratio="cameraAspectRatio" cover class="camera-preview-image"
        :style="previewImageStyle" :id="`cam-${props.camNum}-v-img`" crossorigin="anonymous" @error="onImageError"
        @load="previewImageFailed = false">
        <div v-if="overlayUrl && overlayVisible" class="camera-overlay-layer">
          <img :src="overlayUrl" class="camera-overlay-image" :style="overlayImageStyle" alt="" aria-hidden="true"
            draggable="false" />
        </div>
        <AlignmentOverlay v-if="alignmentVisible" v-model:selected-id="selectedLineId" :lines="alignmentLines"
          :drawing="alignmentDrawMode" :rotation="previewRotation" @add="addAlignmentLine"
          @delete-selected="deleteSelectedLine" />
        <OverlayMoveSurface v-if="overlayMoveMode" :rotation="previewRotation" @drag-start="onOverlayDragStart"
          @drag="onOverlayDrag" @zoom="zoomOverlay" />
        <template #placeholder>
          <div class="d-flex align-center justify-center fill-height">
            <v-progress-circular color="grey-lighten-4" indeterminate />
          </div>
        </template>
        <template #error>
          <div class="d-flex align-center justify-center fill-height">
            <div style="height: 100px;" class="d-flex flex-column">
              <div class="my-auto" style="font-size: larger; font-weight: bold;">
                Something went wrong
              </div>
              <div class="mx-auto" style="font-size: smaller; opacity: 70%;">
                Retrying in 3 seconds...
              </div>
            </div>
          </div>
        </template>
      </v-img>
    </div>
    <div class="d-flex flex-wrap justify-space-between align-center ga-2 my-2 mx-3">
      <div>
        <div style="font-weight: bold; font-size: larger;">
          {{ cameraTitle }}
        </div>
        <div style="font-size: smaller; opacity: 70%;">
          {{ camera.currentImageFormatDescription }} @ {{ camera.currentFrameRate }} fps
          <span v-if="camera.currentQuality">
            (Quality: {{ camera.currentQuality }})
          </span>
        </div>
      </div>
      <div class="d-flex flex-wrap ga-1 ml-auto">
        <v-btn :variant="alignmentDrawMode ? 'flat' : 'tonal'" :color="alignmentDrawMode ? 'primary' : undefined"
          @click="alignmentDrawMode = !alignmentDrawMode" :icon="mdiVectorLine" :disabled="!alignmentVisible"
          :aria-pressed="alignmentDrawMode" :aria-label="drawModeLabel" :title="drawModeLabel" />
        <v-btn variant="tonal" @click="deleteSelectedLine" :icon="mdiDeleteOutline" :disabled="!selectedLineId"
          aria-label="Delete selected line" title="Delete selected line (Delete key)" data-alignment-keep-selection />
        <v-btn variant="tonal" @click="alignmentVisible = !alignmentVisible" :icon="alignmentVisible ? mdiEye : mdiEyeOff"
          :aria-pressed="!alignmentVisible" :aria-label="visibilityLabel" :title="visibilityLabel" />
        <v-btn variant="tonal" @click="clearLinesDialog = true" :icon="mdiDeleteSweepOutline"
          :disabled="alignmentLines.length === 0" aria-label="Clear all alignment lines"
          title="Clear all alignment lines" />
        <v-divider vertical class="mx-1" />
        <v-btn v-if="overlayHasImage" variant="tonal" @click="overlayVisible = !overlayVisible"
          :icon="overlayVisible ? mdiImageOutline : mdiImageOffOutline" :aria-pressed="!overlayVisible"
          :aria-label="overlayVisibilityLabel" :title="overlayVisibilityLabel" />
        <v-btn v-if="overlayHasImage && overlayVisible" :variant="overlayMoveMode ? 'flat' : 'tonal'"
          :color="overlayMoveMode ? 'primary' : undefined" @click="overlayMoveMode = !overlayMoveMode"
          :icon="mdiCursorMove" :aria-pressed="overlayMoveMode" :aria-label="overlayMoveModeLabel"
          :title="overlayMoveModeLabel" />
        <v-btn :variant="settingsOpen ? 'flat' : 'tonal'" :color="settingsOpen ? 'primary' : undefined"
          @click="toggleSettings" :icon="mdiCog" :aria-pressed="settingsOpen" aria-label="Settings" title="Settings" />
        <v-btn variant="tonal" @click="downloadPreview" :icon="mdiCameraOutline" :loading="previewDownloading"
          aria-label="Download preview (with lines and overlay, .png)"
          title="Download preview (with lines and overlay, .png)" />
        <v-btn variant="tonal" @click="captureRawFrame" :icon="mdiRaw"
          aria-label="Download raw frame (unprocessed pixel buffer, .bin)"
          title="Download raw frame (unprocessed pixel buffer, .bin)" />
      </div>
    </div>
  </v-card>

  <v-dialog v-model="clearLinesDialog" max-width="400">
    <v-card>
      <v-card-title :prepend-icon="mdiDeleteSweepOutline">
        Clear alignment lines?
      </v-card-title>
      <v-card-text>
        This removes all {{ alignmentLines.length }} alignment line(s) for this camera.
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="tonal" @click="clearLinesDialog = false">Cancel</v-btn>
        <v-btn variant="tonal" color="error" @click="confirmClearLines">Clear all</v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>

  <!-- One drawer per card, but only one can be open at a time (tracked in the store). -->
  <v-navigation-drawer v-model="settingsOpen" location="right" :width="settingsPanelWidth" :mobile="!mdAndUp"
    disable-resize-watcher disable-route-watcher touchless :aria-label="`${cameraTitle} settings`"
    :inert="!settingsOpen">
    <div class="d-flex align-center pl-4 pr-2 py-2">
      <v-icon :icon="mdiCog" class="mr-2" />
      <div class="text-subtitle-1 font-weight-bold text-truncate">
        {{ cameraTitle }}
      </div>
      <v-spacer />
      <v-btn variant="text" size="small" :icon="mdiClose" @click="settingsOpen = false" aria-label="Close settings"
        title="Close settings" />
    </div>
    <v-divider />

    <div class="pa-4">
      <div class="text-overline mb-2">Camera settings</div>
      <v-select v-model="selectedRotation" :items="rotationOptions" item-title="title" item-value="value"
        :disabled="settingsSaving" label="Preview rotation" />
      <v-select v-model="selectedImageFormatId" :items="camera.imageFormats" item-title="description" item-value="id"
        :disabled="settingsSaving" label="Image size / format" />
      <div class="text-caption text-medium-emphasis mb-3">
        Sets the camera capture resolution and frame rate.
      </div>
      <v-switch v-if="camera.supportsAutoBrightness" v-model="selectedAutoBrightness" :disabled="settingsSaving"
        color="primary" label="Automatic brightness" hide-details />
      <div v-if="camera.supportsAutoBrightness" class="text-caption text-medium-emphasis mb-3">
        Automatically adjusts the camera brightness as scene lighting changes.
      </div>
      <SliderField v-model="selectedQuality" v-if="selectedFormat?.quality" :min="selectedFormat?.quality.min ?? 80"
        :max="selectedFormat?.quality.max ?? 95" :step="selectedFormat?.quality.step ?? 1" :disabled="settingsSaving"
        label="Quality" label-width="5.5em" class="mb-3" />
      <div v-else class="text-center mb-4">This image format may not support quality settings</div>
      <SliderField v-for="control in camera.imageControls ?? []" :key="control.key"
        :model-value="selectedImageControls[control.key] ?? control.value" :min="control.min" :max="control.max"
        :step="control.step" :disabled="settingsSaving" :label="control.label" label-width="5.5em" class="mb-3"
        @update:model-value="setImageControl(control.key, $event)" />
      <div class="d-flex ga-2">
        <v-btn variant="tonal" color="primary" class="flex-grow-1" @click="saveSettings"
          :loading="settingsSaving">Save</v-btn>
        <v-btn variant="tonal" color="error" @click="loadSettingsFromCamera"
          :disabled="settingsSaving || !settingsDirty" title="Revert unsaved changes">Reset</v-btn>
      </div>
      <div class="text-caption text-medium-emphasis mt-2">
        {{ settingsDirty ? 'Unsaved changes. Closing the panel discards them.' : 'No unsaved changes.' }}
      </div>
    </div>
    <v-divider />

    <div class="pa-4">
      <div class="text-overline">Overlay image</div>
      <div class="text-caption text-medium-emphasis mb-3">
        Shown over the camera picture, under the alignment lines. Settings and custom images are kept in this
        browser only; changes apply immediately.
      </div>
      <v-select :model-value="overlaySource" :items="OVERLAY_SOURCE_OPTIONS" item-title="title" item-value="value"
        label="Overlay" hide-details class="mb-3" @update:model-value="setOverlaySource" />
      <template v-if="overlaySource === 'custom'">
        <input ref="overlayFileInput" type="file" accept="image/*" class="d-none" @change="onOverlayFileChange" />
        <div class="d-flex ga-2 mb-2">
          <v-btn variant="tonal" class="flex-grow-1" :prepend-icon="mdiImagePlusOutline" :loading="overlayBusy"
            @click="overlayFileInput?.click()">
            {{ overlayHasCustomImage ? 'Replace image' : 'Choose image' }}
          </v-btn>
          <v-btn v-if="overlayHasCustomImage" variant="tonal" color="error" :prepend-icon="mdiDeleteOutline"
            @click="removeOverlay">Remove</v-btn>
        </div>
        <div v-if="overlayHasCustomImage && overlayName" class="text-caption text-truncate mb-2" :title="overlayName">
          {{ overlayName }}
        </div>
      </template>
      <v-alert v-if="overlayError" type="error" variant="tonal" density="compact" class="mb-2" closable
        @click:close="overlayError = ''">
        {{ overlayError }}
      </v-alert>
      <v-alert v-if="overlayPersistWarning" type="warning" variant="tonal" density="compact" class="mb-2">
        {{ overlayPersistWarning }}
      </v-alert>
      <v-switch v-model="overlayVisible" :disabled="!overlayHasImage" color="primary" label="Show overlay image"
        hide-details />
      <SliderField v-model="overlayOpacity" :min="0" :max="100" :step="1" suffix="%" :disabled="!overlayHasImage"
        label="Opacity" class="mb-4" />
      <v-select v-model="overlayFit" :items="OVERLAY_FIT_OPTIONS" item-title="title" item-value="value"
        :disabled="!overlayHasImage" label="Fit" hide-details />

      <div class="text-subtitle-2 mt-4">Transform</div>
      <div class="text-caption text-medium-emphasis mb-2">
        In camera image coordinates (independent of the preview rotation).
      </div>
      <SliderField label="Rotate" suffix="°" :step="0.1" :min="OVERLAY_TRANSFORM_LIMITS.rotation.min"
        :max="OVERLAY_TRANSFORM_LIMITS.rotation.max" :model-value="overlayTransform.rotation"
        :disabled="!overlayHasImage" @update:model-value="updateOverlayTransform({ rotation: $event })" />
      <div class="d-flex ga-2 mb-3">
        <v-btn variant="tonal" size="small" class="flex-grow-1" :prepend-icon="mdiRotateLeft"
          :disabled="!overlayHasImage" @click="rotateOverlayBy(-90)">-90°</v-btn>
        <v-btn variant="tonal" size="small" class="flex-grow-1" :prepend-icon="mdiRotateRight"
          :disabled="!overlayHasImage" @click="rotateOverlayBy(90)">+90°</v-btn>
      </div>
      <SliderField v-if="overlayTransform.lockAspect" label="Scale" suffix="%" :step="0.1"
        :min="OVERLAY_TRANSFORM_LIMITS.scale.min" :max="OVERLAY_TRANSFORM_LIMITS.scale.max"
        :model-value="overlayTransform.scaleX" :disabled="!overlayHasImage"
        @update:model-value="updateOverlayTransform({ scaleX: $event })" />
      <template v-else>
        <SliderField label="Scale X" suffix="%" :step="0.1" :min="OVERLAY_TRANSFORM_LIMITS.scale.min"
          :max="OVERLAY_TRANSFORM_LIMITS.scale.max" :model-value="overlayTransform.scaleX"
          :disabled="!overlayHasImage" @update:model-value="updateOverlayTransform({ scaleX: $event })" />
        <SliderField label="Scale Y" suffix="%" :step="0.1" :min="OVERLAY_TRANSFORM_LIMITS.scale.min"
          :max="OVERLAY_TRANSFORM_LIMITS.scale.max" :model-value="overlayTransform.scaleY"
          :disabled="!overlayHasImage" @update:model-value="updateOverlayTransform({ scaleY: $event })" />
      </template>
      <v-switch :model-value="overlayTransform.lockAspect" :disabled="!overlayHasImage" color="primary"
        label="Lock aspect ratio" hide-details
        @update:model-value="updateOverlayTransform({ lockAspect: $event === true })" />
      <SliderField label="Offset X" suffix="%" :step="0.1" :min="OVERLAY_TRANSFORM_LIMITS.offset.min"
        :max="OVERLAY_TRANSFORM_LIMITS.offset.max" :model-value="overlayTransform.offsetX" :disabled="!overlayHasImage"
        @update:model-value="updateOverlayTransform({ offsetX: $event })" />
      <SliderField label="Offset Y" suffix="%" :step="0.1" :min="OVERLAY_TRANSFORM_LIMITS.offset.min"
        :max="OVERLAY_TRANSFORM_LIMITS.offset.max" :model-value="overlayTransform.offsetY" :disabled="!overlayHasImage"
        @update:model-value="updateOverlayTransform({ offsetY: $event })" />
      <div class="d-flex align-center ga-1 mb-3">
        <div class="text-caption text-medium-emphasis mr-auto">Nudge 0.1% (Shift: 1%)</div>
        <v-btn v-for="nudge in OVERLAY_NUDGES" :key="nudge.label" variant="tonal" size="small" :icon="nudge.icon"
          :disabled="!overlayHasImage" :aria-label="`Nudge overlay ${nudge.label}`"
          :title="`Nudge overlay ${nudge.label} (Shift: 1%)`" @click="nudgeOverlay(nudge.x, nudge.y, $event)" />
      </div>
      <div class="d-flex ga-2 mb-2">
        <v-btn :variant="overlayTransform.flipH ? 'flat' : 'tonal'"
          :color="overlayTransform.flipH ? 'primary' : undefined" size="small" class="flex-grow-1"
          :prepend-icon="mdiFlipHorizontal" :disabled="!overlayHasImage" :aria-pressed="overlayTransform.flipH"
          @click="updateOverlayTransform({ flipH: !overlayTransform.flipH })">Flip horizontal</v-btn>
        <v-btn :variant="overlayTransform.flipV ? 'flat' : 'tonal'"
          :color="overlayTransform.flipV ? 'primary' : undefined" size="small" class="flex-grow-1"
          :prepend-icon="mdiFlipVertical" :disabled="!overlayHasImage" :aria-pressed="overlayTransform.flipV"
          @click="updateOverlayTransform({ flipV: !overlayTransform.flipV })">Flip vertical</v-btn>
      </div>
      <v-switch v-model="overlayMoveMode" :disabled="!overlayHasImage || !overlayVisible" color="primary"
        label="Move overlay mode" hide-details />
      <div class="text-caption text-medium-emphasis mb-3">
        Drag on the preview to move the overlay; use the mouse wheel to scale it (Shift: fine).
      </div>
      <v-btn variant="tonal" block :prepend-icon="mdiRestore"
        :disabled="!overlayHasImage || isDefaultOverlayTransform(overlayTransform)" @click="resetOverlayTransform">
        Reset transform
      </v-btn>
    </div>
  </v-navigation-drawer>

  <v-snackbar v-model="snackbar" :timeout="2000" :color="snackbarColor">
    {{ snackbarText }}
  </v-snackbar>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { useDisplay } from 'vuetify'
import {
  mdiCameraOutline, mdiRaw, mdiCog, mdiVectorLine, mdiDeleteOutline, mdiDeleteSweepOutline, mdiEye, mdiEyeOff,
  mdiClose, mdiImageOutline, mdiImageOffOutline, mdiImagePlusOutline, mdiCursorMove, mdiRotateLeft, mdiRotateRight,
  mdiFlipHorizontal, mdiFlipVertical, mdiRestore, mdiChevronLeft, mdiChevronRight, mdiChevronUp, mdiChevronDown,
} from '@mdi/js';
import { useMainStore } from '@/store/mainstore';
import { useAlignmentLines } from '@/composables/useAlignmentLines';
import {
  useOverlayImage, OVERLAY_FIT_OPTIONS, OVERLAY_SOURCE_OPTIONS, OVERLAY_TRANSFORM_LIMITS, isDefaultOverlayTransform,
  overlayTransformCss, wrapRotation,
} from '@/composables/useOverlayImage';
import { capturePreview } from '@/composables/usePreviewCapture';
import type { Point } from '@/utils/previewGeometry';
import AlignmentOverlay from '@/components/AlignmentOverlay.vue';
import OverlayMoveSurface from '@/components/OverlayMoveSurface.vue';
import SliderField from '@/components/SliderField.vue';

const LOADING_IMAGE_SRC = "/loading.jpg"
const rotationOptions = [
  { title: '0°', value: 0 },
  { title: '90° clockwise', value: 90 },
  { title: '180°', value: 180 },
  { title: '270° clockwise', value: 270 },
]
/** Overlay nudge directions, in camera image coordinates. */
const OVERLAY_NUDGES = [
  { label: 'left', icon: mdiChevronLeft, x: -1, y: 0 },
  { label: 'right', icon: mdiChevronRight, x: 1, y: 0 },
  { label: 'up', icon: mdiChevronUp, x: 0, y: -1 },
  { label: 'down', icon: mdiChevronDown, x: 0, y: 1 },
]
/** Mouse wheel scale step in move mode: factor per notch, or percentage points per notch with Shift. */
const OVERLAY_WHEEL_FACTOR = 1.02
const OVERLAY_WHEEL_FINE_STEP = 0.1

const mainStore = useMainStore()
const { mdAndUp, width: displayWidth } = useDisplay()
// ~360px, but never wider than very narrow phone screens.
const settingsPanelWidth = computed(() => Math.min(360, displayWidth.value))

const props = defineProps<{
  camNum: number,
}>()

const camera = computed(() => mainStore.clientCameras[props.camNum])
const cameraTitle = computed(() => camera.value.name && camera.value.name.length > 0 ? camera.value.name : `Camera #${camera.value.index}`)

const viewportRef = ref<HTMLDivElement | null>(null)
const imgSrc = ref<string>(LOADING_IMAGE_SRC)
const previewImageFailed = ref<boolean>(false)
const settingsOpen = computed<boolean>({
  get: () => mainStore.settingsPanelCamNum === props.camNum,
  set: (open) => {
    if (open) {
      mainStore.settingsPanelCamNum = props.camNum
    } else if (mainStore.settingsPanelCamNum === props.camNum) {
      mainStore.settingsPanelCamNum = null
    }
  },
})
const rotationStorageKey = `camera-${camera.value.index}-preview-rotation`
const storedRotation = Number(localStorage.getItem(rotationStorageKey))
const rotation = ref<number>(rotationOptions.some(option => option.value === storedRotation) ? storedRotation : 0)
const selectedRotation = ref<number>(rotation.value)
const selectedImageFormatId = ref<number | string>(camera.value.currentImageFormat)
const selectedQuality = ref<number>(camera.value.currentQuality ?? 80)
const selectedAutoBrightness = ref<boolean>(camera.value.autoBrightnessEnabled ?? true)
const selectedImageControls = ref<Record<string, number>>({})
const settingsSaving = ref<boolean>(false)
const snackbar = ref<boolean>(false)
const snackbarText = ref<string>("")
const snackbarColor = ref<string>("success")
const previewDownloading = ref<boolean>(false)
const retryTimeoutId = ref<ReturnType<typeof setTimeout> | null>(null)
const reloadTimeoutId = ref<ReturnType<typeof setTimeout> | null>(null)

const showSnackbar = (text: string, color: 'success' | 'error' = 'success') => {
  snackbarText.value = text
  snackbarColor.value = color
  snackbar.value = true
}

const {
  lines: alignmentLines,
  visible: alignmentVisible,
  drawMode: alignmentDrawMode,
  selectedId: selectedLineId,
  addLine: addAlignmentLine,
  deleteSelected: deleteSelectedLine,
  clearAll: clearAlignmentLines,
} = useAlignmentLines(camera.value.index)
const clearLinesDialog = ref<boolean>(false)
const drawModeLabel = computed(() => alignmentDrawMode.value ? 'Stop drawing alignment lines' : 'Draw alignment lines')
const visibilityLabel = computed(() => alignmentVisible.value ? 'Hide alignment lines' : 'Show alignment lines')

const confirmClearLines = () => {
  clearAlignmentLines()
  clearLinesDialog.value = false
}

const {
  source: overlaySource,
  setSource: setOverlaySource,
  url: overlayUrl,
  name: overlayName,
  hasCustomImage: overlayHasCustomImage,
  opacity: overlayOpacity,
  visible: overlayVisible,
  fit: overlayFit,
  transform: overlayTransform,
  updateTransform: updateOverlayTransform,
  resetTransform: resetOverlayTransform,
  error: overlayError,
  persistWarning: overlayPersistWarning,
  busy: overlayBusy,
  hasImage: overlayHasImage,
  setFile: setOverlayFile,
  remove: removeOverlay,
} = useOverlayImage(camera.value.index)
const overlayFileInput = ref<HTMLInputElement | null>(null)
const overlayVisibilityLabel = computed(() => overlayVisible.value ? 'Hide overlay image' : 'Show overlay image')
const overlayImageStyle = computed(() => ({
  opacity: overlayOpacity.value / 100,
  objectFit: overlayFit.value === 'contain' ? 'contain' as const : 'fill' as const,
  transform: overlayTransformCss(overlayTransform.value),
}))

/** While on, dragging / wheeling on the preview moves / scales the overlay (exclusive with line draw mode). */
const overlayMoveMode = ref<boolean>(false)
const overlayMoveModeLabel = computed(() => overlayMoveMode.value ? 'Stop moving overlay image' : 'Move overlay image (drag / wheel)')
watch(overlayMoveMode, (on) => {
  if (on) alignmentDrawMode.value = false
})
watch(alignmentDrawMode, (on) => {
  if (on) overlayMoveMode.value = false
})
watch([overlayHasImage, overlayVisible], ([hasImage, visible]) => {
  if (!hasImage || !visible) overlayMoveMode.value = false
})

const rotateOverlayBy = (degrees: number) => {
  updateOverlayTransform({ rotation: wrapRotation(overlayTransform.value.rotation + degrees) })
}

const nudgeOverlay = (x: number, y: number, event: MouseEvent) => {
  const step = event.shiftKey ? 1 : 0.1
  updateOverlayTransform({
    offsetX: overlayTransform.value.offsetX + x * step,
    offsetY: overlayTransform.value.offsetY + y * step,
  })
}

let overlayDragStartOffset: Point = { x: 0, y: 0 }
const onOverlayDragStart = () => {
  overlayDragStartOffset = { x: overlayTransform.value.offsetX, y: overlayTransform.value.offsetY }
}
/** `delta` is the total drag so far as fractions of the image size; offsets are percent of the image size. */
const onOverlayDrag = (delta: Point) => {
  updateOverlayTransform({
    offsetX: overlayDragStartOffset.x + delta.x * 100,
    offsetY: overlayDragStartOffset.y + delta.y * 100,
  })
}

const zoomOverlay = (direction: 1 | -1, fine: boolean) => {
  const step = (scale: number) => fine
    ? scale + direction * OVERLAY_WHEEL_FINE_STEP
    : scale * OVERLAY_WHEEL_FACTOR ** direction
  const { scaleX, scaleY } = overlayTransform.value
  updateOverlayTransform({ scaleX: step(scaleX), scaleY: step(scaleY) })
}

const onOverlayFileChange = (event: Event) => {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  // Reset so choosing the same file again still triggers a change.
  input.value = ''
  void setOverlayFile(file)
}

const selectedFormat = computed(() => {
  return camera.value.imageFormats.find(format => format.id === selectedImageFormatId.value)
})

const cameraAspectRatio = computed(() => camera.value.currentResolution.width / camera.value.currentResolution.height)
const previewRotation = computed(() => settingsOpen.value ? selectedRotation.value : rotation.value)
const previewViewportStyle = computed(() => ({
  aspectRatio: previewRotation.value % 180 === 0 ? cameraAspectRatio.value : 1 / cameraAspectRatio.value,
}))
const previewImageStyle = computed(() => ({
  '--preview-rotation': `${previewRotation.value}deg`,
  width: previewRotation.value % 180 === 0 ? '100%' : `${cameraAspectRatio.value * 100}%`,
}))

/** Resets all staged (unsaved) settings to the current camera / preview values. */
const loadSettingsFromCamera = () => {
  selectedRotation.value = rotation.value
  selectedImageFormatId.value = camera.value.currentImageFormat
  selectedQuality.value = camera.value.currentQuality ?? selectedQuality.value
  selectedAutoBrightness.value = camera.value.autoBrightnessEnabled ?? true
  selectedImageControls.value = Object.fromEntries(
    (camera.value.imageControls ?? []).map(control => [control.key, control.value])
  )
}

const settingsDirty = computed(() => {
  const cam = camera.value
  return selectedRotation.value !== rotation.value
    || selectedImageFormatId.value !== cam.currentImageFormat
    || (!!selectedFormat.value?.quality && cam.currentQuality != null && selectedQuality.value !== cam.currentQuality)
    || (!!cam.supportsAutoBrightness && selectedAutoBrightness.value !== (cam.autoBrightnessEnabled ?? true))
    || (cam.imageControls ?? []).some(control => (selectedImageControls.value[control.key] ?? control.value) !== control.value)
})

const toggleSettings = () => {
  settingsOpen.value = !settingsOpen.value
}

// Opening starts from fresh camera info; closing (close button, scrim, Escape, another camera's gear)
// discards unsaved edits, which also reverts the live rotation preview.
watch(settingsOpen, () => {
  loadSettingsFromCamera()
})

const setImageControl = (key: string, value: number | null) => {
  if (value !== null) {
    selectedImageControls.value[key] = value
  }
}

const onImageError = () => {
  if (imgSrc.value === LOADING_IMAGE_SRC) return;
  previewImageFailed.value = true;

  if (retryTimeoutId.value) {
    clearTimeout(retryTimeoutId.value)
  }

  retryTimeoutId.value = setTimeout(() => {
    reloadCameraSrc(1000)
    retryTimeoutId.value = null
  }, 3000)
}

const realCameraUrl = computed(() => {
  let port: number | null = null;
  let path: string | null = null;

  if (camera.value.src.startsWith(':')) {
    const [, portStr, pathStr] = camera.value.src.split(/[:\/]/);
    port = Number(portStr);
    path = pathStr;

    const realUrl = new URL(path, location.href);
    realUrl.port = port.toString();
    return realUrl.toString();
  } else {
    path = camera.value.src;
    return new URL(path, location.href).toString();
  }
})

const reloadCameraSrc = (ms: number = 100) => {
  imgSrc.value = LOADING_IMAGE_SRC;
  previewImageFailed.value = false;

  if (reloadTimeoutId.value) {
    clearTimeout(reloadTimeoutId.value)
    reloadTimeoutId.value = null
  }

  reloadTimeoutId.value = setTimeout(() => {
    imgSrc.value = realCameraUrl.value;
    reloadTimeoutId.value = null;
  }, ms);
}

const downloadBlob = (blob: Blob, filename: string) => {
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = filename;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

/** The `<img>` inside v-img if it is currently showing a live stream frame, otherwise null. */
const getLivePreviewImage = (): HTMLImageElement | null => {
  if (imgSrc.value === LOADING_IMAGE_SRC || previewImageFailed.value || !viewportRef.value) return null
  const images = Array.from(viewportRef.value.querySelectorAll<HTMLImageElement>('img.v-img__img')).reverse()
  for (const img of images) {
    if (img.classList.contains('v-img__img--preload') || img.style.display === 'none') continue
    if (img.naturalWidth === 0 || img.naturalHeight === 0) continue
    const src = img.currentSrc || img.src
    if (!src || new URL(src, location.href).pathname === LOADING_IMAGE_SRC) continue
    return img
  }
  return null
}

const downloadPreview = async () => {
  if (previewDownloading.value) return
  previewDownloading.value = true
  try {
    const previewElement = viewportRef.value?.querySelector<HTMLElement>('.camera-preview-image')
    const result = await capturePreview({
      liveImage: getLivePreviewImage(),
      fallbackUrl: `/api/capture_image?source=${camera.value.index}`,
      rotation: previewRotation.value,
      displayWidth: previewElement?.offsetWidth ?? 0,
      lines: alignmentVisible.value ? alignmentLines.value : [],
      overlay: overlayUrl.value && overlayVisible.value
        ? {
          url: overlayUrl.value,
          opacity: overlayOpacity.value / 100,
          fit: overlayFit.value,
          transform: { ...overlayTransform.value },
        }
        : null,
    })
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-')
    downloadBlob(result.blob, `camera_${camera.value.index}_preview_${timestamp}.png`)
  } catch (error) {
    console.error(error)
    showSnackbar('Failed to capture preview', 'error')
  } finally {
    previewDownloading.value = false
  }
}

const captureRawFrame = () => {
  const url = `/api/capture_binary?source=${camera.value.index}`;

  const link = document.createElement('a');
  link.href = url;
  link.download = `camera_${camera.value.index}_raw.bin`;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
}

const saveSettings = async () => {
  imgSrc.value = LOADING_IMAGE_SRC;
  settingsSaving.value = true;
  await fetch('/api/set_camera_config', {
    method: 'POST',
    body: JSON.stringify({
      index: camera.value.index,
      image_format: selectedImageFormatId.value,
      jpeg_quality: selectedQuality.value,
      image_controls: selectedImageControls.value,
      auto_brightness_enabled: camera.value.supportsAutoBrightness ? selectedAutoBrightness.value : undefined,
    })
  }).then(res => {
    if (res.ok) {
      rotation.value = selectedRotation.value
      localStorage.setItem(rotationStorageKey, String(rotation.value))
      showSnackbar("Settings saved");
      // Refresh camera info now so the panel reflects the saved values without waiting for the next poll.
      void mainStore.updateCameraStatus();
    } else {
      showSnackbar("Failed to save settings", 'error');
    }
  }).catch(() => {
    showSnackbar("Failed to save settings", 'error');
  }).finally(() => {
    settingsSaving.value = false;
    // On small screens the panel covers the preview, so close it to show the result.
    if (!mdAndUp.value) {
      settingsOpen.value = false;
    }
    reloadCameraSrc();
  })
}

watch(realCameraUrl, (newUrl) => {
  if (imgSrc.value !== LOADING_IMAGE_SRC) {
    imgSrc.value = newUrl;
  }
})

watch(selectedImageFormatId, (formatId) => {
  if (formatId === camera.value.currentImageFormat && camera.value.currentQuality != null) {
    // Back on the active format (e.g. after a reset): use its current quality.
    selectedQuality.value = camera.value.currentQuality;
  } else if (selectedFormat.value?.quality) {
    selectedQuality.value = selectedFormat.value?.quality.default ?? selectedFormat.value?.quality.max ?? 90;
  }
})

onMounted(() => {
  reloadCameraSrc();
})

onUnmounted(() => {
  if (retryTimeoutId.value) {
    clearTimeout(retryTimeoutId.value)
    retryTimeoutId.value = null
  }
  settingsOpen.value = false
})
</script>

<style scoped>
.camera-preview-viewport {
  position: relative;
  width: 100%;
  overflow: hidden;
  background: #111;
}

.camera-preview-image {
  position: absolute;
  top: 50%;
  left: 50%;
  max-width: none;
  max-height: none;
  transform: translate(-50%, -50%) rotate(var(--preview-rotation));
  transform-origin: center;
}

/*
 * Client-side overlay image: same box as the camera picture, below the alignment lines (z-index 1).
 * The layer clips the transformed image to the picture area.
 */
.camera-overlay-layer {
  position: absolute;
  inset: 0;
  overflow: hidden;
  z-index: 0;
  pointer-events: none;
}

.camera-overlay-image {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  max-width: none;
  max-height: none;
  transform-origin: center;
  pointer-events: none;
  user-select: none;
}
</style>
