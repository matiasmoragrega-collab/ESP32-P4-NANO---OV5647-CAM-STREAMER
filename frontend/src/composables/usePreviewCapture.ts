import type { AlignmentLine } from '@/composables/useAlignmentLines'
import type { OverlayFit, OverlayTransform } from '@/composables/useOverlayImage'
import { normalizeRotation } from '@/utils/previewGeometry'
import type { LensGuideGeometry, Point } from '@/utils/lensModel'
import { UndistortRenderer, type UndistortParams } from '@/utils/undistortRenderer'

/** Same look as the on-screen alignment overlay (see AlignmentOverlay.vue). */
const LINE_COLOR = '#ffeb3b'
const LINE_OUTLINE_COLOR = 'rgba(0, 0, 0, 0.75)'
const LINE_WIDTH_PX = 2
const LINE_OUTLINE_WIDTH_PX = 4
/** Same look as the on-screen lens guides (see LensGuidesOverlay.vue). */
const GUIDE_COLOR = '#00e5ff'
const GUIDE_GRID_COLOR = 'rgba(0, 229, 255, 0.65)'
const GUIDE_OUTLINE_COLOR = 'rgba(0, 0, 0, 0.6)'
const GUIDE_WIDTH_PX = 1.25
const GUIDE_OUTLINE_WIDTH_PX = 3

export type PreviewOverlayLayer = {
  /** Object URL (or any same-origin URL) of the overlay image. */
  url: string;
  /** 0..1 */
  opacity: number;
  fit: OverlayFit;
  /** Placement relative to the fit box (same as the on-screen CSS transform). */
  transform: OverlayTransform;
}

export type PreviewCaptureOptions = {
  /** The live stream `<img>` currently shown in the preview, or null if no live frame is displayed. */
  liveImage: HTMLImageElement | null;
  /** Single-frame endpoint used when the live frame can't be used (e.g. `/api/capture_image?source=0`). */
  fallbackUrl: string;
  /** Preview rotation in degrees, clockwise (multiple of 90). */
  rotation: number;
  /** On-screen (unrotated) width of the preview image in CSS pixels; used to scale line widths. */
  displayWidth: number;
  /** Alignment lines to draw (pass an empty array when hidden). */
  lines: AlignmentLine[];
  /** Overlay image to draw, or null when there is none / it is hidden. */
  overlay: PreviewOverlayLayer | null;
  /** Lens guides to draw (between the overlay image and the alignment lines), or null when hidden. */
  guides?: LensGuideGeometry | null;
  /**
   * When set, the frame is remapped undistorted first (same WebGL shader as the display-only undistorted view),
   * so the download matches what the preview shows.
   */
  undistort?: UndistortParams | null;
}

export type PreviewCaptureResult = {
  blob: Blob;
  width: number;
  height: number;
  /** Where the camera frame came from. */
  source: 'live' | 'fallback';
}

export const loadImage = (src: string) => new Promise<HTMLImageElement>((resolve, reject) => {
  const img = new Image()
  img.onload = () => resolve(img)
  img.onerror = () => reject(new Error(`Failed to load image ${src}`))
  img.src = src
})

/** Fetches a single JPEG frame and decodes it; the returned image is backed by a same-origin blob URL (never taints). */
const fetchFrame = async (url: string) => {
  const response = await fetch(url, { cache: 'no-store' })
  if (!response.ok) throw new Error(`Frame request failed: ${response.status}`)
  const blob = await response.blob()
  const objectUrl = URL.createObjectURL(blob)
  try {
    return { image: await loadImage(objectUrl), release: () => URL.revokeObjectURL(objectUrl) }
  } catch (error) {
    URL.revokeObjectURL(objectUrl)
    throw error
  }
}

const canvasToPng = (canvas: HTMLCanvasElement) => new Promise<Blob>((resolve, reject) => {
  try {
    // Throws a SecurityError synchronously if the canvas is tainted.
    canvas.toBlob(blob => blob ? resolve(blob) : reject(new Error('Canvas encoding failed')), 'image/png')
  } catch (error) {
    reject(error)
  }
})

