import type { MockHandler } from "vite-plugin-mock-server";
import type { Camera } from "../src/camera";

// ---- OV5647 (newer firmware: sensor geometry, lens calibration, per-mode readout windows) ----------------------

type Distortion = NonNullable<NonNullable<Camera["lensCalibration"]>["distortion"]>;
type Window = NonNullable<Camera["window"]>;

const OV5647_SENSOR = {
  arrayWidth: 2624,
  arrayHeight: 1956,
  activeArea: { x: 16, y: 6, width: 2592, height: 1944 },
};
/** Firmware default lens centre (centre of the active area). */
const OV5647_DEFAULT_CENTER = { x: 16 + 2592 / 2, y: 6 + 1944 / 2 };
/** The mock's "real" lens, used to draw the mock frames (so guides / undistortion can be checked visually). */
const OV5647_TRUE_CENTER = { x: 1330, y: 990 };
const OV5647_TRUE_DISTORTION: Distortion = { fx: 2571, fy: 2571, k1: -0.12, k2: 0.08, p1: 0.0005, p2: -0.0003, k3: 0 };

/** The 5 OV5647 modes: output size, binning scale, fps. */
const OV5647_MODES = [
  { width: 1280, height: 960, scale: 2, fps: 45 },
  { width: 1920, height: 1080, scale: 1, fps: 30 },
  { width: 800, height: 640, scale: 2, fps: 50 },
  { width: 800, height: 800, scale: 2, fps: 50 },
  { width: 800, height: 1280, scale: 1, fps: 30 },
];

const lensState = {
  center: { ...OV5647_TRUE_CENTER },
  source: "calibrated" as "default" | "calibrated",
  distortion: null as Distortion | null,
};
let frameVersion = 0;

const clamp = (v: number, min: number, max: number) => Math.min(max, Math.max(min, v));

/** Simple clamp model: centre the window on the lens, keep it inside the active area, even (Bayer) offsets. */
const computeWindow = (mode: (typeof OV5647_MODES)[number]): Window => {
  const a = OV5647_SENSOR.activeArea;
  const width = mode.width * mode.scale;
  const height = mode.height * mode.scale;
  const centerRange = {
    xMin: a.x + width / 2,
    xMax: a.x + a.width - width / 2,
    yMin: a.y + height / 2,
    yMax: a.y + a.height - height / 2,
  };
  const even = (v: number) => Math.round(v / 2) * 2;
  const x = clamp(even(lensState.center.x - width / 2), a.x, a.x + a.width - width);
  const y = clamp(even(lensState.center.y - height / 2), a.y, a.y + a.height - height);
  return {
    x, y, width, height, scale: mode.scale, mirror: true, flip: false,
    // Achieved window centre minus requested (lens) centre (sensor px), like the firmware.
    centerError: { x: x + width / 2 - lensState.center.x, y: y + height / 2 - lensState.center.y },
    centerRange,
  };
};

const ov5647 = {
  index: 0,
  name: "OV5647",
  src: "/api/mock_frame?index=0&v=0",
  currentFrameRate: 45,
  currentImageFormat: 0,
  currentImageFormatDescription: "",
  currentQuality: 80,
  currentResolution: { width: 1280, height: 960 },
  supportsAutoBrightness: true,
  autoBrightnessEnabled: true,
  imageFormats: OV5647_MODES.map((mode, id) => ({
    id,
    description: `${mode.width}x${mode.height} RAW10 @${mode.fps} fps`,
    quality: { min: 1, max: 100, step: 1, default: 80 },
  })),
} as Camera;

