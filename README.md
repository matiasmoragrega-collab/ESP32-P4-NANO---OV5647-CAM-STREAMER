# ESP32-P4 Nano Camera Streamer (OV5647 / IMX708)

Firmware for the Waveshare ESP32-P4-Nano with either a Waveshare RPi Camera G (OV5647) or an Arducam 12MP IMX708 120° wide-angle module. The camera connects to the Nano's two-lane MIPI-CSI connector. The board captures frames, encodes them as MJPEG, and serves the live preview over Ethernet.

One firmware supports both cameras: at boot `esp_video` probes the SCCB bus for every enabled sensor driver and uses whichever camera answers (OV5647 at I2C 0x36, IMX708 at 0x1a). The detected sensor is shown in the web UI and reported as `sensorName` by `GET /api/get_camera_info`. Swap cameras with the board powered off; the lens calibration is stored per sensor, so a swap never applies the other camera's lens centre.

## View the camera

After boot and Ethernet link-up, open `http://esp-web.local/`. The direct MJPEG endpoint is `http://esp-web.local:81/stream`. If `.local` names do not resolve on your computer, use the IPv4 address printed in the USB-C serial log.

For a direct Ethernet connection to a laptop, configure the laptop Ethernet adapter to provide DHCP/network sharing to the Nano. The Nano obtains its address by DHCP. USB-C is used for power, firmware flashing, and serial logs; the camera stream runs over Ethernet.

The web interface lets you select the **sensor image format** (each labelled with the share of the sensor it shows), JPEG quality, optional **sensor and stream frame-rate limits**, preview rotation, brightness, contrast, saturation, and (OV5647 only) the exposure target and automatic brightness. Controls the active sensor does not support are hidden. Preview rotation is a browser display setting. It does not rotate the camera sensor or the network stream. See [docs/camera-formats.md](docs/camera-formats.md) for the available sensor modes and their effect on framing.

## Hardware configuration

- Board: Waveshare ESP32-P4-Nano (ESP32-P4 rev v1.3, 16 MB flash, PSRAM)
- Supported cameras (MIPI CSI-2, two lanes; one at a time):
  - Waveshare RPi Camera G: OmniVision OV5647, 5 MP, 15-pin FFC.
  - Arducam 12MP IMX708 HDR 120° wide-angle module (SKU B0310): Sony IMX708, 4608 x 2592, 1.4 µm, RAW10, M12 lens 2.87 mm F2.8 (120° H / 152.2° D), **manual focus** (no autofocus motor), IR-cut filter. The module has a 22-pin connector; use Arducam's **15-pin to 22-pin** camera cable (15-pin end on the Nano).
- Sensor control bus: SCCB/I2C port 0 on SDA GPIO7 and SCL GPIO8; no reset/power-down/XCLK pins (both modules run from their own oscillator)
- Network: on-board IP101 Ethernet PHY and RJ45
- Video: sensor capture is converted to JPEG and streamed as multipart MJPEG

The firmware is based on Waveshare's `17_simple_video_server` example and Espressif's `esp_video` and OV5647 sensor components. The component manifest in `main/idf_component.yml` declares the compatible dependencies; ESP-IDF's Component Manager resolves and downloads them during configuration/build.

### IMX708 driver (third-party component)

Espressif's `esp_cam_sensor` has no IMX708 driver. IMX708 support comes from the community component [`mushbraindave/esp_cam_sensor_imx`](https://components.espressif.com/components/mushbraindave/esp_cam_sensor_imx) (0.4.x, by its contributors, licensed **Apache-2.0**; source: <https://github.com/mushBrainDave/esp32-p4-imx-camera>). It registers itself in `esp_cam_sensor`'s auto-detect list and ships an IMX708 ISP (esp_ipa) tuning file; this project registers its own tuning derived from it instead (`main/tuning/imx708_b0310.json`, upstream copy kept alongside). It was developed for the Raspberry Pi Camera Module 3 (IMX708 + DW9807 autofocus); this firmware leaves the DW9807 driver off because the Arducam B0310 has a fixed M12 lens. Project-specific notes:

