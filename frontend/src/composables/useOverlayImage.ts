import { computed, onScopeDispose, ref, watch } from 'vue'

/** How the overlay image is fitted into the camera picture. */
export type OverlayFit = 'stretch' | 'contain'

export const OVERLAY_FIT_OPTIONS: { title: string, value: OverlayFit }[] = [
  { title: 'Stretch to fill', value: 'stretch' },
  { title: 'Contain (keep aspect ratio)', value: 'contain' },
]

/** Where the overlay image comes from: nothing, one of the built-in images, or the user's uploaded image. */
export type OverlaySource = 'none' | 'master' | 'blank' | 'custom'

/** Built-in overlays, served as-is from `public/overlays/` (same origin, so they never taint a canvas). */
const BUILTIN_OVERLAYS: Record<'master' | 'blank', { title: string, url: string }> = {
  master: { title: 'Master overlay', url: '/overlays/Master_overlay.png' },
  blank: { title: 'Blank overlay', url: '/overlays/Blank_overlay.png' },
}

export const OVERLAY_SOURCE_OPTIONS: { title: string, value: OverlaySource }[] = [
  { title: 'None', value: 'none' },
  { title: BUILTIN_OVERLAYS.master.title, value: 'master' },
  { title: BUILTIN_OVERLAYS.blank.title, value: 'blank' },
  { title: 'Custom image…', value: 'custom' },
]

const isOverlaySource = (value: unknown): value is OverlaySource =>
  OVERLAY_SOURCE_OPTIONS.some(option => option.value === value)

export const OVERLAY_MAX_BYTES = 10 * 1024 * 1024
const DEFAULT_OPACITY = 50

/**
 * Overlay placement in the unrotated camera image frame. Applied around the center of the fit box as
 * translate(offset) -> rotate -> scale (flips are negative scales).
 */
export type OverlayTransform = {
  /** Degrees, clockwise. */
  rotation: number;
  /** Percent of the fit box width. */
  scaleX: number;
  /** Percent of the fit box height (equals scaleX while the aspect ratio is locked). */
  scaleY: number;
  lockAspect: boolean;
  /** Percent of the camera image width. */
  offsetX: number;
  /** Percent of the camera image height. */
  offsetY: number;
  flipH: boolean;
  flipV: boolean;
}

export const OVERLAY_TRANSFORM_LIMITS = {
  rotation: { min: -180, max: 180 },
  scale: { min: 10, max: 400 },
  offset: { min: -100, max: 100 },
} as const

export const DEFAULT_OVERLAY_TRANSFORM: Readonly<OverlayTransform> = Object.freeze({
  rotation: 0,
  scaleX: 100,
  scaleY: 100,
  lockAspect: true,
  offsetX: 0,
  offsetY: 0,
  flipH: false,
  flipV: false,
})

/** Rounds to 0.1 (the control step) and clamps; non-finite values fall back to `fallback`. */
export const clampStep = (value: unknown, min: number, max: number, fallback: number) => {
  if (typeof value !== 'number' || !Number.isFinite(value)) return fallback
  const rounded = Math.round(Math.min(max, Math.max(min, value)) * 10) / 10
  return rounded === 0 ? 0 : rounded // no -0
}

/** Wraps an angle into -180..180 (keeping +180 rather than -180), rounded to 0.1. */
export const wrapRotation = (degrees: number) => {
  let wrapped = Math.round((((degrees + 180) % 360 + 360) % 360 - 180) * 10) / 10
  if (wrapped === -180 && degrees > 0) wrapped = 180
  return wrapped === 0 ? 0 : wrapped
}

