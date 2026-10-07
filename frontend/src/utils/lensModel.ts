/**
 * Lens model helpers (client-side only; nothing here changes the camera stream).
 *
 * Coordinate systems:
 * - Sensor: sensor pixel coordinates (as reported by the firmware).
 * - Image: continuous output image pixels (column i covers [i, i + 1)); the readout may be mirrored / flipped:
 *     sensor x = mirror ? window.x + window.width - u * scale : window.x + u * scale
 *     sensor y = flip   ? window.y + window.height - v * scale : window.y + v * scale
 * - Normalized image: image coordinates divided by the image size (0..1 inside the picture), which is what the
 *   preview overlays use so they stay aligned under rotation / resizing:
 *     nx = mirror ? (window.x + window.width - sx) / window.width : (sx - window.x) / window.width
 *     ny = flip   ? (window.y + window.height - sy) / window.height : (sy - window.y) / window.height
 *   The distortion model always works in sensor coordinates; mirror / flip only enter this final conversion.
 * - Ideal (undistorted) normalized camera coordinates: x = tan(angle), relative to the optical axis.
 *
 * Distortion is the OpenCV 5-parameter model with principal point = lens centre (sensor px) and focal lengths
 * fx, fy in sensor px:
 *   r2 = x^2 + y^2
 *   xd = x (1 + k1 r2 + k2 r2^2 + k3 r2^3) + 2 p1 x y + p2 (r2 + 2 x^2)
 *   yd = y (1 + k1 r2 + k2 r2^2 + k3 r2^3) + p1 (r2 + 2 y^2) + 2 p2 x y
 *   sensor = centre + (fx xd, fy yd)
 */
import type { LensDistortion, SensorRect } from '@/camera'

export type Point = { x: number, y: number }

/** Just the parts of a readout window needed for the image <-> sensor mapping. */
export type WindowMapping = SensorRect & { scale: number, mirror?: boolean, flip?: boolean }

/** Lens centre + optional distortion model, as used by the guides and the undistorted view. */
export type LensModel = {
  center: Point;
  distortion: LensDistortion | null;
}

export const DISTORTION_KEYS = ['fx', 'fy', 'k1', 'k2', 'p1', 'p2', 'k3'] as const
export type DistortionKey = typeof DISTORTION_KEYS[number]

/** Field angles (degrees) of the equal-angle rings. */
export const RING_ANGLES_DEG = [5, 10, 15, 20, 25, 30]
/** Angular spacing (degrees) of the distorted grid lines. */
export const GRID_STEP_DEG = 10
/** Never trust the polynomial model beyond this ideal radius (tan 70 deg), even if it stays monotonic. */
const MAX_IDEAL_RADIUS = Math.tan(70 * Math.PI / 180)

export const windowImageSize = (window: WindowMapping) => ({
  width: window.width / window.scale,
  height: window.height / window.scale,
})

export const sensorToNormalized = (window: WindowMapping, point: Point): Point => ({
  x: window.mirror ? (window.x + window.width - point.x) / window.width : (point.x - window.x) / window.width,
  y: window.flip ? (window.y + window.height - point.y) / window.height : (point.y - window.y) / window.height,
})

export const normalizedToSensor = (window: WindowMapping, point: Point): Point => ({
  x: window.mirror ? window.x + window.width - point.x * window.width : window.x + point.x * window.width,
  y: window.flip ? window.y + window.height - point.y * window.height : window.y + point.y * window.height,
})

/** Applies the OpenCV distortion to ideal normalized camera coordinates. */
export const distortIdeal = (point: Point, d: LensDistortion): Point => {
  const { x, y } = point
  const r2 = x * x + y * y
  const radial = 1 + r2 * (d.k1 + r2 * (d.k2 + r2 * d.k3))
  return {
    x: x * radial + 2 * d.p1 * x * y + d.p2 * (r2 + 2 * x * x),
    y: y * radial + d.p1 * (r2 + 2 * y * y) + 2 * d.p2 * x * y,
  }
}

/**
 * Largest ideal radius up to which the radial model is still monotonic (d r_d / d r > 0), capped at tan 70 deg.
 * Beyond it the polynomial folds back and its predictions are meaningless.
 */
export const monotonicRadiusLimit = (d: LensDistortion | null) => {
  if (!d) return MAX_IDEAL_RADIUS
  const step = 0.0025
  for (let r = step; r <= MAX_IDEAL_RADIUS; r += step) {
    const r2 = r * r
    if (1 + r2 * (3 * d.k1 + r2 * (5 * d.k2 + r2 * 7 * d.k3)) <= 0) return r - step
  }
  return MAX_IDEAL_RADIUS
}

export const isValidDistortion = (d: LensDistortion | null | undefined): d is LensDistortion =>
  !!d && DISTORTION_KEYS.every(key => Number.isFinite(d[key])) && d.fx > 0 && d.fy > 0

/**
 * Maps ideal normalized camera coordinates to normalized image coordinates of the given window.
 * With `applyDistortion` false the lens is treated as a perfect pinhole (fx, fy only), which is what the
 * undistorted view shows.
 */
