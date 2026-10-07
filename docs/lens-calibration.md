# Lens centre and lens calibration

The firmware places every OV5647 sensor mode's readout window so that the centre of the
output image sits on the lens's optical centre. Nothing is scaled or resampled: the stream
is still the sensor's native pixels, only *which* pixels are read out changes. The lens
model (centre + distortion) is stored on the device and reported to the web UI, which uses
it for guides/display only; it is never applied to the stream.

## Sensor coordinates

All positions are **OV5647 pixel-array addresses in full-resolution pixels**, the address
space of the array-window registers 0x3800-0x3807, using a continuous *edge* convention:
array column `i` covers `[i, i+1)` (its centre is `i + 0.5`).

| Region | x | y | size |
|---|---|---|---|
| Whole array (incl. dark/dummy border) | 0 .. 2623 | 0 .. 1955 | 2624 x 1956 |
| Active (imaging) area | 16 .. 2607 | 6 .. 1949 | 2592 x 1944 |

The default lens centre (no calibration stored) is the active-area centre **(1312, 978)**.
A lens centre must lie inside the active area (x 16..2608, y 6..1950).

## Window model

For a mode with output size `out_w x out_h` and subsampling/binning scale `s` (2 for the
2x2-binned modes, 1 otherwise; derived from registers 0x3814/0x3815), the firmware reads an
**output region** of `out_w*s x out_h*s` sensor pixels:

```
window.x = clamp(round_to_even(cx - out_w*s/2), 16, 2608 - out_w*s  (rounded down to even))
window.y = clamp(round_to_even(cy - out_h*s/2),  6, 1950 - out_h*s  (rounded down to even))
```

* Start addresses are even, so the colour (Bayer) phase of the first pixel is the same in
  every mode and the ISP's GBRG setting stays correct. In 2x2-binned modes an even shift of
  2 sensor px is a 1-output-pixel shift, so binned modes can be positioned to 1 output px.
* The output region is kept inside the **active area**, so no dark border pixels are shown.
* The array window (0x3800-0x3807) is the output region grown by a symmetric margin of
  4 output px left/right and 2 output px top/bottom; the ISP offsets 0x3810-0x3813 skip
  exactly that margin. Because the margin is symmetric, the result does not depend on the
  side from which the sensor counts the ISP offset when mirroring (it counts from the
  mirrored side - verified on hardware).
* Output size, binning/subsampling and HTS/VTS (so frame rate and exposure timing) are not
  changed. The register table is copied to RAM and only the window/offset and orientation
  registers are rewritten.

**Mapping between output pixels and sensor coordinates** (edge coordinates on both sides):

```
X = window.mirror ? window.x + window.width  - u*window.scale : window.x + u*window.scale
Y = window.flip   ? window.y + window.height - v*window.scale : window.y + v*window.scale
```

With the unified orientation `mirror = true, flip = false`. The image centre
`(out_w/2, out_h/2)` always maps to the window centre `(window.x + width/2, window.y + height/2)`.
A lens centre `(cx, cy)` appears in the image at `u = (window.x + width - cx)/scale`,
`v = (cy - window.y)/scale` (subtract 0.5 for OpenCV pixel-centre coordinates).

`centerError` = achieved window centre - requested lens centre (0 or +-1 px when the mode
has room; larger when clamped). `centerRange` = the lens-centre values the mode can follow
exactly.

### Windows at the default centre (1312, 978)

| Mode | scale | output region (sensor) | array window (0x3800-07) | ISP offset | centre range x / y |
|---|---|---|---|---|---|
| 800 x 1280 RAW8 | 1 | x 912..1711, y 338..1617 | 908..1715 x 336..1619 | 4 / 2 | 416..2208 / 646..1310 |
| 800 x 640 RAW8 | 2 | x 512..2111, y 338..1617 | 504..2119 x 334..1621 | 4 / 2 | 816..1808 / 646..1310 |
| 800 x 800 RAW8 | 2 | x 512..2111, y 178..1777 | 504..2119 x 174..1781 | 4 / 2 | 816..1808 / 806..1150 |
| 1920 x 1080 RAW10 | 1 | x 352..2271, y 438..1517 | 348..2275 x 436..1519 | 4 / 2 | 976..1648 / 546..1410 |
| 1280 x 960 RAW10 | 2 | x 32..2591, y 18..1937 | 24..2599 x 14..1941 | 4 / 2 | 1296..1328 / 966..990 |

The 1280 x 960 mode reads 2560 x 1920 of the 2592 x 1944 active pixels, so it can only
follow the lens centre by +-16 px horizontally and +-12 px vertically; beyond that it is
clamped and `centerError` shows the residual. All smaller modes centre exactly (to +-1 px).

### Orientation

All modes use the 1280 x 960 orientation: sensor mirror on (0x3821 bit 1), no flip
(0x3820 bit 1), ISP mirror/flip bits (bit 2) off; binning bits are kept per mode. (The
stock tables already used this orientation in every mode - the register comments in the
driver are misleading - the firmware now enforces it.)

## Persistent calibration (NVS)

Namespace `lens_cal`, key `calib`: one blob holding version, `cx`, `cy` (double, sensor
coords), a distortion flag and `fx, fy, k1, k2, p1, p2, k3` (double). No key = defaults
(`source: "default"`). Reset erases the key.

## API

`GET /api/get_camera_info` adds to each camera: `sensor` (array size and active area),
`lensCalibration` (`center`, `source`, `distortion` or null) and `window` (the applied
window); each `imageFormats[]` entry gets the `window` that mode would use.