- Sensor orientation: the firmware programs mirror + flip (Kconfig `EXAMPLE_IMX708_HMIRROR` / `EXAMPLE_IMX708_VFLIP`, both on = the Raspberry Pi convention for IMX708 modules) and passes the matching Bayer phase to `esp_video`.
- ESP-IDF 5.5.0 to 5.5.2 (and 5.4.0 to 5.4.3) need `ISP_AWB_WINDOW_X_NUM/_Y_NUM=5` so that `esp_video` and the prebuilt `esp_ipa` library agree on the ISP statistics layout; the top-level `CMakeLists.txt` adds them. Without them the IMX708's auto-exposure runs on garbage statistics.
- Image quality: the P4 (< rev 3.0) ISP has no black-level or white-balance block. For the IMX708, `main/isp_ipa_hook.c` applies the white balance in the ISP's LSC stage, so clipped highlights stay white instead of magenta, and removes the sensor's black pedestal in the gamma curve. Together with the project tuning (AWB window, AE target and metering, BT.709 curve, 50 Hz anti-flicker) this fixes the magenta highlights, the green/yellow cast and the washed-out look. Measurements, remaining limits (CCM calibration with a colour chart, lens colour shading) and the ISP debug API are in [docs/camera-formats.md](docs/camera-formats.md#imx708-image-quality-colour-pipeline). The OV5647 path is unchanged.
- The IMX708 streams in the driver's five 2x2-binned modes (1920 x 1080 default, down to 640 x 480), all crops of one 2304 x 1296 readout, plus a firmware **full-view mode**: 2304 x 1296, the whole sensor, at 7.5 images/s, built by scanning four quarters of the binned field (the sensor's scaler, subsampling and other binning factors do not work and the P4 ISP takes at most 1920 x 1080 - tested on hardware); see [docs/camera-formats.md](docs/camera-formats.md#imx708-full-field-of-view).

## Build and flash

Install ESP-IDF 5.5 with the ESP32-P4 toolchain and open an ESP-IDF PowerShell terminal. From this repository root:

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the Nano's USB-C serial port. `sdkconfig.defaults` and `sdkconfig.defaults.esp32p4` enable Ethernet, the five OV5647 capture modes and the IMX708 driver (five modes, 1920 x 1080 default, 450 MHz MIPI link). IMX708 anti-flicker defaults to **50 Hz** mains (UK/EU); choose 60 Hz or off under *Example Configuration* in `idf.py menuconfig` (`EXAMPLE_IMX708_ANTI_FLICKER_*`). The ISP debug API (`EXAMPLE_ENABLE_ISP_DEBUG_API`) is on by default; turn it off for a locked-down build. The generated `sdkconfig` is intentionally ignored; use `idf.py menuconfig` for local overrides and update `sdkconfig.defaults` when a change should be shared.

The browser UI is embedded in the firmware as compressed files under `frontend/gzipped/`. If you edit the UI under `frontend/src/`, rebuild and refresh those files from the `frontend` directory with the project's pnpm build/compress workflow before rebuilding the firmware.

## Known working camera settings

OV5647: the latest hardware check used **1280 x 960 RAW10 at 45 fps** as the selected sensor capture mode. It gave the widest, least zoomed framing among the currently enabled modes. The browser receives JPEG frames at that capture size. Selecting a different mode changes the sensor window and can change the field of view; changing the browser preview dimensions alone does not change capture framing.

IMX708: **1920 x 1080 RAW10 at 30 fps** (boot default) is the widest single-frame mode. It uses 83 % of the sensor width and height, so the 120° lens is cropped. For the **uncropped** view select **2304 x 1296 · 100 % of view (full sensor, 4-frame scan)**: the whole sensor at 7.5 images/s, with possible seams on fast-moving objects because its four quarters are 33 ms apart (see [docs/camera-formats.md](docs/camera-formats.md#imx708-full-field-of-view)). All six modes were checked on hardware.

### Frame rate

Every IMX708 mode runs the sensor at 30 fps. The browser receives 30 fps in every mode over the 100 Mbit/s Ethernet link: 1920 x 1080 at JPEG quality 70 (about 260 KB/frame, 8 MB/s), and the smaller modes at quality 80. Before this, the stream got 9-14 fps at 1920 x 1080 and 18-28 fps at 1280 x 720.

1920 x 1080 is bound by the link (about 10.3 MB/s of JPEG while encoding) and by the P4's JPEG encoder (about 30 ms of the 33 ms frame). Its default quality is therefore 70 instead of 80: quality 80 gives 351 KB frames and 28.8 fps. The quality slider in the settings starts at the mode's default and can be raised. The OV5647 keeps its own mode rates (30-50 fps).

**Frame-rate limits.** In the settings panel (or `POST /api/set_camera_config` with `sensor_fps_cap` / `stream_fps_cap`, a number or `null`) you can cap the sensor rate (the sensor runs slower; exposure can grow with the longer frame) and/or the stream rate (fewer images sent, the sensor keeps its rate). Both are saved per sensor and survive reboots; details and measurements in [docs/camera-formats.md](docs/camera-formats.md#frame-rate-limits-sensor-and-stream).

The stream port serves one viewer at a time. A second viewer waits until the first disconnects. Per-stage timing is available at `GET /api/debug/stream_stats`. Details, the quality/size table and the P4 limits are in [docs/camera-formats.md](docs/camera-formats.md#frame-rate-and-streaming).

## Repository layout

- `main/`: camera initialization, HTTP API, MJPEG stream, and format reconfiguration
- `main/imx708_modes.c`: extra IMX708 modes built in RAM (the full-view tile scan, the debug test mode)
- `main/isp_ipa_hook.c`, `main/isp_debug_api.c`: hook between `esp_video` and the IPA (IMX708 colour pipeline, debug overrides) and the `/api/debug/*` ISP debug API
- `main/tuning/`: IMX708 ISP tuning (`imx708_b0310.json`) and the upstream file it was derived from
- `frontend/`: browser camera controls and embedded compressed UI assets
- `components/example_video_common/`: board and video helper code
- `sdkconfig.defaults`: shared ESP32-P4 camera/Ethernet configuration
- `dependencies.lock`: pinned ESP-IDF component versions
