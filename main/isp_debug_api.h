/*
 * ISP / IPA debug API (CONFIG_EXAMPLE_ENABLE_ISP_DEBUG_API), on the control
 * port only:
 *
 *   GET  /api/debug/isp        AE/AWB/CCM/gamma/module state, IPA statistics,
 *                              sensor registers, active overrides
 *   POST /api/debug/isp        debug overrides (JSON), {"reset":true} to clear
 *   GET  /api/debug/raw_bayer  one unprocessed frame straight from the sensor
 *                              (ISP bypassed); see docs/camera-formats.md
 *   GET  /api/debug/stream_stats  MJPEG pipeline timing per stage (stream_stats.h)
 *   GET  /api/debug/tcp_throughput?mb=20  raw TCP send ceiling (no camera)
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_cam_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    /* ISP video device fd of the MIPI-CSI camera, or -1. */
    int (*isp_fd)(void *ctx);
    /* Sensor of the MIPI-CSI camera, or NULL. */
    esp_cam_sensor_device_t *(*sensor)(void *ctx);
    /* Capture one RAW frame (ISP bypassed) into a heap buffer the caller frees. */
    esp_err_t (*raw_capture)(void *ctx, int skip_frames, uint8_t **data, size_t *len,
                             uint32_t *width, uint32_t *height, uint32_t *pixfmt);
} isp_debug_api_ops_t;

/* Number of URI handlers isp_debug_api_register() adds. */
#define ISP_DEBUG_API_URI_COUNT 5

esp_err_t isp_debug_api_register(httpd_handle_t server, const isp_debug_api_ops_t *ops);

#ifdef __cplusplus
}
#endif
