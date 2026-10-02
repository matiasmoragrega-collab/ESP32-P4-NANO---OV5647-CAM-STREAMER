# OV5647 image formats and framing

The web UI's **Image size / format** selector changes the OV5647 sensor capture mode. It is separate from the size at which the browser displays the preview. The firmware reads the enabled modes from the sensor driver at runtime, reports them through its camera-info API, and builds the selector from that list.

## Enabled capture modes

| Sensor output | Pixel format | Sensor frame rate | Notes |
|---|---:|---:|---|
| 800 x 1280 | RAW8 | 50 fps | Portrait mode |
| 800 x 640 | RAW8 | 50 fps | Landscape mode |
| 800 x 800 | RAW8 | 50 fps | Square mode |
| 1920 x 1080 | RAW10 | 30 fps | 16:9 mode |
| 1280 x 960 | RAW10 | 45 fps | 4:3 binning mode; default and widest tested framing |

RAW8 and RAW10 describe the sensor-to-ISP capture format. The browser stream is JPEG/MJPEG in every mode. The exact list depends on which modes are enabled in `sdkconfig.defaults` and supported by the OV5647 driver.

## Choosing a mode

Start with **1280 x 960** for the widest tested view and least zoomed-in framing. Use the other entries to compare portrait, square, 16:9, and smaller landscape sensor windows. The sensor modes use different readout windows, so their field of view can change; an aspect-ratio change can crop the scene even when the browser preview is shown at the same size.

The browser scales the returned image to fit the preview. **Preview size, CSS scaling, and preview rotation do not change the sensor's field of view.** If the image looks cropped or off-center, first check the selected sensor format. The available sensor modes are defined by the OV5647 driver's mode tables, not by an arbitrary width/height entered in the GUI.

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

## Related controls

The settings dialog also exposes JPEG quality, exposure target, brightness, and an automatic-brightness toggle when the active sensor driver supports it. Preview rotation is saved in that browser's local storage and only affects the displayed preview.