/** Validates/clamps an arbitrary value into a complete transform. */
export const sanitizeOverlayTransform = (value: unknown): OverlayTransform => {
  const source = (typeof value === 'object' && value !== null ? value : {}) as Record<string, unknown>
  const d = DEFAULT_OVERLAY_TRANSFORM
  const { rotation: r, scale: s, offset: o } = OVERLAY_TRANSFORM_LIMITS
  const lockAspect = typeof source.lockAspect === 'boolean' ? source.lockAspect : d.lockAspect
  const scaleX = clampStep(source.scaleX, s.min, s.max, d.scaleX)
  return {
    rotation: clampStep(source.rotation, r.min, r.max, d.rotation),
    scaleX,
    scaleY: lockAspect ? scaleX : clampStep(source.scaleY, s.min, s.max, d.scaleY),
    lockAspect,
    offsetX: clampStep(source.offsetX, o.min, o.max, d.offsetX),
    offsetY: clampStep(source.offsetY, o.min, o.max, d.offsetY),
    flipH: typeof source.flipH === 'boolean' ? source.flipH : d.flipH,
    flipV: typeof source.flipV === 'boolean' ? source.flipV : d.flipV,
  }
}

export const isDefaultOverlayTransform = (transform: OverlayTransform) =>
  (Object.keys(DEFAULT_OVERLAY_TRANSFORM) as (keyof OverlayTransform)[])
    .every(key => transform[key] === DEFAULT_OVERLAY_TRANSFORM[key])

/**
 * CSS transform for an element whose box is the whole camera image area (percent translations are
 * relative to that box, and the default transform-origin is its center = the fit box center).
 */
export const overlayTransformCss = (t: OverlayTransform) =>
  `translate(${t.offsetX}%, ${t.offsetY}%) rotate(${t.rotation}deg) `
  + `scale(${(t.flipH ? -1 : 1) * t.scaleX / 100}, ${(t.flipV ? -1 : 1) * t.scaleY / 100})`

const parseTransform = (raw: string | null): OverlayTransform => {
  if (!raw) return { ...DEFAULT_OVERLAY_TRANSFORM }
  try {
    return sanitizeOverlayTransform(JSON.parse(raw))
  } catch {
    return { ...DEFAULT_OVERLAY_TRANSFORM }
  }
}

const DB_NAME = 'camera-overlay-images'
const DB_STORE = 'images'

type StoredOverlay = {
  blob: Blob;
  name: string;
}

let dbPromise: Promise<IDBDatabase> | null = null

const openDb = (): Promise<IDBDatabase> => {
  if (!dbPromise) {
    dbPromise = new Promise<IDBDatabase>((resolve, reject) => {
      try {
        const request = indexedDB.open(DB_NAME, 1)
        request.onupgradeneeded = () => {
          if (!request.result.objectStoreNames.contains(DB_STORE)) {
            request.result.createObjectStore(DB_STORE)
          }
        }
        request.onsuccess = () => resolve(request.result)
        request.onerror = () => reject(request.error)
        request.onblocked = () => reject(new Error('IndexedDB open blocked'))
      } catch (error) {
        // indexedDB may be undefined or throw (e.g. disabled storage).
        reject(error)
      }
    })
    dbPromise.catch(() => {
      dbPromise = null
    })
  }
  return dbPromise
}

const runDbRequest = async <T>(
  mode: IDBTransactionMode,
  makeRequest: (store: IDBObjectStore) => IDBRequest<T>,
): Promise<T> => {
  const db = await openDb()
  return new Promise<T>((resolve, reject) => {
    try {
      const transaction = db.transaction(DB_STORE, mode)
      const request = makeRequest(transaction.objectStore(DB_STORE))
      transaction.oncomplete = () => resolve(request.result)
      transaction.onerror = () => reject(transaction.error ?? request.error)
      transaction.onabort = () => reject(transaction.error ?? new Error('IndexedDB transaction aborted'))
    } catch (error) {
      reject(error)
    }
  })
}

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
    // Storage may be full or unavailable; the setting just won't persist.
  }
}

const parseOpacity = (raw: string | null) => {
  const value = Number(raw)
  return raw !== null && Number.isFinite(value) ? Math.min(100, Math.max(0, value)) : DEFAULT_OPACITY
}

