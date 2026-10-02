<template>
  <v-card class="mb-3">
    <div class="camera-preview-viewport" :style="previewViewportStyle">
      <v-img :src="imgSrc" :aspect-ratio="cameraAspectRatio" cover class="camera-preview-image"
        :style="previewImageStyle" :id="`cam-${props.camNum}-v-img`" crossorigin="anonymous" @error="onImageError">
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
    <div class="d-flex justify-space-between align-center my-2 mx-3">
      <div>
        <div style="font-weight: bold; font-size: larger;">
          {{ camera.name && camera.name.length > 0 ? camera.name : `Camera #${camera.index}` }}
        </div>
        <div style="font-size: smaller; opacity: 70%;">
          {{ camera.currentImageFormatDescription }} @ {{ camera.currentFrameRate }} fps
          <span v-if="camera.currentQuality">
            (Quality: {{ camera.currentQuality }})
          </span>
        </div>
      </div>
      <div class="d-flex">
        <v-btn variant="tonal" @click="openSettings" :icon="mdiCog" class="mr-1" aria-label="Settings" />
        <v-btn variant="tonal" @click="captureFrame" :icon="mdiCameraOutline" class="mr-1"
          aria-label="Download Frame" />
        <v-btn variant="tonal" @click="captureRawFrame" :icon="mdiRaw" class="mr-1"
          aria-label="Download Raw Image (BIN)" />
      </div>
    </div>
  </v-card>

  <v-dialog v-model="settingsDialog" max-width="500">
    <v-card>
      <v-card-title :prepend-icon="mdiCog">
        Camera Settings
      </v-card-title>
      <v-card-text>
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
        <v-slider v-model="selectedQuality" v-if="selectedFormat?.quality" :min="selectedFormat?.quality.min ?? 80"
          :max="selectedFormat?.quality.max ?? 95" :step="selectedFormat?.quality.step ?? 1" :disabled="settingsSaving"
          label="Quality">
          <template #append>
            {{ selectedQuality }}
          </template>
        </v-slider>
        <div v-else class="text-center mb-4">This image format may not support quality settings</div>
        <div v-for="control in camera.imageControls ?? []" :key="control.key">
          <v-slider :model-value="selectedImageControls[control.key] ?? control.value" :min="control.min"
            :max="control.max" :step="control.step" :disabled="settingsSaving" :label="control.label"
            @update:model-value="setImageControl(control.key, $event)">
            <template #append>
              {{ selectedImageControls[control.key] ?? control.value }}
            </template>
          </v-slider>
        </div>
        <v-row>
          <v-col cols="9">
            <v-btn variant="tonal" @click="saveSettings" width="100%" :loading="settingsSaving">Save</v-btn>
          </v-col>
          <v-col cols="3">
            <v-btn variant="tonal" @click="settingsDialog = false" color="error" width="100%"
              :disabled="settingsSaving">Cancel</v-btn>
          </v-col>
        </v-row>
      </v-card-text>
    </v-card>
  </v-dialog>

  <v-snackbar v-model="saveStatusSnackbar" :timeout="2000" color="success">
    {{ saveStatusSnackbarText }}
  </v-snackbar>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { mdiCameraOutline, mdiRaw, mdiCog } from '@mdi/js';
import { useMainStore } from '@/store/mainstore';

const LOADING_IMAGE_SRC = "/loading.jpg"
const rotationOptions = [
  { title: '0°', value: 0 },
  { title: '90° clockwise', value: 90 },
  { title: '180°', value: 180 },
  { title: '270° clockwise', value: 270 },
]

const mainStore = useMainStore()

const props = defineProps<{
  camNum: number,
}>()

const camera = computed(() => mainStore.clientCameras[props.camNum])

const imgSrc = ref<string>(LOADING_IMAGE_SRC)
const settingsDialog = ref<boolean>(false)
const rotationStorageKey = `camera-${camera.value.index}-preview-rotation`
const storedRotation = Number(localStorage.getItem(rotationStorageKey))
const rotation = ref<number>(rotationOptions.some(option => option.value === storedRotation) ? storedRotation : 0)
const selectedRotation = ref<number>(rotation.value)
const selectedImageFormatId = ref<number | string>(camera.value.currentImageFormat)
const selectedQuality = ref<number>(camera.value.currentQuality ?? 80)
const selectedAutoBrightness = ref<boolean>(camera.value.autoBrightnessEnabled ?? true)
const selectedImageControls = ref<Record<string, number>>({})
const settingsSaving = ref<boolean>(false)
const saveStatusSnackbar = ref<boolean>(false)
const saveStatusSnackbarText = ref<string>("")
const retryTimeoutId = ref<ReturnType<typeof setTimeout> | null>(null)
const reloadTimeoutId = ref<ReturnType<typeof setTimeout> | null>(null)

const selectedFormat = computed(() => {
  return camera.value.imageFormats.find(format => format.id === selectedImageFormatId.value)
})

const cameraAspectRatio = computed(() => camera.value.currentResolution.width / camera.value.currentResolution.height)
const previewRotation = computed(() => settingsDialog.value ? selectedRotation.value : rotation.value)
const previewViewportStyle = computed(() => ({
  aspectRatio: previewRotation.value % 180 === 0 ? cameraAspectRatio.value : 1 / cameraAspectRatio.value,
}))
const previewImageStyle = computed(() => ({
  '--preview-rotation': `${previewRotation.value}deg`,
  width: previewRotation.value % 180 === 0 ? '100%' : `${cameraAspectRatio.value * 100}%`,
}))

const openSettings = () => {
  selectedRotation.value = rotation.value
  selectedImageFormatId.value = camera.value.currentImageFormat
  selectedQuality.value = camera.value.currentQuality ?? selectedQuality.value
  selectedAutoBrightness.value = camera.value.autoBrightnessEnabled ?? true
  selectedImageControls.value = Object.fromEntries(
    (camera.value.imageControls ?? []).map(control => [control.key, control.value])
  )
  settingsDialog.value = true
}

watch(settingsDialog, (isOpen) => {
  if (!isOpen) {
    selectedRotation.value = rotation.value
    selectedImageFormatId.value = camera.value.currentImageFormat
  }
})

const setImageControl = (key: string, value: number | null) => {
  if (value !== null) {
    selectedImageControls.value[key] = value
  }
}

const onImageError = () => {
  if (imgSrc.value === LOADING_IMAGE_SRC) return;

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

  if (reloadTimeoutId.value) {
    clearTimeout(reloadTimeoutId.value)
    reloadTimeoutId.value = null
  }

  reloadTimeoutId.value = setTimeout(() => {
    imgSrc.value = realCameraUrl.value;
    reloadTimeoutId.value = null;
  }, ms);
}

const captureFrame = () => {
  const url = `/api/capture_image?source=${camera.value.index}`;

  const link = document.createElement('a');
  link.href = url;
  link.download = `camera_${camera.value.index}_image.jpg`;
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
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
      saveStatusSnackbar.value = true;
      saveStatusSnackbarText.value = "Settings saved";
    } else {
      saveStatusSnackbar.value = true;
      saveStatusSnackbarText.value = "Failed to save settings";
    }
  }).finally(() => {
    settingsSaving.value = false;
    settingsDialog.value = false;
    reloadCameraSrc();
  })
}

watch(realCameraUrl, (newUrl) => {
  if (imgSrc.value !== LOADING_IMAGE_SRC) {
    imgSrc.value = newUrl;
  }
})

watch(selectedImageFormatId, () => {
  if (selectedFormat.value?.quality) {
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
</style>
