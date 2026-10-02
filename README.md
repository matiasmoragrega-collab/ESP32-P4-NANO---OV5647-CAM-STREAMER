# ESP32-P4 Nano OV5647 Camera Streamer

Firmware for the Waveshare ESP32-P4-Nano and Waveshare RPi Camera G (OV5647). The camera connects to the Nano's two-lane MIPI-CSI connector. The board captures frames, encodes them as MJPEG, and serves the live preview over Ethernet.

## View the camera

After boot and Ethernet link-up, open `http://esp-web.local/`. The direct MJPEG endpoint is `http://esp-web.local:81/stream`. If `.local` names do not resolve on your computer, use the IPv4 address printed in the USB-C serial log.

For a direct Ethernet connection to a laptop, configure the laptop Ethernet adapter to provide DHCP/network sharing to the Nano. The Nano obtains its address by DHCP. USB-C is used for power, firmware flashing, and serial logs; the camera stream runs over Ethernet.

The web interface lets you select the **sensor image format**, JPEG quality, preview rotation, exposure target, brightness, and automatic brightness where supported. Preview rotation is a browser display setting. It does not rotate the camera sensor or the network stream. See [docs/camera-formats.md](docs/camera-formats.md) for the available sensor modes and their effect on framing.

## Hardware configuration

- Board: Waveshare ESP32-P4-Nano
- Camera: Waveshare RPi Camera G, OV5647, MIPI CSI-2, two lanes
- Sensor control bus: SCCB/I2C on SDA GPIO7 and SCL GPIO8
- Network: on-board IP101 Ethernet PHY and RJ45
- Video: sensor capture is converted to JPEG and streamed as multipart MJPEG

The firmware is based on Waveshare's `17_simple_video_server` example and Espressif's `esp_video` and OV5647 sensor components. Component versions are pinned in `dependencies.lock`; ESP-IDF downloads managed components during configuration/build.

## Build and flash

Install ESP-IDF 5.5 with the ESP32-P4 toolchain and open an ESP-IDF PowerShell terminal. From this repository root:

```powershell
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the Nano's USB-C serial port. `sdkconfig.defaults` enables Ethernet and the five OV5647 capture modes. The generated `sdkconfig` is intentionally ignored; use `idf.py menuconfig` for local overrides and update `sdkconfig.defaults` when a change should be shared.

The browser UI is embedded in the firmware as compressed files under `frontend/gzipped/`. If you edit the UI under `frontend/src/`, rebuild and refresh those files from the `frontend` directory with the project's pnpm build/compress workflow before rebuilding the firmware.

## Known working camera setting

The latest hardware check used **1280 x 960 RAW10 at 45 fps** as the selected sensor capture mode. It gave the widest, least zoomed framing among the currently enabled modes. The browser receives JPEG frames at that capture size. Selecting a different mode changes the sensor window and can change the field of view; changing the browser preview dimensions alone does not change capture framing.

## Repository layout

- `main/`: camera initialization, HTTP API, MJPEG stream, and format reconfiguration
- `frontend/`: browser camera controls and embedded compressed UI assets
- `components/example_video_common/`: board and video helper code
- `sdkconfig.defaults`: shared ESP32-P4 camera/Ethernet configuration
- `dependencies.lock`: pinned ESP-IDF component versions