/** Resolves once the image data can be decoded, so broken/unsupported files are rejected up front. */
const canDecodeImage = (url: string) => new Promise<boolean>((resolve) => {
  const img = new Image()
  img.onload = () => resolve(img.naturalWidth > 0 && img.naturalHeight > 0)
  img.onerror = () => resolve(false)
  img.src = url
})

/**
 * Per-camera client-side overlay image shown on top of the preview: a built-in image or the user's own.
 * The uploaded image is persisted in IndexedDB (too large for localStorage) and kept while another
 * source is selected; the source / opacity / visibility / fit mode / transform are persisted in
 * localStorage and shared by all sources.
 */
export const useOverlayImage = (cameraIndex: string | number) => {
  const dbKey = `camera-${cameraIndex}`
  const sourceStorageKey = `camera-${cameraIndex}-overlay-source`
  const opacityStorageKey = `camera-${cameraIndex}-overlay-opacity`
  const visibleStorageKey = `camera-${cameraIndex}-overlay-visible`
  const fitStorageKey = `camera-${cameraIndex}-overlay-fit`
  const transformStorageKey = `camera-${cameraIndex}-overlay-transform`

  const storedSource = readStorage(sourceStorageKey)
  // No (valid) saved choice yet: 'none', unless an uploaded image already exists (see loadStored).
  const hasStoredSource = isOverlaySource(storedSource)
  const source = ref<OverlaySource>(hasStoredSource ? storedSource : 'none')
  /** Object URL of the uploaded (custom) image, if any. */
  const customUrl = ref<string | null>(null)
  /** File name of the uploaded image. */
  const name = ref<string>('')
  /** Built-in image URL that failed to load (cleared when it is selected again). */
  const failedBuiltinUrl = ref<string | null>(null)
  /** URL of the active overlay image, or null when there is none. */
  const url = computed<string | null>(() => {
    if (source.value === 'custom') return customUrl.value
    if (source.value === 'none') return null
    const builtinUrl = BUILTIN_OVERLAYS[source.value].url
    return builtinUrl === failedBuiltinUrl.value ? null : builtinUrl
  })
  /** Opacity in percent (0..100). */
  const opacity = ref<number>(parseOpacity(readStorage(opacityStorageKey)))
  const visible = ref<boolean>(readStorage(visibleStorageKey) !== 'false')
  const fit = ref<OverlayFit>(readStorage(fitStorageKey) === 'contain' ? 'contain' : 'stretch')
  /** Always replaced as a whole (via updateTransform / resetTransform), never mutated in place. */
  const transform = ref<OverlayTransform>(parseTransform(readStorage(transformStorageKey)))
  const error = ref<string>('')
  const persistWarning = ref<string>('')
  const busy = ref<boolean>(false)
  const hasImage = computed(() => url.value !== null)

  // Bumped on every local change so a slow initial IndexedDB load can't overwrite a newer choice.
  let generation = 0
  // Set once the source was chosen by the user, so the initial load doesn't switch it to 'custom'.
  let sourceChosen = hasStoredSource
  let disposed = false

  const setObjectUrl = (blob: Blob | null) => {
    if (customUrl.value) URL.revokeObjectURL(customUrl.value)
    customUrl.value = blob ? URL.createObjectURL(blob) : null
  }

  /** Checks that the selected built-in image loads; shows an error (and no overlay) if it doesn't. */
  const checkBuiltin = async (value: OverlaySource) => {
    if (value === 'none' || value === 'custom') return
    const { title, url: builtinUrl } = BUILTIN_OVERLAYS[value]
    const ok = await canDecodeImage(builtinUrl)
    if (disposed || source.value !== value) return
    if (ok) {
      if (failedBuiltinUrl.value === builtinUrl) failedBuiltinUrl.value = null
    } else {
      failedBuiltinUrl.value = builtinUrl
      error.value = `The built-in "${title}" image could not be loaded.`
    }
  }

  /** Selects the overlay source; the uploaded image stays stored when switching away from 'custom'. */
  const setSource = (value: OverlaySource) => {
    if (!isOverlaySource(value)) return
    sourceChosen = true
    error.value = ''
    failedBuiltinUrl.value = null
    if (value !== 'none' && value !== source.value) visible.value = true
    source.value = value
  }

  watch(source, (value) => {
    writeStorage(sourceStorageKey, value)
    void checkBuiltin(value)
  }, { immediate: hasStoredSource })
  watch(opacity, value => writeStorage(opacityStorageKey, String(value)))
  watch(visible, value => writeStorage(visibleStorageKey, String(value)))
  watch(fit, value => writeStorage(fitStorageKey, value))
  watch(transform, value => writeStorage(transformStorageKey, JSON.stringify(value)))

  /** Applies a partial change; values are validated and clamped. Locking the aspect ratio copies X to Y. */
  const updateTransform = (patch: Partial<OverlayTransform>) => {
    const next = { ...transform.value, ...patch }
    if (next.lockAspect && patch.scaleY !== undefined && patch.scaleX === undefined) {
      next.scaleX = patch.scaleY
    }
    if (next.lockAspect) next.scaleY = next.scaleX
    transform.value = sanitizeOverlayTransform(next)
  }

  const resetTransform = () => {
    transform.value = { ...DEFAULT_OVERLAY_TRANSFORM }
  }

  const loadStored = async () => {
    const startGeneration = generation
    try {
      const stored = await runDbRequest<StoredOverlay | undefined>('readonly', store => store.get(dbKey))
      if (disposed || startGeneration !== generation || !stored || !(stored.blob instanceof Blob)) return
      setObjectUrl(stored.blob)
      name.value = typeof stored.name === 'string' ? stored.name : ''
      // Migration: an image uploaded before overlay sources existed stays selected.
      if (!sourceChosen) {
        sourceChosen = true
        source.value = 'custom'
      }
    } catch {
      // IndexedDB unavailable: the overlay just won't persist across reloads.
    }
  }

  const setFile = async (file: File | null | undefined) => {
    error.value = ''
    if (!file) return
    if (!file.type.startsWith('image/')) {
      error.value = `"${file.name}" is not an image file.`
      return
    }
    if (file.size > OVERLAY_MAX_BYTES) {
      error.value = `"${file.name}" is too large (${(file.size / 1024 / 1024).toFixed(1)} MB). Maximum is 10 MB.`
      return
    }

    busy.value = true
    const thisGeneration = ++generation
    const candidateUrl = URL.createObjectURL(file)
    try {
      if (!(await canDecodeImage(candidateUrl))) {
        error.value = `"${file.name}" could not be decoded as an image.`
        return
      }
    } finally {
      URL.revokeObjectURL(candidateUrl)
      busy.value = false
    }
    if (disposed || thisGeneration !== generation) return

    setObjectUrl(file)
    name.value = file.name
    sourceChosen = true
    source.value = 'custom'
    visible.value = true
    persistWarning.value = ''
    try {
      await runDbRequest('readwrite', store => store.put({ blob: file, name: file.name } satisfies StoredOverlay, dbKey))
    } catch {
      if (!disposed && thisGeneration === generation) {
        persistWarning.value = 'Browser storage is unavailable; the overlay image will not be kept after reloading.'
      }
    }
  }

  const remove = async () => {
    generation++
    error.value = ''
    persistWarning.value = ''
    setObjectUrl(null)
    name.value = ''
    resetTransform()
    try {
      await runDbRequest('readwrite', store => store.delete(dbKey))
    } catch {
      // Nothing persisted (or storage unavailable).
    }
  }

  onScopeDispose(() => {
    disposed = true
    setObjectUrl(null)
  })

  void loadStored()

  return {
    source,
    setSource,
    url,
    name,
    hasCustomImage: computed(() => customUrl.value !== null),
    opacity,
    visible,
    fit,
    transform,
    updateTransform,
    resetTransform,
    error,
    persistWarning,
    busy,
    hasImage,
    setFile,
    remove,
  }
}
