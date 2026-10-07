/**
 * WebGL remap that shows a camera frame undistorted (display only). For every output pixel (u, v) of the image:
 *   sensor s   = origin + dir * (u, v) * scale    (origin / dir account for mirror / flip, see lensModel.ts)
 *   ideal  n   = (s - centre) / (fx, fy)                       (perfect pinhole, same focal lengths)
 *   source s_d = centre + (fx, fy) * distort(n)                 (OpenCV k1, k2, p1, p2, k3)
 *   texel      = (s_d - origin) * dir / window.size
 * i.e. the inverse map is a forward evaluation of the distortion model, no iteration needed. Output pixels whose
 * ideal radius is beyond the model's monotonic range, or whose source lies outside the frame, are black.
 */
import type { LensDistortion } from '@/camera'
import { monotonicRadiusLimit, type Point, type WindowMapping } from '@/utils/lensModel'

export type UndistortParams = {
  window: WindowMapping;
  center: Point;
  distortion: LensDistortion;
}

const VERTEX_SHADER = `
attribute vec2 a_pos;
varying vec2 v_uv;
void main() {
  // Top-left origin, like image pixels.
  v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);
  gl_Position = vec4(a_pos, 0.0, 1.0);
}`

const FRAGMENT_SHADER = `
precision highp float;
uniform sampler2D u_tex;
uniform vec2 u_origin;   // sensor point of image corner (0, 0): window.xy, or the far edge when mirrored / flipped
uniform vec2 u_dir;      // +1 / -1 per axis (-1 = mirrored / flipped)
uniform vec2 u_winSize;  // window width, height (sensor px)
uniform vec2 u_center;   // lens centre (sensor px)
uniform vec2 u_f;        // fx, fy (sensor px)
uniform vec3 u_k;        // k1, k2, k3
uniform vec2 u_p;        // p1, p2
uniform float u_rmax2;
varying vec2 v_uv;
void main() {
  vec2 sensor = u_origin + u_dir * v_uv * u_winSize;
  vec2 n = (sensor - u_center) / u_f;
  float r2 = dot(n, n);
  if (r2 > u_rmax2) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
  float radial = 1.0 + r2 * (u_k.x + r2 * (u_k.y + r2 * u_k.z));
  vec2 d = n * radial + vec2(
    2.0 * u_p.x * n.x * n.y + u_p.y * (r2 + 2.0 * n.x * n.x),
    u_p.x * (r2 + 2.0 * n.y * n.y) + 2.0 * u_p.y * n.x * n.y);
  vec2 uv = (u_center + d * u_f - u_origin) * u_dir / u_winSize;
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) { gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
  gl_FragColor = texture2D(u_tex, uv);
}`

let supportCache: boolean | null = null

/** Whether WebGL is available in this browser (cached). */
export const isWebGlSupported = () => {
  if (supportCache !== null) return supportCache
  try {
    const canvas = document.createElement('canvas')
    const gl = canvas.getContext('webgl')
    supportCache = !!gl
    gl?.getExtension('WEBGL_lose_context')?.loseContext()
  } catch {
    supportCache = false
  }
  return supportCache
}

const compile = (gl: WebGLRenderingContext, type: number, source: string) => {
  const shader = gl.createShader(type)
  if (!shader) throw new Error('WebGL: cannot create shader')
  gl.shaderSource(shader, source)
  gl.compileShader(shader)
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader)
    gl.deleteShader(shader)
    throw new Error(`WebGL shader error: ${log}`)
  }
  return shader
}

export class UndistortRenderer {
  readonly canvas: HTMLCanvasElement
  private gl: WebGLRenderingContext
  private program: WebGLProgram
  private texture: WebGLTexture
  private buffer: WebGLBuffer
  private uniforms: Record<string, WebGLUniformLocation | null>

  /** Throws if WebGL is unavailable. */
  constructor(canvas: HTMLCanvasElement = document.createElement('canvas')) {
    this.canvas = canvas
    const gl = canvas.getContext('webgl', { alpha: false, antialias: false, depth: false, stencil: false })
    if (!gl) throw new Error('WebGL is not available')
    this.gl = gl
    const program = gl.createProgram()
    if (!program) throw new Error('WebGL: cannot create program')
    gl.attachShader(program, compile(gl, gl.VERTEX_SHADER, VERTEX_SHADER))
    gl.attachShader(program, compile(gl, gl.FRAGMENT_SHADER, FRAGMENT_SHADER))
    gl.linkProgram(program)
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(`WebGL link error: ${gl.getProgramInfoLog(program)}`)
    }
    this.program = program
    gl.useProgram(program)

    const buffer = gl.createBuffer()
    if (!buffer) throw new Error('WebGL: cannot create buffer')
    this.buffer = buffer
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer)
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW)
    const position = gl.getAttribLocation(program, 'a_pos')
    gl.enableVertexAttribArray(position)
    gl.vertexAttribPointer(position, 2, gl.FLOAT, false, 0, 0)

    const texture = gl.createTexture()
    if (!texture) throw new Error('WebGL: cannot create texture')
    this.texture = texture
    gl.bindTexture(gl.TEXTURE_2D, texture)
    // Non-power-of-two textures: no mipmaps, clamp.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR)
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false)

    this.uniforms = Object.fromEntries(
      ['u_tex', 'u_origin', 'u_dir', 'u_winSize', 'u_center', 'u_f', 'u_k', 'u_p', 'u_rmax2']
        .map(name => [name, gl.getUniformLocation(program, name)]),
    )
    gl.uniform1i(this.uniforms.u_tex, 0)
  }

  /**
   * Uploads `source` (a decoded frame of the current mode) and draws it undistorted into the canvas, resizing the
   * canvas to `width` x `height`. Throws a SecurityError for cross-origin (tainting) sources.
   */
  render(source: TexImageSource, width: number, height: number, params: UndistortParams) {
    const { gl, uniforms: u } = this
    if (this.canvas.width !== width || this.canvas.height !== height) {
      this.canvas.width = width
      this.canvas.height = height
    }
    gl.viewport(0, 0, width, height)
    gl.bindTexture(gl.TEXTURE_2D, this.texture)
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source)

    const { window: w, center, distortion: d } = params
    const rMax = monotonicRadiusLimit(d)
    gl.uniform2f(u.u_origin, w.mirror ? w.x + w.width : w.x, w.flip ? w.y + w.height : w.y)
    gl.uniform2f(u.u_dir, w.mirror ? -1 : 1, w.flip ? -1 : 1)
    gl.uniform2f(u.u_winSize, w.width, w.height)
    gl.uniform2f(u.u_center, center.x, center.y)
    gl.uniform2f(u.u_f, d.fx, d.fy)
    gl.uniform3f(u.u_k, d.k1, d.k2, d.k3)
    gl.uniform2f(u.u_p, d.p1, d.p2)
    gl.uniform1f(u.u_rmax2, rMax * rMax)
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4)
  }

  dispose() {
    const { gl } = this
    gl.deleteTexture(this.texture)
    gl.deleteBuffer(this.buffer)
    gl.deleteProgram(this.program)
    gl.getExtension('WEBGL_lose_context')?.loseContext()
  }
}