const refreshOv5647 = () => {
  const camera = ov5647;
  const formatId = Number(camera.currentImageFormat);
  const mode = OV5647_MODES[formatId] ?? OV5647_MODES[0];
  camera.imageFormats.forEach((format, id) => {
    format.window = computeWindow(OV5647_MODES[id]);
  });
  camera.sensor = OV5647_SENSOR;
  camera.lensCalibration = {
    center: { ...lensState.center },
    source: lensState.source,
    distortion: lensState.distortion ? { ...lensState.distortion } : null,
  };
  camera.window = computeWindow(mode);
  camera.currentResolution = { width: mode.width, height: mode.height };
  camera.currentFrameRate = mode.fps;
  camera.currentImageFormatDescription = camera.imageFormats[formatId]?.description;
  camera.src = `/api/mock_frame?index=0&v=${frameVersion}`;
};
refreshOv5647();

const distortIdeal = (x: number, y: number, d: Distortion) => {
  const r2 = x * x + y * y;
  const radial = 1 + r2 * (d.k1 + r2 * (d.k2 + r2 * d.k3));
  return [x * radial + 2 * d.p1 * x * y + d.p2 * (r2 + 2 * x * x), y * radial + d.p1 * (r2 + 2 * y * y) + 2 * d.p2 * x * y];
};

/**
 * SVG test frame for the current mode: a 5 deg world grid seen through the mock's true lens (so it is barrel-distorted),
 * equal-angle circles every 10 deg, and a red target at the true optical centre.
 */
const renderMockFrame = () => {
  const w = ov5647.window!;
  const W = w.width / w.scale;
  const H = w.height / w.scale;
  const c = OV5647_TRUE_CENTER;
  const d = OV5647_TRUE_DISTORTION;
  // Sensor -> image, honouring mirror / flip like the firmware.
  const sensorToImage = (sx: number, sy: number) => [
    (w.mirror ? w.x + w.width - sx : sx - w.x) / w.scale,
    (w.flip ? w.y + w.height - sy : sy - w.y) / w.scale,
  ];
  const toImage = (ix: number, iy: number) => {
    const [xd, yd] = distortIdeal(ix, iy, d);
    return sensorToImage(c.x + d.fx * xd, c.y + d.fy * yd);
  };
  const fmt = (p: number[]) => `${p[0].toFixed(1)},${p[1].toFixed(1)}`;
  const lines: string[] = [];
  for (let deg = -40; deg <= 40; deg += 5) {
    const v = Math.tan((deg * Math.PI) / 180);
    const vertical: string[] = [];
    const horizontal: string[] = [];
    for (let i = 0; i <= 80; i++) {
      const t = -0.85 + (1.7 * i) / 80;
      vertical.push(fmt(toImage(v, t)));
      horizontal.push(fmt(toImage(t, v)));
    }
    const color = deg === 0 ? "#ffffff" : deg % 10 === 0 ? "#9e9e9e" : "#5a5a5a";
    lines.push(`<polyline points="${vertical.join(" ")}" stroke="${color}" />`);
    lines.push(`<polyline points="${horizontal.join(" ")}" stroke="${color}" />`);
  }
  const circles: string[] = [];
  for (let deg = 10; deg <= 30; deg += 10) {
    const r = Math.tan((deg * Math.PI) / 180);
    const points: string[] = [];
    for (let i = 0; i <= 120; i++) {
      const phi = (i / 120) * 2 * Math.PI;
      points.push(fmt(toImage(r * Math.cos(phi), r * Math.sin(phi))));
    }
    circles.push(`<polyline points="${points.join(" ")}" stroke="#ff9800" stroke-dasharray="6 6" />`);
  }
  const [tx, ty] = sensorToImage(c.x, c.y);
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${W}" height="${H}" viewBox="0 0 ${W} ${H}">
<rect width="${W}" height="${H}" fill="#202830" />
<g fill="none" stroke-width="2">${lines.join("")}${circles.join("")}</g>
<circle cx="${tx}" cy="${ty}" r="10" fill="none" stroke="#ff1744" stroke-width="3" />
<line x1="${tx - 18}" y1="${ty}" x2="${tx + 18}" y2="${ty}" stroke="#ff1744" stroke-width="2" />
<line x1="${tx}" y1="${ty - 18}" x2="${tx}" y2="${ty + 18}" stroke="#ff1744" stroke-width="2" />
<text x="12" y="28" fill="#ffffff" font-family="sans-serif" font-size="20">mock ${W}x${H}, window ${w.x},${w.y} ${w.width}x${w.height} /${w.scale}${w.mirror ? " mirrored" : ""}; red = true lens centre ${c.x},${c.y}</text>
</svg>`;
};

const mockCameras: Camera[] = [
  ov5647,
  {
    index: 1,
    name: "Back Camera",
    src: "https://picsum.photos/1920/1080",
    currentFrameRate: 25,
    currentImageFormat: 1,
    currentImageFormatDescription: "RGB 5-6-5 1920x1080",
    currentQuality: 90,
    currentResolution: {
      width: 1920,
      height: 1080,
    },
    imageFormats: [
      {
        id: 1,
        description: "RGB 5-6-5 1920x1080",
        quality: {
          min: 50,
          max: 100,
          step: 5,
          default: 90,
        },
      },
      {
        id: 2,
        description: "RGB 8-8-8 1920x1080",
        quality: {
          min: 80,
          max: 100,
          step: 1,
          default: 95,
        },
      },
      {
        id: 3,
        description: "RGB 8-8-8 1080x720",
        quality: {
          min: 60,
          max: 100,
          step: 10,
          default: 80,
        },
      },
    ],
  },
  {
    index: 2,
    name: "Side Camera",
    src: "https://picsum.photos/800/600",
    currentFrameRate: 15,
    currentImageFormat: 2,
    currentImageFormatDescription: "RGB 8-8-8 800x600",
    currentQuality: 95,
    currentResolution: {
      width: 800,
      height: 600,
    },
    imageFormats: [
      {
        id: 1,
        description: "RGB 8-8-8 800x600",
        quality: {
          min: 50,
          max: 100,
          step: 5,
          default: 85,
        },
      },
      {
        id: 2,
        description: "YUV 4-2-2 800x600",
        quality: {
          min: 80,
          max: 100,
          step: 1,
          default: 95,
        },
      },
    ],
  },
];

const updateCameraSrc = (camera: Camera) => {
  const { width, height } = camera.currentResolution;
  camera.src = `https://picsum.photos/${width}/${height}`;
};

