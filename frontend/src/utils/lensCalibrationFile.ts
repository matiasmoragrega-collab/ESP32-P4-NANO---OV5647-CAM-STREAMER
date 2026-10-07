import type { LensDistortion, SensorRect } from '@/camera'
import { DISTORTION_KEYS, type Point } from '@/utils/lensModel'

/** Body of `POST /api/set_lens_calibration` (also the calibration.json file format). */
export type LensCalibrationBody = {
  index: number | string;
  center: Point;
  distortion: LensDistortion | null;
}

const isRecord = (value: unknown): value is Record<string, unknown> => typeof value === 'object' && value !== null

const isFiniteNumber = (value: unknown): value is number => typeof value === 'number' && Number.isFinite(value)

/** Throws an Error with a user-facing message if the centre is unusable for this sensor. */
export const validateCenter = (center: Point, activeArea: SensorRect) => {
  if (!isFiniteNumber(center.x) || !isFiniteNumber(center.y)) throw new Error('Lens centre must be two numbers.')
  const { x, y, width, height } = activeArea
  if (center.x < x || center.x > x + width || center.y < y || center.y > y + height) {
    throw new Error(`Lens centre (${center.x}, ${center.y}) is outside the active area `
      + `(x ${x}…${x + width}, y ${y}…${y + height}).`)
  }
}

/** Throws an Error with a user-facing message if the distortion object is invalid. */
export const validateDistortion = (value: unknown): LensDistortion => {
  if (!isRecord(value)) throw new Error('"distortion" must be an object or null.')
  const missing = DISTORTION_KEYS.filter(key => !isFiniteNumber(value[key]))
  if (missing.length > 0) throw new Error(`Distortion parameter(s) missing or not numbers: ${missing.join(', ')}.`)
  const distortion = Object.fromEntries(DISTORTION_KEYS.map(key => [key, value[key] as number])) as LensDistortion
  if (!(distortion.fx > 0 && distortion.fy > 0)) throw new Error('Focal lengths fx and fy must be positive.')
  return distortion
}

/**
 * Parses a calibration.json file (same shape as the POST body: `{ index?, center: {x, y}, distortion: null | {...} }`).
 * Unknown extra fields are ignored. Throws an Error with a user-facing message.
 */
export const parseCalibrationFile = (text: string, activeArea: SensorRect) => {
  let data: unknown
  try {
    data = JSON.parse(text)
  } catch {
    throw new Error('The file is not valid JSON.')
  }
  if (!isRecord(data)) throw new Error('The file must contain a JSON object.')
  if (data.reset === true) throw new Error('This file is a reset request, not a calibration.')
  if (!isRecord(data.center)) throw new Error('Missing "center": {"x": …, "y": …}.')
  const center = { x: data.center.x as number, y: data.center.y as number }
  validateCenter(center, activeArea)
  const distortion = data.distortion === undefined || data.distortion === null ? null : validateDistortion(data.distortion)
  const index = typeof data.index === 'number' || typeof data.index === 'string' ? data.index : null
  return { center, distortion, index }
}
