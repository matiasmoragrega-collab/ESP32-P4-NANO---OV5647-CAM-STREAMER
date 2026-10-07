import type { AlignmentLine } from '@/composables/useAlignmentLines'
import type { OverlayFit, OverlayTransform } from '@/composables/useOverlayImage'
import { normalizeRotation } from '@/utils/previewGeometry'

/** Same look as the on-screen alignment overlay (see AlignmentOverlay.vue). */
const LINE_COLOR = '#ffeb3b'
const LINE_OUTLINE_COLOR = 'rgba(0, 0, 0, 0.75)'
const LINE_WIDTH_PX = 2
const LINE_OUTLINE_WIDTH_PX = 4

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

/**
 * Composites camera frame + overlay image + alignment lines in unrotated image space,
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

  ctx.drawImage(frame, 0, 0, width, height)
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