/** Draws the lens guides in unrotated image space (`ctx` is already set up for it). */
const drawGuides = (
  ctx: CanvasRenderingContext2D, guides: LensGuideGeometry, width: number, height: number, scale: number,
  rotation: number,
) => {
  const toPx = (p: Point) => ({ x: p.x * width, y: p.y * height })
  const polyline = (points: Point[]) => {
    ctx.beginPath()
    points.forEach((p, i) => {
      const { x, y } = toPx(p)
      if (i === 0) ctx.moveTo(x, y)
      else ctx.lineTo(x, y)
    })
    ctx.stroke()
  }
  const lineWidth = Math.max(GUIDE_WIDTH_PX, GUIDE_WIDTH_PX * scale)
  const outlineWidth = Math.max(GUIDE_OUTLINE_WIDTH_PX, GUIDE_OUTLINE_WIDTH_PX * scale)
  const center = guides.center ? toPx(guides.center) : null
  const markerRadius = Math.min(width, height) / 60
  ctx.save()
  ctx.lineJoin = 'round'
  for (const outline of [true, false]) {
    ctx.lineWidth = outline ? outlineWidth : lineWidth
    ctx.strokeStyle = outline ? GUIDE_OUTLINE_COLOR : GUIDE_GRID_COLOR
    guides.grid.forEach(polyline)
    ctx.strokeStyle = outline ? GUIDE_OUTLINE_COLOR : GUIDE_COLOR
    guides.rings.forEach(ring => polyline(ring.points))
    if (center) {
      ctx.setLineDash(outline ? [] : [8 * scale, 5 * scale])
      ctx.beginPath()
      ctx.moveTo(0, center.y)
      ctx.lineTo(width, center.y)
      ctx.moveTo(center.x, 0)
      ctx.lineTo(center.x, height)
      ctx.stroke()
      ctx.setLineDash([])
      ctx.beginPath()
      ctx.arc(center.x, center.y, markerRadius, 0, 2 * Math.PI)
      ctx.stroke()
    }
  }
  if (guides.rings.length > 0) {
    // Labels stay upright in the rotated output, like the on-screen counter-rotated labels.
    const fontSize = Math.max(8, Math.min(width, height) / 32)
    ctx.font = `600 ${fontSize}px Roboto, sans-serif`
    ctx.textAlign = 'left'
    ctx.textBaseline = 'alphabetic'
    ctx.lineWidth = fontSize / 6
    ctx.strokeStyle = 'rgba(0, 0, 0, 0.75)'
    ctx.fillStyle = GUIDE_COLOR
    for (const ring of guides.rings) {
      const { x, y } = toPx(ring.labelAt)
      ctx.save()
      ctx.translate(x, y)
      ctx.rotate(-rotation * Math.PI / 180)
      ctx.strokeText(`${ring.angleDeg}°`, 0, 0)
      ctx.fillText(`${ring.angleDeg}°`, 0, 0)
      ctx.restore()
    }
  }
  ctx.restore()
}

/**
 * Composites camera frame (optionally undistorted) + overlay image + lens guides + alignment lines in unrotated
 * image space,
 * then applies the preview rotation (output is H x W for 90/270 degrees).
 */
