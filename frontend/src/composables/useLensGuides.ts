import { ref, watch } from 'vue'

const readStorage = (key: string): string | null => {
  try {
    return localStorage.getItem(key)
  } catch {
    return null
  }
}

const writeStorage = (key: string, value: string) => {
  try {
    localStorage.setItem(key, value)
  } catch {
    // Storage may be unavailable (e.g. private mode); toggles just won't persist.
  }
}

/**
 * Per-camera lens guide toggles (persisted in localStorage) and the display-only undistorted view toggle
 * (deliberately not persisted: it always starts off).
 */
export const useLensGuides = (cameraIndex: string | number) => {
  const storageKey = `camera-${cameraIndex}-lens-guides`
  let stored: Record<string, unknown> = {}
  try {
    const parsed: unknown = JSON.parse(readStorage(storageKey) ?? '{}')
    if (typeof parsed === 'object' && parsed !== null) stored = parsed as Record<string, unknown>
  } catch {
    // ignore corrupt storage
  }

  const crosshair = ref<boolean>(stored.crosshair === true)
  const rings = ref<boolean>(stored.rings === true)
  const grid = ref<boolean>(stored.grid === true)
  const undistort = ref<boolean>(false)

  watch([crosshair, rings, grid], ([crosshairOn, ringsOn, gridOn]) => {
    writeStorage(storageKey, JSON.stringify({ crosshair: crosshairOn, rings: ringsOn, grid: gridOn }))
  })

  return { crosshair, rings, grid, undistort }
}
