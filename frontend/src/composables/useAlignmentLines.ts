import { ref, watch } from 'vue'

/**
 * A single alignment line. Endpoints are stored in normalized image
 * coordinates (0..1 of the unrotated image width/height), so a line stays
 * on the same scene feature regardless of preview rotation or card size.
 */
export type AlignmentLine = {
  id: string;
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

const MAX_LINES = 200

let idCounter = 0
// crypto.randomUUID() is unavailable on plain-HTTP (insecure) origins such as the device itself.
export const createLineId = () => `${Date.now().toString(36)}-${(idCounter++).toString(36)}-${Math.random().toString(36).slice(2, 8)}`

const clamp01 = (value: number) => Math.min(1, Math.max(0, value))

const isFiniteNumber = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value)

const parseLines = (raw: string | null): AlignmentLine[] => {
  if (!raw) return []
  try {
    const parsed: unknown = JSON.parse(raw)
    if (!Array.isArray(parsed)) return []
    const lines: AlignmentLine[] = []
    for (const item of parsed) {
      if (typeof item !== 'object' || item === null) continue
      const { id, x1, y1, x2, y2 } = item as Record<string, unknown>
      if (![x1, y1, x2, y2].every(isFiniteNumber)) continue
      lines.push({
        id: typeof id === 'string' && id.length > 0 ? id : createLineId(),
        x1: clamp01(x1 as number),
        y1: clamp01(y1 as number),
        x2: clamp01(x2 as number),
        y2: clamp01(y2 as number),
      })
      if (lines.length >= MAX_LINES) break
    }
    return lines
  } catch {
    return []
  }
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
    // Storage may be full or unavailable (e.g. private mode); lines just won't persist.
  }
}

/**
 * Per-camera alignment line state, persisted in localStorage.
 */
export const useAlignmentLines = (cameraIndex: string | number) => {
  const linesStorageKey = `camera-${cameraIndex}-alignment-lines`
  const visibleStorageKey = `camera-${cameraIndex}-alignment-lines-visible`

  const lines = ref<AlignmentLine[]>(parseLines(readStorage(linesStorageKey)))
  const visible = ref<boolean>(readStorage(visibleStorageKey) !== 'false')
  const drawMode = ref<boolean>(false)
  const selectedId = ref<string | null>(null)

  watch(lines, (value) => {
    writeStorage(linesStorageKey, JSON.stringify(value))
  }, { deep: true })

  watch(visible, (value) => {
    writeStorage(visibleStorageKey, String(value))
    if (!value) {
      drawMode.value = false
      selectedId.value = null
    }
  })

  const addLine = (line: Omit<AlignmentLine, 'id'>) => {
    if (lines.value.length >= MAX_LINES) return
    lines.value.push({ ...line, id: createLineId() })
  }

  const deleteSelected = () => {
    if (!selectedId.value) return
    lines.value = lines.value.filter(line => line.id !== selectedId.value)
    selectedId.value = null
  }

  const clearAll = () => {
    lines.value = []
    selectedId.value = null
  }

  return {
    lines,
    visible,
    drawMode,
    selectedId,
    addLine,
    deleteSelected,
    clearAll,
  }
}
