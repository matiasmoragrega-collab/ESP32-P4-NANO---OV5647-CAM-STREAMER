/**
 * Geometry helpers for the camera preview, which is shown rotated by a multiple of 90 degrees
 * (clockwise, via a CSS transform on the image element). Everything drawn on top of the picture
 * (alignment lines, overlay image) lives in the unrotated image frame, so screen positions and
 * movements have to be mapped back through that rotation.
 */

export type Point = { x: number, y: number }

/** Snaps a rotation to a multiple of 90 degrees in 0..359. */
export const normalizeRotation = (rotation: number) => ((Math.round(rotation / 90) * 90) % 360 + 360) % 360

/** Unrotated rendered size of the image, given the screen bounding box of the rotated image element. */
export const unrotatedSize = (rect: DOMRect, rotation: number) => {
  const swapped = normalizeRotation(rotation) % 180 !== 0
  return {
    width: swapped ? rect.height : rect.width,
    height: swapped ? rect.width : rect.height,
  }
}

/** Maps a screen-space vector (CSS pixels) into the unrotated image frame (CSS pixels). */
export const screenVectorToImage = (dx: number, dy: number, rotation: number): Point => {
  const theta = normalizeRotation(rotation) * Math.PI / 180
  const cos = Math.round(Math.cos(theta))
  const sin = Math.round(Math.sin(theta))
  // Inverse of CSS rotate(theta) (clockwise, y axis pointing down).
  return {
    x: dx * cos + dy * sin,
    y: -dx * sin + dy * cos,
  }
}

/**
 * Converts a screen position into normalized unrotated image coordinates (0..1 inside the image,
 * not clamped). `rect` is the screen bounding box of the rotated image element.
 */
export const screenToNormalized = (rect: DOMRect, rotation: number, clientX: number, clientY: number): Point => {
  const { width, height } = unrotatedSize(rect, rotation)
  const { x, y } = screenVectorToImage(
    clientX - (rect.left + rect.width / 2),
    clientY - (rect.top + rect.height / 2),
    rotation,
  )
  return {
    x: width > 0 ? x / width + 0.5 : 0.5,
    y: height > 0 ? y / height + 0.5 : 0.5,
  }
}