const composite = (
  frame: CanvasImageSource,
  width: number,
  height: number,
  options: PreviewCaptureOptions,
  overlayImage: HTMLImageElement | null,
): HTMLCanvasElement => {
  if (!(width > 0 && height > 0)) throw new Error('Frame has no size')
  const rotation = normalizeRotation(options.rotation)
  const swapped = rotation % 180 !== 0

  const canvas = document.createElement('canvas')
  canvas.width = swapped ? height : width
  canvas.height = swapped ? width : height
  const ctx = canvas.getContext('2d')
  if (!ctx) throw new Error('Canvas 2D context unavailable')

  ctx.translate(canvas.width / 2, canvas.height / 2)
  ctx.rotate(rotation * Math.PI / 180)
  ctx.translate(-width / 2, -height / 2)

  if (options.undistort) {
    // Throws a SecurityError for a tainting frame, like getImageData below (caller falls back).
    const renderer = new UndistortRenderer()
    try {
      renderer.render(frame as TexImageSource, width, height, options.undistort)
      // Copy synchronously, before the WebGL drawing buffer can be cleared.
      ctx.drawImage(renderer.canvas, 0, 0, width, height)
    } finally {
      renderer.dispose()
    }
  } else {
    ctx.drawImage(frame, 0, 0, width, height)
  }
  // Fail fast (and fall back) if the frame tainted the canvas.
  ctx.getImageData(0, 0, 1, 1)

  if (options.overlay && overlayImage && overlayImage.naturalWidth > 0 && overlayImage.naturalHeight > 0) {
    ctx.save()
    ctx.globalAlpha = Math.min(1, Math.max(0, options.overlay.opacity))
    // Same as the preview: clipped to the frame, transformed around the frame (= fit box) center as
    // translate(offset) -> rotate -> scale (flips are negative scales).
    ctx.beginPath()
    ctx.rect(0, 0, width, height)
    ctx.clip()
    const t = options.overlay.transform
    ctx.translate(width / 2 + t.offsetX / 100 * width, height / 2 + t.offsetY / 100 * height)
    ctx.rotate(t.rotation * Math.PI / 180)
    ctx.scale((t.flipH ? -1 : 1) * t.scaleX / 100, (t.flipV ? -1 : 1) * t.scaleY / 100)
    if (options.overlay.fit === 'contain') {
      const scale = Math.min(width / overlayImage.naturalWidth, height / overlayImage.naturalHeight)
      const drawWidth = overlayImage.naturalWidth * scale
      const drawHeight = overlayImage.naturalHeight * scale
      ctx.drawImage(overlayImage, -drawWidth / 2, -drawHeight / 2, drawWidth, drawHeight)
    } else {
      ctx.drawImage(overlayImage, -width / 2, -height / 2, width, height)
    }
    ctx.restore()
  }

  if (options.guides) {
    drawGuides(ctx, options.guides, width, height, options.displayWidth > 0 ? width / options.displayWidth : 1, rotation)
  }

  if (options.lines.length > 0) {
    // Scale strokes so they look like the preview, where they are a fixed number of screen pixels.
    const scale = options.displayWidth > 0 ? width / options.displayWidth : 1
    const lineWidth = Math.max(LINE_WIDTH_PX, LINE_WIDTH_PX * scale)
    const outlineWidth = Math.max(LINE_OUTLINE_WIDTH_PX, LINE_OUTLINE_WIDTH_PX * scale)
    ctx.save()
    ctx.lineCap = 'round'
    for (const [color, strokeWidth] of [[LINE_OUTLINE_COLOR, outlineWidth], [LINE_COLOR, lineWidth]] as const) {
      ctx.strokeStyle = color
      ctx.lineWidth = strokeWidth
      for (const line of options.lines) {
        ctx.beginPath()
        ctx.moveTo(line.x1 * width, line.y1 * height)
        ctx.lineTo(line.x2 * width, line.y2 * height)
        ctx.stroke()
      }
    }
    ctx.restore()
  }

  return canvas
}

/**
 * Renders exactly what the preview shows (frame at native resolution, overlay image,
 * alignment lines, rotation) into a PNG. Uses the live preview frame when possible and
 * falls back to fetching a single frame from the device.
 */
export const capturePreview = async (options: PreviewCaptureOptions): Promise<PreviewCaptureResult> => {
  const overlayImage = options.overlay ? await loadImage(options.overlay.url).catch(() => null) : null

  const live = options.liveImage
  // Note: `complete` is not checked on purpose; an MJPEG (multipart) stream never finishes loading.
  if (live && live.naturalWidth > 0 && live.naturalHeight > 0) {
    try {
      const canvas = composite(live, live.naturalWidth, live.naturalHeight, options, overlayImage)
      const blob = await canvasToPng(canvas)
      return { blob, width: canvas.width, height: canvas.height, source: 'live' }
    } catch (error) {
      console.warn('Live preview capture failed, falling back to a single frame', error)
    }
  }

  const { image, release } = await fetchFrame(options.fallbackUrl)
  try {
    const canvas = composite(image, image.naturalWidth, image.naturalHeight, options, overlayImage)
    const blob = await canvasToPng(canvas)
    return { blob, width: canvas.width, height: canvas.height, source: 'fallback' }
  } finally {
    release()
  }
}