`POST /api/set_lens_calibration`

```json
{"index":0, "center":{"x":1340.5,"y":962.2}, "distortion":{"fx":1500,"fy":1497,"k1":-0.25,"k2":0.06,"p1":0.0008,"p2":-0.0012,"k3":-0.005}}
{"index":0, "center":{"x":1340.5,"y":962.2}, "distortion":null}
{"index":0, "reset":true}
```

The values are validated (finite numbers, centre inside the active area, fx/fy > 0), the
current mode is re-applied with the new window (the stream pauses briefly, JPEG quality and
image controls are kept) and then the calibration is saved. Response `OK`, or HTTP 400 with
a message (500 if the re-apply/save failed; the previous calibration is restored then).

## Distortion model

OpenCV 5-parameter model in sensor coordinates: `x = (X - cx)/fx`, `y = (Y - cy)/fy`,
`r^2 = x^2 + y^2`,
`x_d = x(1 + k1 r^2 + k2 r^4 + k3 r^6) + 2 p1 x y + p2 (r^2 + 2x^2)`,
`y_d = y(1 + k1 r^2 + k2 r^4 + k3 r^6) + p1 (r^2 + 2y^2) + 2 p2 x y`,
`X_d = cx + fx x_d`, `Y_d = cy + fy y_d` (distorted = what the sensor sees). `(cx, cy)` is
the stored lens centre. Image-space intrinsics measured in a mode convert as
`fx_sensor = fx_image * scale`, the principal point through the window mapping, and the
mirror flips the sign of `p2` (a flip would flip `p1`); `calibrate.py` does this.

## Calibration procedure

### 1. One-time setup (Windows)

There is no system Python; use the ESP-IDF one to create a venv at a **short path**
(long paths hit the 260-character limit and break the numpy/opencv install):

```powershell
C:\Espressif\tools\python\v5.5\venv\Scripts\python.exe -m venv C:\lensvenv
C:\lensvenv\Scripts\python.exe -m pip install -r tools\lens_calibration\requirements.txt
```

Optional self-test (renders a synthetic dataset with a known off-centre lens and checks
that it is recovered): `C:\lensvenv\Scripts\python.exe tools\lens_calibration\synth_test.py`

### 2. Print a checkerboard

Print a checkerboard with e.g. 10 x 7 squares (= **9 x 6 inner corners**), 20-30 mm
squares, on A4/A3 at 100 % scale. Glue it to something flat and rigid (foam board,
glass). Measure the square size if you want metric extrinsics (not needed for the lens
centre).

### 3. Capture ~15 varied shots in 1280 x 960

1. Select **1280 x 960** in the web UI (widest view; any mode works because the window is
   recorded, but use one mode per capture folder).
2. Fix the camera; keep the focus as it will be used.
3. Run

   ```powershell
   C:\lensvenv\Scripts\python.exe tools\lens_calibration\capture.py --host 192.168.137.x --out shots --board 9x6
   ```

   A preview window shows live detection; press **SPACE** to save, **Q** to quit
   (`--no-gui` uses ENTER instead; `--interval 3 --count 15` saves automatically).
4. Take 15-25 shots: board filling the frame, near each **corner and edge** (important for
   distortion), tilted +-30-45 degrees about both axes, at two or three distances. Avoid
   motion blur and glare; the whole board must be visible.

### 4. Calibrate and upload

```powershell
C:\lensvenv\Scripts\python.exe tools\lens_calibration\calibrate.py shots --board 9x6 --square 25 --upload 192.168.137.x
```

It prints the RMS reprojection error (aim for < 0.5 px; drop blurry shots with the worst
errors and rerun if higher), the lens centre in sensor coordinates with its uncertainty,
and the distortion, writes `shots/calibration.json` (exactly the POST body) and
`shots/calibration_report.json`, and with `--upload` posts it and prints the device's new
window and `centerError`. Without `--upload` you can post the file later, e.g.
`curl -X POST http://<host>/api/set_lens_calibration -d @shots/calibration.json`.
Use `--no-k3` if the shots do not reach the image corners.

Optional cross-check: photograph a uniformly lit white surface (flat field, e.g. a diffuser
over the lens) and run
`calibrate.py --vignetting flat.jpg --info shots/camera_info.json`. It fits a radial
brightness model and prints the falloff centre. Note that the sensor's lens-shading
correction (LENC, register 0x5000) flattens the falloff, so this estimate is only
meaningful if the falloff is still clearly visible; the tool warns when it is not.

### 5. Verify

* Re-read `/api/get_camera_info`: `lensCalibration.source` is `calibrated`; every mode's
  `window.centerError` is ~0 except 1280 x 960 if the centre is more than 16/12 px off.
* Cycle through all modes in the UI: an object at the lens centre must stay at the image
  centre in every mode (only the crop/zoom changes), and the image must not be mirrored or
  flipped between modes.
* To go back to the default: `POST /api/set_lens_calibration {"index":0,"reset":true}`.

## Hardware verification (development record)

Measured on the P4 Nano + OV5647 by SIFT/phase-correlation registration of every mode
against a 1280 x 960 capture, using the mapping above: residuals were <= 1.3 sensor px in all
modes, both at the default centre and with the centre moved by (+200, +100) px (windows moved
as computed; 1280 x 960 clamped at its +16/+12 limit). Colour ratios (G/R, G/B) match the
1280 x 960 capture in every mode except 800 x 1280, whose stock register table has a very low
gain ceiling and produces a dark, green-tinted image (same with the stock firmware; the Bayer
phase is correct - foliage stays green).
