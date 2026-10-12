# Image formats and framing (OV5647 and IMX708)

The web UI's **Image size / format** selector changes the sensor capture mode of whichever camera is connected (OV5647 or IMX708, detected at boot). It is separate from the size at which the browser displays the preview. The firmware reads the enabled modes from the sensor driver at runtime, reports them through its camera-info API, and builds the selector from that list.

## OV5647

### Enabled capture modes

| Sensor output | Pixel format | Sensor frame rate | Notes |
|---|---:|---:|---|
| 800 x 1280 | RAW8 | 50 fps | Portrait mode |
| 800 x 640 | RAW8 | 50 fps | Landscape mode |
| 800 x 800 | RAW8 | 50 fps | Square mode |
| 1920 x 1080 | RAW10 | 30 fps | 16:9 mode |
| 1280 x 960 | RAW10 | 45 fps | 4:3 binning mode; default and widest tested framing |

The binned modes read most of the sensor; the share of the 2592 x 1944 active area each mode shows is reported as `fieldOfView` (see [Field of view](#field-of-view-coverage)): 1280 x 960 98.8 % x 98.8 %, 800 x 800 61.7 % x 82.3 %, 800 x 640 61.7 % x 65.8 %, 1920 x 1080 74.1 % x 55.6 %, 800 x 1280 30.9 % x 65.8 %. **1280 x 960 is the OV5647's full-view mode**; its full-resolution 2592 x 1944 mode is not offered because the ESP32-P4 ISP takes at most 1920 x 1080.

RAW8 and RAW10 describe the sensor-to-ISP capture format. The browser stream is JPEG/MJPEG in every mode. The exact list depends on which modes are enabled in `sdkconfig.defaults` and supported by the OV5647 driver.

### Choosing a mode

Start with **1280 x 960** for the widest tested view and least zoomed-in framing. Use the other entries to compare portrait, square, 16:9, and smaller landscape sensor windows. The modes read different amounts of the sensor (1280 x 960, 800 x 640 and 800 x 800 are 2x2 binned, i.e. 2 sensor pixels per image pixel; 1920 x 1080 and 800 x 1280 are 1:1 crops), so the field of view changes between modes.

**All modes are centred on the lens centre and share one orientation.** The firmware rewrites each mode's readout window (sensor windowing only - no scaling or resampling of the stream) so the image centre lands on the stored lens centre, and every mode uses the 1280 x 960 mirror/flip setting. A point at the lens centre therefore stays at the image centre when switching modes; only the crop changes. The lens centre defaults to the centre of the sensor's active area and can be calibrated; 1280 x 960 uses almost the whole sensor and can only follow the lens centre by +-16 px horizontally / +-12 px vertically. Window tables, the coordinate system and the calibration procedure are in [lens-calibration.md](lens-calibration.md).

| Mode | Sensor pixels per image pixel | Sensor area read (default centre) |
|---|---:|---|
| 800 x 1280 | 1 | 800 x 1280 |
| 800 x 640 | 2 | 1600 x 1280 |
| 800 x 800 | 2 | 1600 x 1600 |
| 1920 x 1080 | 1 | 1920 x 1080 |
| 1280 x 960 | 2 | 2560 x 1920 |

The browser scales the returned image to fit the preview. **Preview size, CSS scaling, and preview rotation do not change the sensor's field of view.** The available sensor modes are defined by the OV5647 driver's mode tables, not by an arbitrary width/height entered in the GUI.

## IMX708 (Arducam B0310, 120° M12 lens)

Driver: `mushbraindave/esp_cam_sensor_imx` (see the README). The driver's five modes are the **same sensor readout**: the whole 4608 x 2592 array is read, 2x2-binned to 2304 x 1296, and a digital crop of the output size is taken out of it. The sensor cannot shrink that field and the P4's ISP takes at most 1920 x 1080, so a single frame cannot show the whole sensor; the firmware's sixth mode, **2304 x 1296 full view**, scans it in four tiles instead (see [Full field of view](#imx708-full-field-of-view)). Line and frame timing are identical. The driver's tables run every mode at 28 fps; the firmware shortens the frame length so every mode runs at **30 fps** (30.01, see [Frame rate and streaming](#frame-rate-and-streaming)) with the same exposure range; a smaller mode only saves bandwidth and encode time, and costs field of view. Nothing is scaled: a smaller mode is a narrower window, not the whole scene shrunk.

| Mode (index) | Pixel format | Sensor fps | Sensor area read (sensor px) | Share of binned field (w x h) | Est. FOV H x V (deg)* |
|---|---:|---:|---|---|---|
| 1920 x 1080 (0, default) | RAW10 | 30 | 3840 x 2160 | 83 % x 83 % | about 100 x 56 |
| 1280 x 720 (1) | RAW10 | 30 | 2560 x 1440 | 56 % x 56 % | about 67 x 38 |
| 1024 x 768 (2) | RAW10 | 30 | 2048 x 1536 | 44 % x 59 % | about 53 x 40 |
| 800 x 600 (3) | RAW10 | 30 | 1600 x 1200 | 35 % x 46 % | about 42 x 31 |
| 640 x 480 (4) | RAW10 | 30 | 1280 x 960 | 28 % x 37 % | about 33 x 25 |
| **2304 x 1296 full view (5)** | RAW10 | 30 (7.5 images/s) | 4608 x 2592 (all) | 100 % x 100 % | about 120 x 68 |

\* **Estimate, pending measurement.** The module is specified as 120° horizontal / 152.2° diagonal over the full 4608 x 2592 sensor (6.45 x 3.63 mm). The figures assume an f-theta (equidistant) lens scaled to that 120° (about 18.6° per mm on the sensor). The real lens is strongly barrel-distorted, so treat them as +-10 %. Even the widest mode sees roughly 100° of the 120°: the outer ~17 % of the binned width and height is cropped away. Measure the real value with the lens-calibration tools, but read the note on fisheye models in [lens-calibration.md](lens-calibration.md) first.

All IMX708 modes are centred on the stored lens centre too: the firmware moves the digital crop, in 4-sensor-px steps, inside the 2304 x 1296 binned field. At the default centre each mode is the centred crop, identical to the driver's own tables. 1920 x 1080 can follow the lens centre by +-384 sensor px horizontally and +-216 vertically; smaller modes have more room.

Measured on hardware (ESP32-P4-Nano, Ethernet through Windows ICS): every mode applies through `POST /api/set_camera_config`, returns JPEGs of the right size, and each smaller mode is exactly the centred crop of the 1920 x 1080 frame (image registration, 0 px offset). The sensor runs at 30 fps (`X-Framerate: 30`) and the browser receives 30 fps in every mode; see [Frame rate and streaming](#frame-rate-and-streaming) for the measurements and the JPEG quality used at 1920 x 1080.

### IMX708 full field of view

**What was tried on the sensor (all failed, measured on hardware 2026-10-12).** Each candidate was built as a RAM copy of the driver's 1920 x 1080 table (`main/imx708_modes.c`, the driver is not modified), streamed, and registered against a 1920 x 1080 capture of the same scene (SIFT + RANSAC). The 1920 x 1080 crop has 2 sensor px per image px; the "scale" below is reference px per test px, so 1.0 means the same sampling as the crop (nothing shrunk), 0.5 means full resolution (1 sensor px per px) and a working 4x reduction would show 2.0.

| Attempt | Registers | Result |
|---|---|---|
| CCS scaler, M = 24 (1.5x down) on the full 2304 x 1296 field -> 1536 x 864 | 0x0401 = 2, 0x0405 = 24 | Writes do not stick (read back 0 / 16, both streaming and in standby). Image scale 1.0000 against the 1080 crop (1292 SIFT inliers): no scaling; the frame is the top-left 1536 x 864 of the field. Confirms the driver changelog. |
| 2x2 binning + 2x subsampling -> 1152 x 648 | 0x0383 = 0x0387 = 3 | Registers read back, but scale 1.0001: subsampling ignored in binned readout. |
| No binning + 4x subsampling -> 1152 x 648 | 0x0900 = 0, 0x0383 = 0x0387 = 7 | Scale 0.4999: full-resolution readout, no subsampling; quad-Bayer colour checkerboard. |
| Other binning factors | 0x0901 = 0x44, 0x12, 0x21, 0x24, 0x42, 0x41, 0x14, 0x33 (0x44 also with 0x3200/0x3201 = 0x42 / 0x44 / 0x81) | All scale 0.50: any value other than 0x22 reads the array unbinned. |
| Wider / taller crops through the ISP | 2048 x 1152, 1920 x 1296 | Rejected when streaming starts: `ISP_LSC: invalid h_res or v_res`; the IDF HAL limits the ISP to `ISP_LL_HSIZE_MAX` 1920 x `ISP_LL_VSIZE_MAX` 1080. |

So on this chip 1920 x 1080 (83 % x 83 %) is the largest single frame. True-RAW ISP-bypass capture delivers no frames (see the ISP debug API), so the ISP cannot be sidestepped either.

**What works: a 4-tile scan.** The digital-crop offset (0x0408-0x040B) can be changed while streaming and takes effect cleanly at a frame boundary (a live 320-px move appeared from one frame to the next, no torn frame). Mode 5 runs the sensor at a 1168 x 664 crop of the binned field (a quarter plus a 16-px overlap, so the ISP's border pixels are never used) and moves it to the next quarter after every frame (group hold 0x0104, ~2.5 ms of SCCB writes). A write after frame k lands in frame k+2 (with the latency set to 1 every quarter was pasted one place off). The firmware only trusts a frame's tile when the write was made in time after a fresh frame; otherwise the frame is skipped. The used quarter of each frame is copied by the PPA (2D-DMA, 26-42 ms, overlapped with the next frame) into a 2304 x 1296 canvas, and every completed canvas is JPEG-encoded (42 ms) by a separate task. AE/AWB see the mean statistics of the last four frames, i.e. the whole scene.

Result: **2304 x 1296, 100 % of the sensor at the binned resolution (2 sensor px per image px), 7.5 images/s** at the 30 fps sensor rate (sensor / 4).

- Geometry: each quarter of the output registered against the 1080 crop at scale 1.0000 and offset (-192.1, -108.0), (-192.0, -107.7), (-192.1, -107.9), (-191.9, -107.9) px against the predicted (-192, -108): the tiles are placed to 0.3 px.
- Seams: the mean step across the tile joins is 5.8 / 7.6 grey levels against 3.9 / 5.2 between neighbouring columns / rows - visible only on close inspection in a still scene. Moving objects show the scan: the four quarters are taken 33 ms apart (an image spans 100 ms of sensor time).
- Colour and exposure: same pipeline as the other modes (WB in LSC, black level in gamma); AE settled at its target (luma 46.9 for 46).
- Rate and size (scene of the other tables): quality 70 / 80 / 90 / 95: 297 / 401 / 670 / 1122 KB, all 7.5 fps, regular 133 ms gaps; the link carries even q95 (8.3 MB/s). Default **80**; quality is capped at **95** in this mode (its JPEG slots hold 1 B/px; quality 100 overflows them, and an overflowing hardware encode was twice followed by a crash in the ISP AWB interrupt).
- Robustness: 5-minute soak at q95: 7.5 fps throughout, about one tile per 1000 frames could not be trusted (scheduling hiccup), which drops one image (267 ms gap). Repeated switching to and from the mode: no failures. PSRAM: the two canvases take 12 MB; 7.8 MB stays free.
- Lens window: the whole sensor, `scale` 2; its centre is fixed, so `centerRange` is a point and `centerError` is the sensor centre minus the lens centre (e.g. (-200, +100) with the lens centre at (2504, 1196)).
- `/api/capture_binary` returns the last composed canvas (RGB565, 2304 x 1296).

Image controls on the IMX708: brightness, contrast and saturation (ISP). There is no exposure-target control (on this sensor the exposure value is the raw integration time, owned by the ISP pipeline's auto-exposure) and no automatic-brightness switch; the API omits both.

### IMX708 image quality (colour pipeline)

With the community component's tuning as shipped, the IMX708 gave magenta highlights, a green/yellow cast (fully green when a white object filled the frame), and a lifted, washed-out picture. All three were traced on hardware with the ISP debug API (below) to the ESP32-P4's ISP before rev 3.0 (this board is v1.3) together with that tuning. The fixes are IMX708-only, in `main/isp_ipa_hook.c` (Kconfig `EXAMPLE_IMX708_COLOUR_PIPELINE`, default on) and the project's own tuning file `main/tuning/imx708_b0310.json`.

| Fault | Root cause (measured) | Fix |
|---|---|---|
| Magenta highlights | The P4 < v3.0 ISP has no white-balance block, so `esp_video` multiplies the WB gains into the CCM columns. Green clips first; the CCM then scales the unclipped red/blue by 1.6..3x and its negative terms subtract the over-range red/blue from green, so a clipped white comes out about (1, 0.6, 1). Test: the same WB moved to a pre-demosaic stage turned the light tubes white (magenta pixels 0.55 % -> 0.00 %) with the rest of the image unchanged; 1/2 or 1/4 exposure did not help, because the tubes stay clipped. | WB is applied in the ISP's **LSC** block (per-Bayer-channel gain before demosaic, output saturates like a real WB stage); the ISP's own WB stays at 1.0. |
| Lifted, purple blacks; washed out | The IMX708 outputs a 64/1023 black pedestal (register 0x0008, read-only) and P4 < v3.0 has no black-level block, so nothing removes it: 16/255 at the ISP input, times WB gains and CCM, lifted black to about 55/36/64 after gamma. | The pedestal is removed per channel inside the gamma LUT, where it is a constant vector: each channel's curve subtracts its offset and stretches back to full scale, and the CCM is pre-scaled so the net transform is exact and saturated pixels still map to white. The LUT points are placed per channel so one sits exactly on that channel's black (power-of-two steps, log-spaced in the toe). Effective pedestal 15.0/255 (the sensor's 15.9 less the ISP's internal truncations), set by keeping a white card neutral down a 1/1 to 1/8 exposure ladder. |
| Green / yellow cast | The tuning's AWB white-patch window (raw R/G 0.50-0.89, B/G 0.48-0.78, pedestal included) excludes real neutrals under the lab's LED lighting (white card: R/G 0.50, B/G 0.34 pedestal-free, needing gains 2.0 / 2.95). AWB balanced on whatever else fell inside (gains 1.56 / 1.78 -> yellow-green), its 0.5 minimum step froze that first estimate, and with the white cube filling the frame it found 199 < 1000 candidates and never ran (gains 1.0, green image). | New window (pedestal-free R/G 0.35-0.95, B/G 0.22-0.85, still rejects yellow/orange/green), 0.02 steps, 500 minimum. The hook gives the IPA pedestal-free, WB-divided statistics and maps its window into the domain the ISP measures in. |
| Brightness depends on luck; highlights blown | AE hold band 50..150 (AE did nothing anywhere in it), low-light-priority metering, and AE statistics including the pedestal. | Target 46 (18 % grey, pedestal-free) with a +-10 % band, high-light-priority metering. |
| Greenish shadows | The ISP's RGB565 output **truncates** (R/B lose 0..7 levels, G 0..3), so every tone is ~2 levels greener. | +4/+2/+4 in the IMX708 gamma output turns truncation into rounding. |

Other tuning changes in `imx708_b0310.json`: BT.709 tone curve instead of x^0.72. The x^0.72 curve also clipped every input above 240. BT.709's 4.5x toe, against sRGB's 12.9x, keeps 8-bit linear noise and residual black error from becoming coloured shadow noise. A milder CCM green row (-0.45, 1.70, -0.25), which pulls 0.15 x (R - G) out of green so oranges are not pushed towards yellow; neutrals are unchanged. Neutral contrast (128). 50 Hz anti-flicker by default (Kconfig `EXAMPLE_IMX708_ANTI_FLICKER_50HZ/60HZ/OFF`). The autofocus unit is removed, since the B0310 has no focus motor. The component's original file is kept as `main/tuning/imx708_default.upstream-0.4.0.json`.

Measured before/after: same scene, back-to-back app swaps, auto exposure, white cube as neutral reference. Masks were segmented from the "after" frame, and the scene was checked unchanged (gradient correlation 0.96).

| Metric | Before (component tuning) | After |
|---|---|---|
| White cube, lit (R/G, B/G) | 0.49, 0.20 (AWB never ran) | 1.015, 1.003 |
| White cube, shaded (R/G, B/G) | 0.46, 0.23 | 0.97, 0.99 |
| Yellow cable hue | 82 deg (green) | 50 deg |
| Orange box hue | 64 deg (yellow) | 32 deg |
| Darkest 3 % (RGB) | 28 / 36 / 28 | 8 / 1 / 9 |
| Luma 1st percentile / std-dev | 31 / 34 | 4 / 47 |
| Brightest 0.5 % (R/G, B/G) | 0.66, 0.45 | 1.01, 0.99 |

On an earlier scene, where AWB did run: the white card measured 0.91 / 0.73 before, with magenta light tubes and black 55/36/64. All five modes keep the pipeline (LSC grid sized per mode) and stay neutral at the centre within +-3 %. Frame rate was unchanged by the pipeline (28 fps sensor at the time, 14 fps stream at 1920 x 1080 with both firmwares, limited by the old encode/transfer loop; now 30 / 30, see [Frame rate and streaming](#frame-rate-and-streaming)). "After" JPEGs are about 20 % larger because there is more contrast.

Remaining limits / follow-ups:

- **CCM** is still the component's uncalibrated seed with one mild correction. A proper matrix needs a 24-patch colour chart photographed under the target light. Fit it on pedestal-free linear data, which the debug API can produce (WB 1, CCM identity, gamma linear), then put it in `acc.ccm.table`, adding entries per colour temperature if the lighting varies.
- **Lens colour shading.** Red falls about 5 % towards the frame edges relative to the centre on a white surface (R/G 1.015 at the centre, 0.95-0.97 at the edges); blue is flat within 2.5 %. The LSC block now carries the white balance as flat tables and could also carry a measured shading correction. That needs a flat field: a uniformly lit white sheet or diffuser filling the frame. A non-uniformly lit object can't serve.
- Very deep shadows are limited by the 8-bit linear ISP data and the RGB565 hand-off to the JPEG encoder. Moving the capture format to YUV422 would remove the RGB565 step for every sensor.

### ISP debug API

Kconfig `EXAMPLE_ENABLE_ISP_DEBUG_API` (default on) adds the following, on the control port only:

- `GET /api/debug/isp` reports:
  - AE: exposure, gain, limits, AGC on/off, the IPA's luma, 5x5 block luminance and histogram, and the tuning's AE target and metering.
  - AWB: the IPA's gains, the ISP WB, white-patch sums before and after the hook, and the window.
  - CCM: as tuned and as programmed.
  - Gamma: x and y points per channel.
  - Brightness, contrast, saturation and hue.
  - Module enables: BF, sharpen, demosaic, gamma, CCM, LSC (BLC is reported as absent on this chip).
  - IPA metadata update counters.
  - The IMX708 pipeline state: WB/LSC gains, black offsets, stretch, CCM scale.
  - The relevant sensor registers: IMX708 exposure 0x0202, analogue gain 0x0204, digital gains 0x020E-0x0215, pedestal 0x0008, frame length, orientation. OV5647 AEC/AGC/AWB/BLC registers.
  - The active tuning JSON.
- `POST /api/debug/isp` takes a JSON object; every key is optional:
  - `{"reset":true}` clears every override.
  - `"awb": {"red":r,"blue":b}` or `"auto"`.
  - `"ccm": [9 numbers]`, `"identity"` or `"auto"`.
  - `"gamma": {"gamma":g,"black":[r,g,b]}`, `{"curve":"srgb"|"bt709"}` or `"auto"`.
  - `"lsc": {"r","gr","gb","b"}` (flat gains) or `"auto"`.
  - `"ae": {"exposure_lines":n,"gain_index":i}` (manual) or `"auto"`.
  - `"ae_luma_scale"`: an AE target shift.
  - `"modules": {"bf"|"sharpen"|"demosaic"|"gamma"|"ccm"|"lsc"|"color": false|true}`.
  - `"imx708_pipeline": true|false` for A/B, and `"imx708_pedestal": 15.0`.
  - IMX708 test pattern: `"test_pattern"` and `"test_colour":[r,gr,b,gb]` (10-bit).
  - `"sensor_regs": [{"addr","value"}]` writes registers; `"read_regs": [addr, ...]` reads them.
- `POST /api/debug/imx708_mode` defines an experimental IMX708 mode (appended after the others) from the 1920 x 1080 table: `{"out":[w,h], "crop":[w,h], "binning":34, "x_odd_inc":1, "y_odd_inc":1, "scale_mode":0, "scale_m":16, "tiles":0, "regs":[[addr,val],...]}`; select it with `/api/set_camera_config` (switch to another mode before redefining it). `{"tile_latency":n}` sets the full-view scan's crop-write latency (default 2). Used for the full-field-of-view tests above.
- In `GET /api/debug/isp`, `ae.exposure_max_us` is the AE ceiling in force (it used to report the boot-time sensor range); `sensor_exposure_max_us` is the range the ISP pipeline uses and `frame_exposure_max_us` the longest exposure the current frame holds.
- `GET /api/debug/stream_stats` reports the MJPEG pipeline per stage over the last 5 s window (see [Frame rate and streaming](#frame-rate-and-streaming)); the same line is logged every 5 s while someone is streaming.
- `GET /api/debug/tcp_throughput?mb=20&chunk=32768&mem=psram|internal&nodelay=1` sends `mb` MB of filler over a plain socket: the raw TCP ceiling without the camera.
- `GET /api/debug/raw_bayer?skip=4` returns one frame of Bayer data. `X-Width` / `X-Height` / `X-Pixel-Format` headers; the MJPEG stream pauses for about 1 s. The ISP-bypassed RAW10 path delivers no frames on this chip and IDF combination, so the endpoint falls back to RAW8. RAW8 is taken from the ISP's RAW stage: before demosaic, WB, CCM and gamma, but after LSC and Bayer denoise. Its scale depends on the ISP state, so calibrate it with the test pattern before reading absolute levels; channel ratios and clipping are reliable. For an absolute linear view, use the stream with WB 1, CCM identity, gamma `{"gamma":1}`, and sharpen and colour off. The RGB path is exactly raw10 / 4.

## Frame rate and streaming

**Result** (IMX708, firmware defaults, 15 s per mode, measured with a client reading `:81/stream`; device-side figures from `/api/debug/stream_stats`; 100 Mbit/s Ethernet to a Windows laptop):

| Mode | Sensor fps | Stream fps (browser) | JPEG quality | Avg frame | Stream | Before: stream fps / frame / MB/s (q80, 28 fps sensor) |
|---|---:|---:|---:|---:|---:|---|
| 1920 x 1080 | 30.01 | 30.0 | 70 | 263 KB | 8.1 MB/s | 9.4 / 343 KB / 3.3 |
| 1280 x 720 | 30.01 | 30.0 | 80 | 146 KB | 4.5 MB/s | 20.0 / 145 KB / 3.0 |
| 1024 x 768 | 30.01 | 30.0 | 80 | 125 KB | 3.9 MB/s | 27.9 / 122 KB / 3.5 |
| 800 x 600 | 30.01 | 30.0 | 80 | 74 KB | 2.3 MB/s | 28.0 / 75 KB / 2.2 |
| 640 x 480 | 30.01 | 30.0 | 80 | 46 KB | 1.4 MB/s | 28.0 / 45 KB / 1.3 |

No duplicate or corrupt frames; inter-frame gap 33.3 ms mean, max 34-42 ms. Frame sizes depend on the scene (an indoor scene here); the 1920 x 1080 default has headroom for about 25 % busier scenes.

**Sensor rate.** The IMX708 driver's tables use a frame length of 2672 lines (28.0 fps). After every mode change the firmware sets it to 2494 lines through the driver's live frame-length control: 7824 px x 2494 lines at 585.6 MHz = 33.32 ms = 30.01 fps (`CONFIG_EXAMPLE_IMX708_FRAME_RATE`, default 30; the length is rounded down so the rate is never below the setting). Only VTS changes, so the readout window and the lens-centred crop are untouched. The longest exposure becomes 2446 lines = 32.7 ms, and the auto-exposure ceiling is set to whole mains-flicker periods inside it: 30 ms with 50 Hz anti-flicker (25 ms at 60 Hz). With 50 Hz anti-flicker AE could only use 30 ms at 28 fps as well, so no exposure range is lost. `X-Framerate` and `currentFrameRate` report the live rate (30). The OV5647 keeps its own mode timing (30-50 fps).

**Pipeline.** A capture task per camera dequeues each V4L2 frame, JPEG-encodes it straight out of the V4L2 buffer into one of three output buffers, and re-queues the V4L2 buffer. The stream handler always sends the newest finished JPEG and skips older ones, so a slow client drops frames instead of stalling the sensor. Each part goes out as two `send()` calls (boundary + part header with `MSG_MORE`, then the JPEG) on a raw, non-chunked response with `TCP_NODELAY`; the connection closes when the stream ends. `/api/capture_image` takes the next frame finished after the request from the same ring, and `/api/capture_binary` gets a copy of the next unencoded frame. Encoding only runs while someone is watching or capturing. The stream port still serves one client at a time.

**Per-stage timing at 1920 x 1080** (ms per frame, average / max):

| | DQBUF wait | JPEG encode | Send | Frame | Delivered |
|---|---:|---:|---:|---:|---:|
| Before (one serial loop, q80, 28 fps) | 28.0 / 32.4 | 29.9 / 31.8 | 47.5 / 50.1 | 285 KB | 9.4 fps |
| After, q80 (30 fps) | 2.4 / 4.1 | 30.7 / 34.0 | 34.6 / 37.7 | 351 KB | 28.8 fps |
| After, q70 default (30 fps) | 2.5 / 4.4 | 30.7 / 33.6 | 25.2 / 27.2 | 263 KB | 30.0 fps |

Before, the three stages ran one after the other (DQBUF, encode, three chunked writes), so their times added up. Now encode and send overlap, and the slowest stage alone sets the rate.

**P4 limits at 1920 x 1080, 30 fps:**

- MIPI CSI: 2 lanes x 900 Mbit/s. A 1920-px RAW10 line is 19.2 kbit per 13.36 us line = 1.44 Gbit/s while lines are active. The line rate is set by the line length, so 30 fps does not change it; the average is 0.62 Gbit/s.
- ISP: 62 Mpx/s. The driver author measured every mode clean at 56 fps (640 x 480 tears from 48 fps).
- PSRAM (200 MHz, x16): about 124 MB/s RGB565 written by the ISP, 124 MB/s read by the JPEG encoder, plus JPEG output and network reads, about 265 MB/s in total. That is not a limit, but the contention shows: encode 29.9 to 30.7 ms, raw TCP 11.8 to 11.1 MB/s when the data is not in cache.
- **JPEG encoder: about 14.5 ns per pixel (69 Mpx/s; RGB565 in, 4:2:2 out).** That gives 30-31 ms at 1920 x 1080, at most about 32 fps, and only just fits 30. The other modes need 13.4 / 11.6 / 6.7 / 4.3 ms, with ample margin. Above roughly quality 85 the encoder also slows with output size (33.8 ms at q90). The capture task runs at priority 16, above the EMAC RX task, so the encoder is restarted the moment a frame finishes. At priority 7 it averaged 33.2 ms and capture fell behind the sensor (29.9 of 30.1 fps).
- **Ethernet: 100 Mbit/s.** Raw TCP measures 11.8 MB/s (the wire maximum is 11.86) from a cache-resident buffer and 11.1 MB/s from PSRAM. While the encoder runs, about 10.2-10.4 MB/s of JPEG gets through, so 30 fps at 1920 x 1080 needs frames of 340 KB or less. Before the lwIP/EMAC tuning (below) the ceiling was 6.35 MB/s.

**JPEG quality at 1920 x 1080 (30 fps sensor, this scene):**

| Quality | Avg frame | Stream fps | MB/s |
|---:|---:|---:|---:|
| 90 | 567 KB | 17.9 | 10.4 |
| 85 | 424 KB | 23.6 | 10.3 |
| 80 | 351 KB | 28.8 | 10.3 |
| 75 | 296 KB | 30.0 | 9.1 |
| **70 (default)** | 263 KB | 30.0 | 8.1 |
| 65 | 236 KB | 30.0 | 7.2 |
| 60 | 214 KB | 30.0 | 6.6 |
| 50 | 183 KB | 30.0 | 5.6 |

1920 x 1080 therefore defaults to quality 70 (`CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY_FULL_HD`). Quality 75 also reaches 30 fps on this scene but has only ~12 % headroom, and frame sizes at a fixed quality varied by ~25 % during testing. Every smaller mode keeps quality 80 (`CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY`); 1280 x 720 still reaches 30 fps at quality 90 (238 KB, 7.3 MB/s). The default is per mode, reported as each format's `quality.default`. The web UI's quality slider starts there when you pick the mode and shows why. You can move the slider: above the default, 1920 x 1080 trades frame rate for quality as in the table. The OV5647's 1920 x 1080 mode gets the same default.

**Network settings** (`sdkconfig.defaults`, `sdkconfig.defaults.esp32p4`): 30 EMAC TX descriptors of 1536 B (default 10 x 512 B, about three full frames), Ethernet and lwIP TX/RX in IRAM, lwIP core locking (socket calls run lwIP in the caller instead of a tcpip-thread round trip), MSS 1460. Together they raised raw TCP from 6.35 to 11.8 MB/s. `CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER` is 3: with 2, an encode that ran long made the CSI receiver drop the next frame (22 of 28 fps captured at 1080p).

**Measuring.** `GET /api/debug/stream_stats` gives, per 5 s window: sensor fps (ISP statistics interrupts), captured / encoded / sent fps, frames not encoded because of a stream cap (`not_encoded_cap`), frames a capped client passed over (`client_cap_skipped`), DQBUF wait, encode, client wait and send times (average / max), JPEG size, MB/s, frames dropped for lack of a free buffer, and frames a client skipped. Each part also carries `X-Timestamp`, the time the frame left V4L2 in seconds since boot (now in microseconds; it used to print nanoseconds in a microsecond field).

## Field of view coverage

`GET /api/get_camera_info` reports for every format `"fieldOfView": {"coverage": {"horizontal": h, "vertical": v}, "method": m}`: the share of the sensor's active area (OV5647 2592 x 1944, IMX708 4608 x 2592) the mode shows at the current lens centre, and how (`crop`: a window of the binned field, `binned`: OV5647 2x2-binned window, `tiled`: IMX708 4-frame scan). The UI's format selector shows it, e.g. "1920x1080 RAW10 @30 fps · 83% of view (crop)" and "2304x1296 RAW10 @7.5 fps · 100% of view (full sensor, 4-frame scan)".

| Sensor | Mode | Horizontal | Vertical | Method |
|---|---|---:|---:|---|
| IMX708 | 2304 x 1296 full view | 100 % | 100 % | tiled |
| IMX708 | 1920 x 1080 | 83.3 % | 83.3 % | crop |
| IMX708 | 1280 x 720 | 55.6 % | 55.6 % | crop |
| IMX708 | 1024 x 768 | 44.4 % | 59.3 % | crop |
| IMX708 | 800 x 600 | 34.7 % | 46.3 % | crop |
| IMX708 | 640 x 480 | 27.8 % | 37.0 % | crop |
| OV5647 | 1280 x 960 | 98.8 % | 98.8 % | binned |
| OV5647 | 800 x 800 | 61.7 % | 82.3 % | binned |
| OV5647 | 800 x 640 | 61.7 % | 65.8 % | binned |
| OV5647 | 1920 x 1080 | 74.1 % | 55.6 % | crop |
| OV5647 | 800 x 1280 | 30.9 % | 65.8 % | crop |

## Frame-rate limits (sensor and stream)

Two optional caps per camera, set in the settings panel (Camera settings: "Sensor frame rate limit" and "Stream frame rate limit", each with a "No limit" switch, staged with Save) or with `POST /api/set_camera_config` (`"sensor_fps_cap"` / `"stream_fps_cap"`: a number 1..120 or `null` = no limit; absent = unchanged). Both are stored in NVS per sensor (namespace `cam_cfg`, key `fps_<sensor>`, e.g. `fps_imx708`), applied at boot and after every mode change.

- **Sensor cap**: the sensor runs at min(cap, mode maximum). IMX708: frame length through the driver's live control, rounded up so the rate never exceeds the cap (15 -> 4990 lines = 14.999 fps); no stream restart. OV5647: VTS (0x380E/0x380F) in the RAM copy of the mode table, ceil(VTS x fps / cap), HTS unchanged, the AEC maximum exposure (0x3A02/03, 0x3A14/15) set to the new VTS - 4, and the driver recomputes the banding steps / max bands from the new VTS; the mode is re-applied (the stream restarts). The AE ceiling follows the longer frame in whole flicker periods (IMX708, 50 Hz: 30 ms at 30 / 25 fps, 60 ms at 15, 90 ms at 10). esp_video reads the sensor's exposure range only once, at boot; the firmware updates that range with the frame (`isp_ipa_hook.c`), otherwise AE could not go past the boot-time 35 ms.
- **Stream cap**: the MJPEG sender sends at most N images per second, taking frames on an N-per-second grid of their timestamps (half a frame of tolerance), so they are dropped evenly. The capture task does not encode frames that no client is due for (`/api/capture_image` always gets the next frame).

API: `GET /api/get_camera_info` reports `"frameRate": {"sensorCap", "streamCap", "sensorFps", "modeMaxFps", "framesPerImage", "imageFps", "streamFps", "capMin"}` (caps `null` when off; `sensorFps` is the actual rate; `imageFps` = sensorFps / framesPerImage; `streamFps` = what a client receives) and per format `"maxFps"` (the mode's top sensor rate, the cap's range) and `"framesPerImage"` (4 for the full-view scan). `currentFrameRate` is the actual sensor rate; the stream's `X-Framerate` header is the rate the client will receive (one decimal). The UI shows the actual sensor rate and the rate it measures on its own preview.

**Measured (IMX708, 1920 x 1080 q70 unless noted; 10-12 s per case read from `:81/stream`, device figures from `/api/debug/stream_stats`; no duplicate frames in any case):**

| Sensor cap | Stream cap | Sensor fps | Client fps (timestamps) | Frame gaps (ms) | Encoded fps | AE ceiling |
|---|---|---:|---:|---|---:|---:|
| - | - | 30.01 | 30.0 | 33.3 mean | 30 | 30 ms |
| 25 | - | 25.0 | 25.0 | 39.9-40.1 | 25 | 30 ms |
| 15 | - | 15.0 | 15.0 | 66.6-66.8 | 15 | 60 ms |
| 10 | - | 10.0 | 10.0 | 99.9-100.1 | 10 | 90 ms |
| - | 5 | 30.01 | 5.0 | 199.8-199.9 | 4.97 | 30 ms |
| - | 15 | 30.0 | 15.0 | 66.6-66.7 | 15.07 | 30 ms |
| - | 25 | 30.0 | 25.0 | 29-67 (5 of 6 frames) | 25.1 | 30 ms |
| 15 | 5 | 15.0 | 5.0 | 199.8-200.0 | 5.0 | 60 ms |
| full view, - | - | 30.0 | 7.5 | 133.1-133.5 | 7.5 | 30 ms |
| full view, 15 | - | 15.0 | 3.75 | 266.7 | 3.8 | 60 ms |
| full view, - | 5 | 30.0 | 4.98 | 133-267 | 4.97 | 30 ms |
| full view, - | 2 | 30.0 | 2.0 | 400-533 | 2.0 | 30 ms |

AE with a cap: with the scene's AE already settled the IPA keeps its exposure/gain split (30 ms, gain 7.5) when the ceiling rises; when it needs more light it uses the longer frame - forcing that (AE statistics scaled by 0.5 through the debug API) gave 90 ms at gain 5.7 with a 10 fps cap, against 30 ms at the 14.2 gain limit without. Caps were restored from NVS after a reboot (log `frame-rate caps "fps_imx708": sensor 15, stream 10`; the AE ceiling is applied once the ISP pipeline runs) and kept across switches between 1920 x 1080, 1280 x 720 and the full view. `/api/capture_image` during a 10 fps-capped stream took 100 ms and did not disturb the stream.

## Switching while streaming

Changing the sensor format requires stopping capture and recreating the capture/encoder buffers. The firmware asks the active MJPEG handler to leave its frame loop, stops the stream server, applies the requested sensor mode, and starts the server again. A connected preview can pause or reconnect briefly while this happens. This orderly shutdown avoids freeing buffers while an HTTP stream is still using them.

## Build-time options

The shared defaults currently enable these OV5647 modes:

```ini
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X640_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X1280_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW8_800X800_50FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS=y
CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS=y
CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1280X960_BINNING_45FPS=y
```

These are sensor capture modes, not preview-size presets. After changing enabled modes, rebuild and flash the firmware; the GUI repopulates the selector from the modes the running firmware reports.

For the IMX708 (`sdkconfig.defaults.esp32p4`):

```ini
CONFIG_CAMERA_IMX708=y
CONFIG_CAMERA_IMX708_AUTO_DETECT_MIPI_INTERFACE_SENSOR=y
CONFIG_CAMERA_IMX708_MIPI_IF_FORMAT_INDEX_DEFAULT=0   # boot mode: 1920 x 1080
CONFIG_CAMERA_IMX708_LINK_FREQ_450MHZ=y               # 447/450/453 MHz only move EMI; timing is identical
CONFIG_CAMERA_IMX219=n
CONFIG_CAM_MOTOR_DW9807=n                             # the B0310 has no autofocus motor
CONFIG_EXAMPLE_IMX708_HMIRROR=y                       # sensor orientation, see lens-calibration.md
CONFIG_EXAMPLE_IMX708_VFLIP=y
CONFIG_EXAMPLE_IMX708_COLOUR_PIPELINE=y               # WB in LSC + black-level removal (image quality section)
CONFIG_EXAMPLE_IMX708_ANTI_FLICKER_50HZ=y             # or _60HZ / _OFF: mains frequency for AE anti-flicker
CONFIG_EXAMPLE_ENABLE_ISP_DEBUG_API=y                 # /api/debug/isp, raw_bayer, stream_stats, tcp_throughput
```

Frame rate and streaming (`main/Kconfig.projbuild` defaults, see [Frame rate and streaming](#frame-rate-and-streaming)):

```ini
CONFIG_EXAMPLE_IMX708_FRAME_RATE=30                   # IMX708 sensor rate in every mode (frame length 2494 lines); the top of the sensor cap
CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY=80            # default JPEG quality below 1920 x 1080
CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY_FULL_HD=70    # default JPEG quality at 1920 x 1080
CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER=3           # V4L2 frame buffers (4 MB each at 1080p)
```

The IMX708 ISP tuning file is `main/tuning/imx708_b0310.json` (registered in the top-level `CMakeLists.txt`; `-DIMX708_IPA_JSON=<path>` builds with another one, e.g. the upstream copy for an A/B comparison).

## Related controls

The settings dialog also exposes JPEG quality (starting at the selected mode's default: 70 at 1920 x 1080, 80 otherwise, including the full view), the sensor and stream frame-rate limits, brightness, contrast and saturation and, when the active sensor supports them (OV5647), the exposure target and an automatic-brightness toggle. Preview rotation is saved in that browser's local storage and only affects the displayed preview.