const readBody = (req: any) => new Promise<string>((resolve) => {
  let body = "";
  req.on("data", (chunk: any) => { body += chunk.toString(); });
  req.on("end", () => resolve(body));
});

const isNum = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v);

export default (): MockHandler[] => [
  {
    pattern: "/api/mock_frame",
    method: "GET",
    handle: (req, res) => {
      res.setHeader("Content-Type", "image/svg+xml");
      res.setHeader("Cache-Control", "no-store");
      res.end(renderMockFrame());
    },
  },

  {
    pattern: "/api/set_lens_calibration",
    method: "POST",
    handle: async (req, res) => {
      const fail = (message: string) => {
        res.statusCode = 400;
        res.setHeader("Content-Type", "text/plain");
        res.end(message);
      };
      let body: any;
      try {
        body = JSON.parse(await readBody(req));
      } catch {
        return fail("Invalid JSON");
      }
      if (!body || String(body.index) !== String(ov5647.index)) return fail("Unknown camera index");
      if (body.reset === true) {
        lensState.center = { ...OV5647_DEFAULT_CENTER };
        lensState.source = "default";
        lensState.distortion = null;
      } else {
        const center = body.center;
        const a = OV5647_SENSOR.activeArea;
        if (!center || !isNum(center.x) || !isNum(center.y)) return fail("center.x / center.y must be numbers");
        if (center.x < a.x || center.x > a.x + a.width || center.y < a.y || center.y > a.y + a.height) {
          return fail("center is outside the active area");
        }
        const d = body.distortion;
        if (d !== null && d !== undefined) {
          const keys = ["fx", "fy", "k1", "k2", "p1", "p2", "k3"];
          if (!keys.every((k) => isNum(d[k]))) return fail("distortion needs numeric fx, fy, k1, k2, p1, p2, k3");
          if (d.fx <= 0 || d.fy <= 0) return fail("fx and fy must be positive");
        }
        lensState.center = { x: center.x, y: center.y };
        lensState.source = "calibrated";
        lensState.distortion = d ? { fx: d.fx, fy: d.fy, k1: d.k1, k2: d.k2, p1: d.p1, p2: d.p2, k3: d.k3 } : null;
      }
      // Like the firmware: the stream restarts with the new readout window.
      frameVersion++;
      refreshOv5647();
      res.setHeader("Content-Type", "text/plain");
      res.end("OK");
    },
  },

  {
    pattern: "/api/get_camera_info",
    method: "GET",
    handle: (req, res) => {
      res.setHeader("Content-Type", "application/json");
      res.end(
        JSON.stringify({
          cameras: mockCameras,
        })
      );
    },
  },

  {
    pattern: "/api/capture_image",
    method: "GET",
    handle: (req, res) => {
      const url = new URL(req.url!, `http://${req.headers.host}`);
      const source = url.searchParams.get("source");

      if (!source) {
        res.statusCode = 400;
        res.end("Missing source parameter");
        return;
      }

      const camera = mockCameras.find((cam) => cam.index.toString() === source);
      if (!camera) {
        res.statusCode = 404;
        res.end("Camera not found");
        return;
      }

      res.statusCode = 302;
      res.setHeader(
        "Location",
        camera === ov5647
          ? `/api/mock_frame?index=0&v=${frameVersion}`
          : `https://picsum.photos/${camera.currentResolution.width}/${camera.currentResolution.height}`
      );
      res.end();
    },
  },

  {
    pattern: "/api/capture_binary",
    method: "GET",
    handle: (req, res) => {
      const url = new URL(req.url!, `http://${req.headers.host}`);
      const source = url.searchParams.get("source");

      if (!source) {
        res.statusCode = 400;
        res.end("Missing source parameter");
        return;
      }

      const camera = mockCameras.find((cam) => cam.index.toString() === source);
      if (!camera) {
        res.statusCode = 404;
        res.end("Camera not found");
        return;
      }

      const mockBinaryData = Buffer.from(
        `Mock binary data for camera ${source}`,
        "utf-8"
      );
      res.setHeader("Content-Type", "application/octet-stream");
      res.setHeader(
        "Content-Disposition",
        `attachment; filename="camera_${source}_raw.bin"`
      );
      res.end(mockBinaryData);
    },
  },

  {
    pattern: "/api/set_camera_config",
    method: "POST",
    handle: (req, res) => {
      let body = "";
      req.on("data", (chunk) => {
        body += chunk.toString();
      });

      req.on("end", () => {
        try {
          const config = JSON.parse(body);
          const { index, image_format, jpeg_quality } = config;

          const camera = mockCameras.find(
            (cam) => cam.index.toString() === index.toString()
          );
          if (!camera) {
            res.statusCode = 404;
            res.setHeader("Content-Type", "application/json");
            res.end(JSON.stringify({ error: "Camera not found" }));
            return;
          }

          if (image_format !== undefined) {
            camera.currentImageFormat = image_format;
            const format = camera.imageFormats.find(
              (f) => f.id === image_format
            );
            if (format) {
              camera.currentImageFormatDescription = format.description;
            }
          }

          if (jpeg_quality !== undefined) {
            camera.currentQuality = jpeg_quality;
          }

          if (camera === ov5647) {
            frameVersion++;
            refreshOv5647();
          } else {
            updateCameraSrc(camera);
          }

          res.setHeader("Content-Type", "application/json");
          res.end(
            JSON.stringify({
              success: true,
              message: "Camera configuration updated successfully",
            })
          );
        } catch (error) {
          console.error(error);
          res.statusCode = 400;
          res.setHeader("Content-Type", "application/json");
          res.end(JSON.stringify({ error: "Invalid JSON in request body" }));
        }
      });
    },
  },
];