export const idealToNormalized = (
  window: WindowMapping, model: LensModel & { distortion: LensDistortion }, ideal: Point, applyDistortion: boolean,
): Point => {
  const d = model.distortion
  const p = applyDistortion ? distortIdeal(ideal, d) : ideal
  return sensorToNormalized(window, { x: model.center.x + d.fx * p.x, y: model.center.y + d.fy * p.y })
}

export type LensGuideRing = {
  angleDeg: number;
  /** Closed polyline in normalized image coordinates. */
  points: Point[];
  /** Where to put the angle label (normalized image coordinates). */
  labelAt: Point;
}

export type LensGuideGeometry = {
  /** Lens centre in normalized image coordinates (null if crosshair is off). */
  center: Point | null;
  rings: LensGuideRing[];
  /** Polylines (normalized image coordinates) of straight world lines at GRID_STEP_DEG spacing. */
  grid: Point[][];
  /** Image size in output pixels (for drawing in pixel units). */
  imageWidth: number;
  imageHeight: number;
}

export type LensGuideOptions = {
  crosshair: boolean;
  rings: boolean;
  grid: boolean;
  /** False when the picture is shown undistorted: guides are then drawn for a perfect pinhole lens. */
  applyDistortion: boolean;
}

const RING_SEGMENTS = 180
const GRID_SAMPLES = 120

/**
 * Builds the lens guide geometry for a readout window. Rings and grid need a distortion model (at least fx, fy);
 * without one only the crosshair is produced.
 */
export const buildLensGuides = (window: WindowMapping, model: LensModel, options: LensGuideOptions): LensGuideGeometry => {
  const { width: imageWidth, height: imageHeight } = windowImageSize(window)
  const geometry: LensGuideGeometry = {
    center: options.crosshair ? sensorToNormalized(window, model.center) : null,
    rings: [],
    grid: [],
    imageWidth,
    imageHeight,
  }
  const d = model.distortion
  if (!isValidDistortion(d) || (!options.rings && !options.grid)) return geometry
  const withModel = { center: model.center, distortion: d }
  const rMax = monotonicRadiusLimit(d)
  const project = (ideal: Point) => idealToNormalized(window, withModel, ideal, options.applyDistortion)

  if (options.rings) {
    for (const angleDeg of RING_ANGLES_DEG) {
      const r = Math.tan(angleDeg * Math.PI / 180)
      if (r > rMax) break
      const points: Point[] = []
      for (let i = 0; i <= RING_SEGMENTS; i++) {
        const phi = i / RING_SEGMENTS * 2 * Math.PI
        points.push(project({ x: r * Math.cos(phi), y: r * Math.sin(phi) }))
      }
      // Label at the upper right of the ring in the image (sensor directions are reversed when mirrored / flipped).
      const label = { x: r * Math.SQRT1_2 * (window.mirror ? -1 : 1), y: r * Math.SQRT1_2 * (window.flip ? 1 : -1) }
      geometry.rings.push({ angleDeg, points, labelAt: project(label) })
    }
  }

  if (options.grid) {
    // Extent of the picture in (roughly) ideal coordinates, with margin for barrel distortion; capped by rMax.
    const corners = [
      { x: window.x, y: window.y }, { x: window.x + window.width, y: window.y },
      { x: window.x, y: window.y + window.height }, { x: window.x + window.width, y: window.y + window.height },
    ]
    const extentX = Math.max(...corners.map(c => Math.abs(c.x - model.center.x) / d.fx))
    const extentY = Math.max(...corners.map(c => Math.abs(c.y - model.center.y) / d.fy))
    const boundX = Math.min(rMax, extentX * 1.6)
    const boundY = Math.min(rMax, extentY * 1.6)
    const pushLine = (fixed: number, vertical: boolean, bound: number) => {
      let current: Point[] = []
      for (let i = 0; i <= GRID_SAMPLES; i++) {
        const t = -bound + 2 * bound * i / GRID_SAMPLES
        const ideal = vertical ? { x: fixed, y: t } : { x: t, y: fixed }
        if (Math.hypot(ideal.x, ideal.y) > rMax) {
          if (current.length > 1) geometry.grid.push(current)
          current = []
          continue
        }
        current.push(project(ideal))
      }
      if (current.length > 1) geometry.grid.push(current)
    }
    for (let deg = -80; deg <= 80; deg += GRID_STEP_DEG) {
      const value = Math.tan(deg * Math.PI / 180)
      if (Math.abs(value) <= boundX) pushLine(value, true, boundY)
      if (Math.abs(value) <= boundY) pushLine(value, false, boundX)
    }
  }
  return geometry
}

/** Field of view (degrees) of a pinhole lens with focal length `f` over `size` pixels centred on the axis. */
export const fieldOfViewDeg = (size: number, f: number) => 2 * Math.atan(size / 2 / f) * 180 / Math.PI

/** Snaps a sensor coordinate to 0.5 px. */
export const snapHalf = (value: number) => Math.round(value * 2) / 2
