export type QualityRange = {
  min: number;
  max: number;
  step?: number;
  default?: number;
};

export type ImageFormat = {
  id: string | number;
  description: string;
  quality?: QualityRange;
  /** Sensor readout window of this mode (newer firmware only). */
  window?: SensorWindow;
};

/** A rectangle in sensor pixel coordinates. */
export type SensorRect = {
  x: number;
  y: number;
  width: number;
  height: number;
};

/** Sensor pixel address space (newer firmware only). */
export type SensorInfo = {
  arrayWidth: number;
  arrayHeight: number;
  activeArea: SensorRect;
};

/**
 * OpenCV 5-parameter distortion model (k1, k2, p1, p2, k3). Focal lengths are in sensor pixels; the principal point is
 * the lens centre. Only used client-side for guides and the display-only undistorted view.
 */
export type LensDistortion = {
  fx: number;
  fy: number;
  k1: number;
  k2: number;
  p1: number;
  p2: number;
  k3: number;
};

export type LensCalibration = {
  /** Lens optical centre in sensor pixel coordinates. */
  center: { x: number, y: number };
  source: 'default' | 'calibrated';
  distortion: LensDistortion | null;
};

/**
 * Where an image mode reads from on the sensor. Output image pixel (u, v) maps to sensor
 * x = mirror ? x + width - u * scale : x + u * scale, y = flip ? y + height - v * scale : y + v * scale;
 * the output size is width / scale x height / scale.
 */
export type SensorWindow = SensorRect & {
  scale: number;
  mirror: boolean;
  flip: boolean;
  /** Residual offset between the readout centre and the lens centre (sensor px), as reported by the camera. */
  centerError: { x: number, y: number };
  /** Lens centre positions this mode can be centred on exactly (sensor px). */
  centerRange: { xMin: number, xMax: number, yMin: number, yMax: number };
};

export type Resolution = {
  width: number;
  height: number;
};

export type CameraImageControl = {
  key: string;
  label: string;
  min: number;
  max: number;
  step: number;
  default: number;
  value: number;
};

export type Camera = {
  index: string | number;
  name?: string;
  src: string;
  currentFrameRate: number;
  currentImageFormat: number | string;
  currentImageFormatDescription?: string;
  currentQuality?: number;
  currentResolution: Resolution;
  supportsAutoBrightness?: boolean;
  autoBrightnessEnabled?: boolean;
  imageFormats: ImageFormat[];
  imageControls?: CameraImageControl[];
  /** Newer firmware only; all three are absent on older firmware. */
  sensor?: SensorInfo;
  lensCalibration?: LensCalibration;
  /** Readout window of the current mode. */
  window?: SensorWindow;
};
