/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "driver/ppa.h"
#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
#include "driver/jpeg_encode.h"
#endif
#include "nvs_flash.h"
#include "nvs.h"
#include <ctype.h>
#include "esp_check.h"
#include "esp_http_server.h"
#include "protocol_examples_common.h"
#include "mdns.h"
#include "lwip/inet.h"
#include "lwip/apps/netbiosns.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_video_isp_pipeline.h"
#include "esp_cam_sensor.h"
#include "esp_video_device_common.h"
#include "esp_video_device_internal.h"
#include "example_video_common.h"
#include "lens_calibration.h"
#include "isp_ipa_hook.h"
#include "isp_debug_api.h"
#include "stream_stats.h"
#if CONFIG_CAMERA_IMX708
#include "imx708.h"
#endif
#include "imx708_modes.h"

#define EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER  CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER

#define EXAMPLE_JPEG_ENC_QUALITY            CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY
#define EXAMPLE_JPEG_ENC_QUALITY_FULL_HD    CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY_FULL_HD

#define EXAMPLE_MDNS_INSTANCE               CONFIG_EXAMPLE_MDNS_INSTANCE
#define EXAMPLE_MDNS_HOST_NAME              CONFIG_EXAMPLE_MDNS_HOST_NAME

#define EXAMPLE_PART_BOUNDARY               CONFIG_EXAMPLE_HTTP_PART_BOUNDARY

/*
 * The MJPEG response is written straight onto the socket (no chunked
 * encoding): one send() for the boundary + part header and one for the JPEG,
 * instead of three chunks of three writes each. The connection is closed when
 * the stream ends. X-Timestamp is when the frame left V4L2 (seconds since
 * boot, microsecond resolution).
 */
static const char *STREAM_RESPONSE_HEADER = "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace;boundary=" EXAMPLE_PART_BOUNDARY "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache, no-store\r\n"
        "X-Framerate: %s\r\n"
        "Connection: close\r\n\r\n";
static const char *STREAM_PART = "\r\n--" EXAMPLE_PART_BOUNDARY "\r\n"
        "Content-Type: image/jpeg\r\nContent-Length: %" PRIu32 "\r\nX-Timestamp: %lld.%06lld\r\n\r\n";

/*
 * JPEG frames are encoded by a capture task into a small ring of output
 * buffers ("slots"); stream clients and /api/capture_image take a reference
 * to the newest finished slot and send it, so a slow client never holds up
 * the sensor - it just skips frames. One slot is being encoded, one is the
 * newest, the rest are being sent.
 */
#define JPEG_SLOT_COUNT     3
#define MAX_FRAME_WAITERS   4
#define CAPTURE_TASK_STACK  (6 * 1024)
/* Above the EMAC RX task (15): the encoder must be restarted as soon as it
 * finishes a frame, or at 1920x1080 (30 ms encode per 33.3 ms frame) the
 * sensor gets ahead of it. The task only sleeps on V4L2/encoder events. */
#define CAPTURE_TASK_PRIO   16
#define STREAM_TASK_PRIO    14

/*
 * A consumer of new frames: stream clients (capped = follow the camera's
 * stream frame-rate cap) and /api/capture_image (never capped). next_due_us is
 * the earliest frame time (ts_us) the client still wants (0 = the next frame).
 */
typedef struct {
    SemaphoreHandle_t sem;              /*!< given on every new frame */
    bool capped;
    int64_t next_due_us;                /*!< sender's grid: next image time it accepts */
    int64_t enc_due_us;                 /*!< the same grid as seen by the capture task (frames encoded for it) */
} frame_waiter_t;

typedef struct {
    uint8_t *buf;
    uint32_t size;      /*!< capacity */
    uint32_t len;       /*!< JPEG bytes */
    uint32_t seq;       /*!< frame number, 1.. */
    int64_t ts_us;      /*!< when the frame left V4L2 */
    uint8_t refs;       /*!< readers + the encoder while it is being written */
} jpeg_slot_t;

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t loading_jpg_gz_start[] asm("_binary_loading_jpg_gz_start");
extern const uint8_t loading_jpg_gz_end[] asm("_binary_loading_jpg_gz_end");
extern const uint8_t favicon_ico_gz_start[] asm("_binary_favicon_ico_gz_start");
extern const uint8_t favicon_ico_gz_end[] asm("_binary_favicon_ico_gz_end");
extern const uint8_t assets_index_js_gz_start[] asm("_binary_index_js_gz_start");
extern const uint8_t assets_index_js_gz_end[] asm("_binary_index_js_gz_end");
extern const uint8_t assets_index_css_gz_start[] asm("_binary_index_css_gz_start");
extern const uint8_t assets_index_css_gz_end[] asm("_binary_index_css_gz_end");
/* Embedded with EMBED_FILES (no trailing NUL): end - start is the exact .gz size. */
extern const uint8_t overlays_master_png_gz_start[] asm("_binary_Master_overlay_png_gz_start");
extern const uint8_t overlays_master_png_gz_end[] asm("_binary_Master_overlay_png_gz_end");
extern const uint8_t overlays_blank_png_gz_start[] asm("_binary_Blank_overlay_png_gz_start");
extern const uint8_t overlays_blank_png_gz_end[] asm("_binary_Blank_overlay_png_gz_end");

/**
 * @brief Web cam control structure
 */
typedef struct web_cam_video {
    int fd;
    int isp_fd;
    uint8_t index;
    const char *dev_name;
    httpd_handle_t stream_httpd;
    volatile bool stop_stream_requested;

    example_encoder_handle_t encoder_handle;

    /* Capture task and the JPEG slot ring (see JPEG_SLOT_COUNT). */
    jpeg_slot_t slots[JPEG_SLOT_COUNT];
    int latest_slot;                    /*!< newest finished slot, -1 if none */
    uint32_t latest_seq;
    SemaphoreHandle_t slot_lock;
    frame_waiter_t waiters[MAX_FRAME_WAITERS];
    volatile uint32_t consumers;        /*!< registered waiters: encode only while > 0 */
    TaskHandle_t capture_task;
    volatile bool capture_stop;
    SemaphoreHandle_t capture_exited;
    /* One-shot copy of an unencoded frame for /api/capture_binary. */
    SemaphoreHandle_t raw_done;
    volatile bool raw_pending;
    uint8_t *raw_data;
    uint32_t raw_len;

    uint8_t *buffer[EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_length[EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_size;

    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t jpeg_quality;

    uint32_t frame_rate;                /*!< sensor rate, whole fps (format matching only) */
    double sensor_fps;                  /*!< actual sensor frame rate */
    double mode_max_fps;                /*!< the mode's uncapped sensor rate */
    double sensor_cap;                  /*!< sensor frame-rate cap, 0 = none (NVS, per sensor) */
    double stream_cap;                  /*!< MJPEG stream frame-rate cap, 0 = none (NVS, per sensor) */
    bool caps_loaded;
    volatile bool ae_ceiling_pending;   /*!< re-apply the AE ceiling once the ISP pipeline runs */
    esp_cam_sensor_device_t *sensor_dev;
    uint32_t img_width, img_height;     /*!< delivered image size (tile scan: the canvas) */

    /* IMX708 full-view tile scan (imx708_modes.h); tiling.count == 0 when off. */
    imx708_tiling_t tiling;
    uint8_t *canvas[2];                 /*!< composed images: one being filled, one encoded */
    uint32_t canvas_size;
    int tile_cw;                        /*!< canvas being filled */
    uint8_t tile_mask;                  /*!< tiles pasted into it this cycle */
    bool tile_published;                /*!< a canvas has been completed at least once */
    uint32_t tile_fc;                   /*!< frames dequeued since the scan started */
    int8_t tile_sched[8];               /*!< tile shown by frame fc (fc % 8), -1 unknown */
    int tile_next;                      /*!< tile to program next */
    int64_t tile_prev_us;
    TaskHandle_t tile_enc_task;
    SemaphoreHandle_t tile_enc_go;
    SemaphoreHandle_t tile_enc_exited;
    volatile int tile_enc_idx;          /*!< canvas handed to the encoder, -1 if idle */
    int64_t tile_enc_ts;
    uint32_t tile_errors;               /*!< frames whose tile could not be established */
    uint32_t tile_paste_us;             /*!< last PPA paste time */
    SemaphoreHandle_t tile_paste_sem;   /*!< given by the PPA when a paste is done */
    struct v4l2_buffer tile_paste_buf;  /*!< V4L2 buffer being pasted (re-queued when done) */
    int tile_paste_tile;                /*!< tile being pasted, -1 if none */
    int64_t tile_paste_t0, tile_paste_ts;

    int32_t image_control_values[4];
    uint8_t image_control_values_valid;

    SemaphoreHandle_t sem;

    lens_window_t applied_window;   /* lens-centred window of the active mode */

    uint32_t support_control_jpeg_quality   : 1;
} web_cam_video_t;

typedef struct web_cam {
    uint8_t video_count;
    web_cam_video_t video[0];
} web_cam_t;

typedef struct web_cam_video_config {
    const char *dev_name;
    uint32_t buffer_count;
} web_cam_video_config_t;

typedef struct request_desc {
    int index;
} request_desc_t;

static const char *TAG = "example";

typedef struct {
    const char *key;
    const char *label;
    uint32_t id;
    bool is_isp_control;
    bool can_read_value;
} camera_image_control_t;

static const camera_image_control_t s_camera_image_controls[] = {
    {"exposure_target", "Auto exposure target", V4L2_CID_EXPOSURE, false, false},
    {"brightness", "Brightness", V4L2_CID_BRIGHTNESS, true, true},
    {"contrast", "Contrast", V4L2_CID_CONTRAST, true, true},
    {"saturation", "Saturation", V4L2_CID_SATURATION, true, true},
};

static int camera_image_control_fd(web_cam_video_t *video, const camera_image_control_t *setting)
{
    return setting->is_isp_control ? video->isp_fd : video->fd;
}

static bool camera_auto_brightness_get(web_cam_video_t *video, bool *enabled)
{
    if (!video || !enabled) {
        return false;
    }

    esp_video_cam_t camera = {0};
    if (esp_video_device_common_get_video_cam(CSI_NAME, &camera) != ESP_OK || !camera.sensor) {
        return false;
    }

    int value = 0;
    if (esp_cam_sensor_get_para_value(camera.sensor, ESP_CAM_SENSOR_AE_CONTROL, &value, sizeof(value)) != ESP_OK) {
        return false;
    }

    *enabled = value != 0;
    return true;
}

static esp_err_t camera_auto_brightness_set(web_cam_video_t *video, bool enabled)
{
    ESP_RETURN_ON_FALSE(video, ESP_ERR_INVALID_ARG, TAG, "camera is NULL");

    esp_video_cam_t camera = {0};
    ESP_RETURN_ON_ERROR(esp_video_device_common_get_video_cam(CSI_NAME, &camera), TAG,
                        "failed to get camera sensor");
    ESP_RETURN_ON_FALSE(camera.sensor, ESP_ERR_INVALID_STATE, TAG, "camera sensor is NULL");

    int value = enabled ? 1 : 0;
    return esp_cam_sensor_set_para_value(camera.sensor, ESP_CAM_SENSOR_AE_CONTROL, &value, sizeof(value));
}

/* Detected sensor of the MIPI-CSI camera (NULL if none / other interface). */
static esp_cam_sensor_device_t *camera_sensor(web_cam_video_t *video)
{
    if (!video || !video->dev_name || strcmp(video->dev_name, ESP_VIDEO_MIPI_CSI_DEVICE_NAME) != 0) {
        return NULL;
    }
    esp_video_cam_t camera = {0};
    if (esp_video_device_common_get_video_cam(CSI_NAME, &camera) != ESP_OK) {
        return NULL;
    }
    return camera.sensor;
}

/*
 * V4L2_CID_EXPOSURE is the sensor's ESP_CAM_SENSOR_EXPOSURE_VAL. On the OV5647
 * that is the target of the sensor's own AEC loop ("Auto exposure target"); on
 * the IMX708 it is the raw integration time in lines, which the ISP pipeline's
 * AE (esp_ipa agc) owns - a user value would only fight it, so the control is
 * not offered there.
 */
static bool camera_exposure_is_manual_time(web_cam_video_t *video)
{
    esp_cam_sensor_device_t *sensor = camera_sensor(video);
    return sensor && sensor->name && strcmp(sensor->name, "IMX708") == 0;
}

/* ------------------------------------------------------------------ */
/* Sensor identity (frame rate and caps: see "Sensor frame rate" below)  */
/* ------------------------------------------------------------------ */
static bool camera_is_imx708(web_cam_video_t *video)
{
    esp_cam_sensor_device_t *sensor = camera_sensor(video);
    return sensor && sensor->name && strcmp(sensor->name, "IMX708") == 0;
}

/*
 * Sensor format enumeration: the driver's own modes, then (IMX708) the extra
 * full-view modes built in RAM by imx708_modes.c. Every place that maps a
 * format index to a mode goes through here, so the extra modes are ordinary
 * selectable formats (index = driver count + i).
 */
static int enum_sensor_format(web_cam_video_t *video, struct v4l2_sensor_format_enum *e)
{
    if (ioctl(video->fd, VIDIOC_ENUM_SENSOR_FMT, e) == 0) {
        return 0;
    }
#if CONFIG_CAMERA_IMX708
    if (camera_is_imx708(video)) {
        uint32_t n = (uint32_t)imx708_format_count();
        const esp_cam_sensor_format_t *f = e->index >= n ? imx708_modes_get(e->index - n) : NULL;
        if (f) {
            e->format = *f;
            return 0;
        }
    }
#endif
    return -1;
}

/* Sensor frames per delivered image: 4 for the IMX708 full-view tile scan, else 1. */
static int format_tiles(const esp_cam_sensor_format_t *format)
{
    return imx708_modes_tiling(format, NULL) ? 4 : 1;
}

/* Delivered image size of a format (tile scan: the composed canvas). */
static void format_image_size(const esp_cam_sensor_format_t *format, uint32_t *w, uint32_t *h)
{
    imx708_tiling_t t;
    if (imx708_modes_tiling(format, &t)) {
        *w = t.canvas_w;
        *h = t.canvas_h;
    } else {
        *w = format->width;
        *h = format->height;
    }
}

#if CONFIG_CAMERA_IMX708
/*
 * The rate an IMX708 mode runs at without a cap: the frame length is the
 * longest that still gives CONFIG_EXAMPLE_IMX708_FRAME_RATE (rounded down,
 * so the rate is never below the setting: 30 -> 2494 lines = 30.01 fps).
 */
static uint32_t imx708_uncapped_lines(const esp_cam_sensor_isp_info_v1_t *t)
{
    return (uint32_t)((uint64_t)t->pclk / ((uint64_t)t->hts * CONFIG_EXAMPLE_IMX708_FRAME_RATE));
}
#endif

/*
 * Highest sensor frame rate of a mode: what it runs at with no sensor cap,
 * and the top of the cap's range. IMX708: CONFIG_EXAMPLE_IMX708_FRAME_RATE
 * (frame length set by apply_sensor_frame_rate()); OV5647: the mode table's.
 */
static double format_max_fps(web_cam_video_t *video, const esp_cam_sensor_format_t *format)
{
#if CONFIG_CAMERA_IMX708
    if (camera_is_imx708(video) && format->isp_info) {
        const esp_cam_sensor_isp_info_v1_t *t = &format->isp_info->isp_v1_info;
        uint32_t lines = imx708_uncapped_lines(t);
        if (t->hts && lines) {
            return (double)t->pclk / ((double)t->hts * lines);
        }
    }
#endif
    return format->fps;
}

/* Whole-fps rate of a mode (format descriptions and format matching). */
static uint32_t mode_stream_fps(web_cam_video_t *video, const esp_cam_sensor_format_t *format)
{
    return (uint32_t)floor(format_max_fps(video, format) + 1e-6);
}

/* Images per second the camera delivers before the stream cap. */
static double video_image_fps(web_cam_video_t *video)
{
    return video->sensor_fps / (video->tiling.count ? video->tiling.count : 1);
}

/* What a stream client receives: the image rate, limited by the stream cap. */
static double video_stream_fps(web_cam_video_t *video)
{
    double f = video_image_fps(video);
    return video->stream_cap > 0 && video->stream_cap < f ? video->stream_cap : f;
}

/* Rates as JSON / header numbers: two decimals, no float noise. */
static double round2(double v)
{
    return round(v * 100.0) / 100.0;
}

/* A rate for text (descriptions, X-Framerate): one decimal, none when whole (30.01 -> "30", 7.5 -> "7.5"). */
static void format_fps(char *buf, size_t size, double fps)
{
    double r = round(fps * 10.0) / 10.0;
    if (fabs(r - round(r)) < 1e-9) {
        snprintf(buf, size, "%.0f", r);
    } else {
        snprintf(buf, size, "%.1f", r);
    }
}

/*
 * Default JPEG quality of a mode, from the JPEG pixel rate it produces. A
 * 1920x1080 stream at 30 fps is bound by the 100 Mbit/s link (about 10.3 MB/s
 * of JPEG gets through while the encoder shares PSRAM), so modes at or near
 * that rate default to EXAMPLE_JPEG_COMPRESSION_QUALITY_FULL_HD; every other
 * mode reaches its full rate at EXAMPLE_JPEG_COMPRESSION_QUALITY (the IMX708
 * full-view scan delivers 2304x1296 at 7.5 fps = 22 Mpx/s). It is a default
 * only: the UI's quality slider starts here for the mode and the user can move
 * it (docs/camera-formats.md has the size/quality/fps tables).
 */
static int default_jpeg_quality(uint32_t width, uint32_t height, double image_fps)
{
    return (double)width * height * image_fps >= 0.9 * 1920.0 * 1080.0 * 30.0 ?
           EXAMPLE_JPEG_ENC_QUALITY_FULL_HD : EXAMPLE_JPEG_ENC_QUALITY;
}

/* ------------------------------------------------------------------ */
/* Frame-rate caps (persisted per sensor)                              */
/* ------------------------------------------------------------------ */
/*
 * Two independent, optional caps per camera, stored in NVS per sensor
 * (namespace "cam_cfg", key "fps_<sensor>", e.g. "fps_imx708"):
 *   sensor cap - the sensor runs at min(cap, mode max) by lengthening its
 *                frame (IMX708: frame length through the driver; OV5647: VTS
 *                in the RAM copy of the mode table). Exposure may then grow
 *                with the frame (AE ceiling follows, in whole flicker periods).
 *   stream cap - the MJPEG sender sends at most this many images per second,
 *                dropping evenly by frame timestamp; the capture task does not
 *                encode frames that no consumer is due for.
 */
#define CAM_CFG_NVS_NAMESPACE   "cam_cfg"
#define CAM_CFG_FPS_VERSION     0x46500001u   /* 'FP' v1 */
#define FPS_CAP_MIN             1.0
#define FPS_CAP_MAX             120.0

typedef struct {
    uint32_t version;
    uint32_t sensor_cap_mfps;   /*!< milli-fps, 0 = no cap */
    uint32_t stream_cap_mfps;
} cam_fps_caps_nvs_t;

static void fps_caps_key(web_cam_video_t *video, char *key, size_t size)
{
    esp_cam_sensor_device_t *sensor = video->sensor_dev;
    size_t n = (size_t)snprintf(key, size, "fps_");
    for (const char *c = sensor && sensor->name ? sensor->name : "cam"; *c && n + 1 < size; c++) {
        key[n++] = (char)tolower((unsigned char)*c);
    }
    key[n] = '\0';
}

static void fps_caps_load(web_cam_video_t *video)
{
    char key[16];
    fps_caps_key(video, key, sizeof(key));
    video->sensor_cap = 0;
    video->stream_cap = 0;
    nvs_handle_t nvs;
    if (nvs_open(CAM_CFG_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    cam_fps_caps_nvs_t rec = {0};
    size_t size = sizeof(rec);
    esp_err_t ret = nvs_get_blob(nvs, key, &rec, &size);
    nvs_close(nvs);
    if (ret != ESP_OK || size != sizeof(rec) || rec.version != CAM_CFG_FPS_VERSION) {
        return;
    }
    if (rec.sensor_cap_mfps >= FPS_CAP_MIN * 1000 && rec.sensor_cap_mfps <= FPS_CAP_MAX * 1000) {
        video->sensor_cap = rec.sensor_cap_mfps / 1000.0;
    }
    if (rec.stream_cap_mfps >= FPS_CAP_MIN * 1000 && rec.stream_cap_mfps <= FPS_CAP_MAX * 1000) {
        video->stream_cap = rec.stream_cap_mfps / 1000.0;
    }
    ESP_LOGI(TAG, "video%d: frame-rate caps \"%s\": sensor %.3g, stream %.3g fps (0 = none)", video->index, key,
             video->sensor_cap, video->stream_cap);
}

static esp_err_t fps_caps_save(web_cam_video_t *video)
{
    char key[16];
    fps_caps_key(video, key, sizeof(key));
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(CAM_CFG_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    if (video->sensor_cap <= 0 && video->stream_cap <= 0) {
        ret = nvs_erase_key(nvs, key);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    } else {
        cam_fps_caps_nvs_t rec = {
            .version = CAM_CFG_FPS_VERSION,
            .sensor_cap_mfps = (uint32_t)lround(video->sensor_cap * 1000),
            .stream_cap_mfps = (uint32_t)lround(video->stream_cap * 1000),
        };
        ret = nvs_set_blob(nvs, key, &rec, sizeof(rec));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Sensor frame rate                                                   */
/* ------------------------------------------------------------------ */
/*
 * IMX708: the driver ships every mode at 28 fps (frame length 2672 lines).
 * After each mode change the firmware sets the frame length (VTS) through the
 * driver's live control - nothing in the readout window moves, so the
 * lens-centred crop is unaffected:
 *   no cap: CONFIG_EXAMPLE_IMX708_FRAME_RATE, rounded down to whole lines so
 *           the rate never falls below it (30 -> 2494 lines = 30.01 fps with
 *           the 13.361 us line, 7824 px at 585.6 MHz);
 *   cap   : rounded up so the rate never exceeds the cap (15 -> 4990 lines =
 *           14.999 fps).
 * The AE ceiling is then set to the longest exposure the frame holds, in
 * whole mains-flicker periods (3 x 10 ms at 30 fps / 50 Hz, 6 x 10 ms at 15).
 * OV5647: the cap is applied in the mode table (lens_window_build_format).
 */
#if CONFIG_EXAMPLE_IMX708_ANTI_FLICKER_50HZ
#define AE_FLICKER_PERIOD_US    10000
#elif CONFIG_EXAMPLE_IMX708_ANTI_FLICKER_60HZ
#define AE_FLICKER_PERIOD_US    8333
#else
#define AE_FLICKER_PERIOD_US    0
#endif

static void apply_ae_ceiling(web_cam_video_t *video)
{
    esp_cam_sensor_device_t *sensor = video->sensor_dev;
    esp_cam_sensor_format_t current = {0};
    esp_cam_sensor_param_desc_t qdesc = { .id = ESP_CAM_SENSOR_EXPOSURE_VAL };
    if (!sensor || ioctl(video->fd, VIDIOC_G_SENSOR_FMT, &current) != 0 || !current.isp_info ||
            !current.isp_info->isp_v1_info.tline_ns ||
            esp_cam_sensor_query_para_desc(sensor, &qdesc) != ESP_OK) {
        video->ae_ceiling_pending = false;
        return;
    }
    uint32_t sensor_max_us = (uint32_t)((uint64_t)qdesc.number.maximum * current.isp_info->isp_v1_info.tline_ns / 1000);
    uint32_t max_us = sensor_max_us;
    if (AE_FLICKER_PERIOD_US && max_us >= AE_FLICKER_PERIOD_US) {
        max_us = max_us / AE_FLICKER_PERIOD_US * AE_FLICKER_PERIOD_US;
    }
    /* esp_video checks the ceiling against the sensor range it read at boot
     * (28 fps); keep that range in step with the frame first. */
    bool range_ok = isp_hook_set_sensor_max_exposure(sensor_max_us);
    esp_err_t r = esp_video_isp_pipeline_set_agc_max_exposure(max_us);
    video->ae_ceiling_pending = !range_ok || r != ESP_OK;
    ESP_LOGI(TAG, "video%d: AE max exposure %" PRIu32 " us (frame holds %" PRIu32 " us)%s", video->index, max_us,
             sensor_max_us, r == ESP_OK ? (range_ok ? "" : " (ISP range update pending)") : " (pending)");
}

static void apply_sensor_frame_rate(web_cam_video_t *video)
{
#if CONFIG_CAMERA_IMX708
    if (!camera_is_imx708(video)) {
        return;
    }
    esp_cam_sensor_device_t *sensor = video->sensor_dev;
    esp_cam_sensor_format_t current = {0};
    if (!sensor || ioctl(video->fd, VIDIOC_G_SENSOR_FMT, &current) != 0 || !current.isp_info) {
        ESP_LOGW(TAG, "video%d: no sensor timing; frame rate left at the mode's", video->index);
        return;
    }
    const esp_cam_sensor_isp_info_v1_t *t = &current.isp_info->isp_v1_info;
    uint32_t uncapped = imx708_uncapped_lines(t);
    double max_fps = (double)t->pclk / ((double)t->hts * uncapped);
    uint32_t lines = uncapped;
    if (video->sensor_cap > 0 && video->sensor_cap < max_fps) {
        lines = (uint32_t)ceil((double)t->pclk / ((double)t->hts * video->sensor_cap) - 1e-6);
    }
    if (esp_cam_sensor_set_para_value(sensor, IMX708_CID_FRAME_LENGTH, &lines, sizeof(lines)) != ESP_OK) {
        ESP_LOGW(TAG, "video%d: failed to set frame length %" PRIu32, video->index, lines);
        return;
    }
    uint32_t actual = lines;
    imx708_get_frame_length(&actual, NULL, NULL);     /* long frames are rounded to 2^n lines */
    video->mode_max_fps = max_fps;
    video->sensor_fps = (double)t->pclk / ((double)t->hts * actual);
    video->frame_rate = (uint32_t)floor(video->sensor_fps + 1e-6);
    ESP_LOGI(TAG, "video%d: IMX708 frame length %" PRIu32 " lines -> %.3f fps (mode max %.2f, cap %s)",
             video->index, actual, video->sensor_fps, max_fps, video->sensor_cap > 0 ? "on" : "off");
    apply_ae_ceiling(video);
#else
    (void)video;
#endif
}

static bool query_camera_image_control(web_cam_video_t *video, const camera_image_control_t *setting,
                                       int *fd, struct v4l2_query_ext_ctrl *qctrl)
{
    *fd = camera_image_control_fd(video, setting);
    if (*fd < 0) {
        return false;
    }
    if (setting->id == V4L2_CID_EXPOSURE && camera_exposure_is_manual_time(video)) {
        return false;
    }

    memset(qctrl, 0, sizeof(*qctrl));
    qctrl->id = setting->id;
    return ioctl(*fd, VIDIOC_QUERY_EXT_CTRL, qctrl) == 0 &&
           !(qctrl->flags & V4L2_CTRL_FLAG_DISABLED) &&
           qctrl->type == V4L2_CTRL_TYPE_INTEGER && qctrl->step > 0;
}

static bool get_camera_image_control_value(web_cam_video_t *video, size_t setting_index,
                                           int fd, int *value)
{
    if (!s_camera_image_controls[setting_index].can_read_value) {
        if (video->image_control_values_valid & (1U << setting_index)) {
            *value = video->image_control_values[setting_index];
            return true;
        }
        return false;
    }

    struct v4l2_ext_control control = {0};
    struct v4l2_ext_controls controls = {0};
    control.id = s_camera_image_controls[setting_index].id;
    controls.ctrl_class = V4L2_CID_USER_CLASS;
    controls.count = 1;
    controls.controls = &control;

    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls) == 0) {
        *value = control.value;
        video->image_control_values[setting_index] = control.value;
        video->image_control_values_valid |= (1U << setting_index);
        return true;
    }

    if (video->image_control_values_valid & (1U << setting_index)) {
        *value = video->image_control_values[setting_index];
        return true;
    }
    return false;
}

static esp_err_t set_camera_image_controls(web_cam_video_t *video, const cJSON *json_controls)
{
    if (!json_controls) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(cJSON_IsObject(json_controls), ESP_ERR_INVALID_ARG, TAG, "invalid image controls object");

    for (size_t i = 0; i < sizeof(s_camera_image_controls) / sizeof(s_camera_image_controls[0]); i++) {
        const camera_image_control_t *setting = &s_camera_image_controls[i];
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(json_controls, setting->key);
        if (!item) {
            continue;
        }
        ESP_RETURN_ON_FALSE(cJSON_IsNumber(item), ESP_ERR_INVALID_ARG, TAG, "invalid value for %s", setting->key);

        int fd;
        struct v4l2_query_ext_ctrl qctrl;
        ESP_RETURN_ON_FALSE(query_camera_image_control(video, setting, &fd, &qctrl), ESP_ERR_NOT_SUPPORTED, TAG,
                            "camera control %s is not supported", setting->key);
        double requested = item->valuedouble;
        ESP_RETURN_ON_FALSE(requested >= qctrl.minimum && requested <= qctrl.maximum &&
                            requested == (int32_t)requested &&
                            (((int64_t)requested - qctrl.minimum) % qctrl.step) == 0,
                            ESP_ERR_INVALID_ARG, TAG, "value for %s is out of range", setting->key);

        struct v4l2_ext_control control = {0};
        struct v4l2_ext_controls controls = {0};
        control.id = setting->id;
        control.value = (int32_t)requested;
        controls.ctrl_class = V4L2_CID_USER_CLASS;
        controls.count = 1;
        controls.controls = &control;
        ESP_RETURN_ON_ERROR(ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls), TAG, "failed to set %s", setting->key);

        video->image_control_values[i] = control.value;
        video->image_control_values_valid |= (1U << i);
    }

    return ESP_OK;
}

static bool is_valid_web_cam(web_cam_video_t *video)
{
    return video->fd != -1;
}

static const char *sensor_format_name(esp_cam_sensor_output_format_t format)
{
    switch (format) {
    case ESP_CAM_SENSOR_PIXFORMAT_RAW8:
        return "RAW8";
    case ESP_CAM_SENSOR_PIXFORMAT_RAW10:
        return "RAW10";
    case ESP_CAM_SENSOR_PIXFORMAT_RAW12:
        return "RAW12";
    case ESP_CAM_SENSOR_PIXFORMAT_YUV422:
        return "YUV422";
    case ESP_CAM_SENSOR_PIXFORMAT_RGB565:
        return "RGB565";
    case ESP_CAM_SENSOR_PIXFORMAT_RGB888:
        return "RGB888";
    case ESP_CAM_SENSOR_PIXFORMAT_JPEG:
        return "JPEG";
    default:
        return "sensor";
    }
}

static int find_sensor_format_index(web_cam_video_t *video, const esp_cam_sensor_format_t *wanted)
{
    if (!video || video->fd < 0 || !wanted) {
        return -1;
    }

    for (uint32_t i = 0; ; i++) {
        struct v4l2_sensor_format_enum sensor_enum = { .index = i };
        if (enum_sensor_format(video, &sensor_enum) != 0) {
            break;
        }

        const esp_cam_sensor_format_t *candidate = &sensor_enum.format;
        /* fps is not compared: a retimed mode (apply_sensor_frame_rate) reports
         * its live rate while the table keeps the driver's. The name is unique. */
        if ((wanted->regs && candidate->regs == wanted->regs) ||
                (candidate->width == wanted->width && candidate->height == wanted->height &&
                 candidate->format == wanted->format &&
                 candidate->name && wanted->name && strcmp(candidate->name, wanted->name) == 0)) {
            return (int)i;
        }
    }
    return -1;
}

static int current_capture_format_index(web_cam_video_t *video)
{
    int dimensions_match = -1;
    for (uint32_t i = 0; ; i++) {
        struct v4l2_sensor_format_enum sensor_enum = { .index = i };
        if (enum_sensor_format(video, &sensor_enum) != 0) {
            break;
        }
        const esp_cam_sensor_format_t *format = &sensor_enum.format;
        if (format->width == video->width && format->height == video->height) {
            if (mode_stream_fps(video, format) == video->frame_rate) {
                return (int)i;
            }
            if (dimensions_match < 0) {
                dimensions_match = (int)i;
            }
        }
    }

    if (dimensions_match >= 0) {
        return dimensions_match;
    }

    esp_cam_sensor_format_t current = {0};
    if (ioctl(video->fd, VIDIOC_G_SENSOR_FMT, &current) == 0) {
        return find_sensor_format_index(video, &current);
    }
    return -1;
}

/*
 * Lens-centred windows apply to MIPI-CSI sensors with a profile in
 * lens_calibration.c (OV5647, IMX708). Selecting the profile also loads that
 * sensor's own calibration from NVS.
 */
static bool camera_lens_windows_supported(web_cam_video_t *video)
{
    esp_cam_sensor_device_t *sensor = camera_sensor(video);
    if (!sensor) {
        return false;
    }
    return lens_sensor_select(sensor->name);
}

/*
 * Apply a sensor mode with its readout window re-centred on the stored lens
 * centre. The rewritten format lives in static storage owned by
 * lens_calibration.c because the driver keeps the pointer as its current
 * format. Falls back to the stock table for sensors we do not handle.
 */
static esp_err_t apply_lens_centred_sensor_format(web_cam_video_t *video, const esp_cam_sensor_format_t *base)
{
    const esp_cam_sensor_format_t *to_apply = base;

    memset(&video->applied_window, 0, sizeof(video->applied_window));
    if (camera_lens_windows_supported(video)) {
        /* Sensors whose orientation lives outside the mode table (IMX708):
         * program it before VIDIOC_S_SENSOR_FMT, which is where esp_video
         * latches the (flip-dependent) Bayer phase. */
        int hmirror, vflip;
        esp_cam_sensor_device_t *sensor = camera_sensor(video);
        if (sensor && lens_sensor_orientation(&hmirror, &vflip)) {
            if (esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_HMIRROR, &hmirror, sizeof(hmirror)) != ESP_OK ||
                    esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_VFLIP, &vflip, sizeof(vflip)) != ESP_OK) {
                ESP_LOGW(TAG, "video%d: failed to set sensor orientation", video->index);
            }
        }
        esp_cam_sensor_format_t current = {0};
        const void *in_use_regs = NULL;
        if (ioctl(video->fd, VIDIOC_G_SENSOR_FMT, &current) == 0) {
            in_use_regs = current.regs;
        }
        lens_window_t win;
        const esp_cam_sensor_format_t *built = lens_window_build_format(base, lens_calib_get(), in_use_regs, &win,
                                               video->sensor_cap);
        if (built) {
            to_apply = built;
            video->applied_window = win;
            if (format_tiles(base) > 1) {
                /* Tile scan: the image is the whole sensor, whatever crop the first tile has. */
                lens_window_full_sensor(lens_calib_get(), win.scale, &video->applied_window);
            }
        } else {
            ESP_LOGW(TAG, "video%d: cannot lens-centre %ux%u; using the stock window", video->index,
                     base->width, base->height);
        }
    }

    /*
     * IMX708: setting a mode soft-resets the sensor, so exposure and gain go
     * back to the mode table's defaults - but the ISP pipeline's AE (esp_ipa)
     * keeps the values it last wrote and only writes again when its new target
     * differs. If AE had settled (or was pinned at a limit) the sensor would
     * be left at the defaults. Every IMX708 mode shares the same line timing,
     * so put back exactly what AE had set.
     */
    esp_cam_sensor_device_t *sensor = camera_exposure_is_manual_time(video) ? camera_sensor(video) : NULL;
    uint32_t exposure_val = 0, gain_index = 0;
    bool have_exposure = sensor && esp_cam_sensor_get_para_value(sensor, ESP_CAM_SENSOR_EXPOSURE_VAL,
                         &exposure_val, sizeof(exposure_val)) == ESP_OK;
    bool have_gain = sensor && esp_cam_sensor_get_para_value(sensor, ESP_CAM_SENSOR_GAIN,
                     &gain_index, sizeof(gain_index)) == ESP_OK;

    esp_err_t ret = ioctl(video->fd, VIDIOC_S_SENSOR_FMT, to_apply);
    if (ret == ESP_OK) {
        /* Mode rate (OV5647: with the cap built into the table); the IMX708 retimes below. */
        video->mode_max_fps = format_max_fps(video, base);
        video->sensor_fps = lens_sensor_geometry() && lens_sensor_geometry()->kind == LENS_SENSOR_OV5647 ?
                            lens_ov5647_capped_fps(base, video->sensor_cap, NULL) : video->mode_max_fps;
        video->frame_rate = (uint32_t)floor(video->sensor_fps + 1e-6);
        /* Before restoring exposure, so it is clamped to the new frame. */
        apply_sensor_frame_rate(video);
    }
    if (ret == ESP_OK && have_exposure &&
            esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_EXPOSURE_VAL, &exposure_val, sizeof(exposure_val)) != ESP_OK) {
        ESP_LOGW(TAG, "video%d: failed to restore exposure %" PRIu32, video->index, exposure_val);
    }
    if (ret == ESP_OK && have_gain &&
            esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_GAIN, &gain_index, sizeof(gain_index)) != ESP_OK) {
        ESP_LOGW(TAG, "video%d: failed to restore gain index %" PRIu32, video->index, gain_index);
    }
    return ret;
}

static esp_err_t decode_request(web_cam_t *web_cam, httpd_req_t *req, request_desc_t *desc)
{
    esp_err_t ret;
    int index = -1;
    char buffer[32];

    if ((ret = httpd_req_get_url_query_str(req, buffer, sizeof(buffer))) != ESP_OK) {
        return ret;
    }
    ESP_LOGD(TAG, "source: %s", buffer);

    for (int i = 0; i < web_cam->video_count; i++) {
        char source_str[16];

        if (snprintf(source_str, sizeof(source_str), "source=%d", i) <= 0) {
            return ESP_FAIL;
        }

        if (strcmp(buffer, source_str) == 0) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        return ESP_ERR_INVALID_ARG;
    }

    desc->index = index;
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Capture task and JPEG slot ring                                     */
/* ------------------------------------------------------------------ */

static void update_stream_stats_info(web_cam_video_t *video)
{
    stream_stats_info_t info = {
        .width = video->img_width, .height = video->img_height, .quality = video->jpeg_quality,
        .sensor_fps = (float)video->sensor_fps, .image_fps = (float)video_image_fps(video),
        .sensor_cap = (float)video->sensor_cap, .stream_cap = (float)video->stream_cap,
        .tiles = video->tiling.count ? video->tiling.count : 1,
        .v4l2_buffers = EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER, .jpeg_slots = JPEG_SLOT_COUNT,
    };
#if CONFIG_CAMERA_IMX708
    if (camera_is_imx708(video)) {
        imx708_get_frame_length(&info.frame_length, NULL, NULL);
    }
#endif
    stream_stats_set_info(&info);
}

static void slot_lock(web_cam_video_t *video)
{
    xSemaphoreTake(video->slot_lock, portMAX_DELAY);
}

static void slot_unlock(web_cam_video_t *video)
{
    xSemaphoreGive(video->slot_lock);
}

/*
 * Register a semaphore that is given on every new frame. While at least one
 * consumer is due for a frame the capture task encodes; otherwise it just
 * recycles buffers. A capped waiter (stream client) follows the stream cap.
 * Returns the waiter slot, -1 if all are taken.
 */
static int frame_waiter_add(web_cam_video_t *video, SemaphoreHandle_t sem, bool capped)
{
    int slot = -1;
    slot_lock(video);
    for (int i = 0; i < MAX_FRAME_WAITERS; i++) {
        if (!video->waiters[i].sem) {
            video->waiters[i] = (frame_waiter_t) { .sem = sem, .capped = capped };
            video->consumers++;
            slot = i;
            break;
        }
    }
    slot_unlock(video);
    return slot;
}

static void frame_waiter_remove(web_cam_video_t *video, SemaphoreHandle_t sem)
{
    slot_lock(video);
    for (int i = 0; i < MAX_FRAME_WAITERS; i++) {
        if (video->waiters[i].sem == sem) {
            video->waiters[i].sem = NULL;
            video->consumers--;
            break;
        }
    }
    slot_unlock(video);
}

/* Stream cap as an interval between images (0 = none). */
static int64_t stream_interval_us(web_cam_video_t *video)
{
    return video->stream_cap > 0 ? (int64_t)(1e6 / video->stream_cap) : 0;
}

/*
 * Is waiter @p w due for an image taken at @p ts? Frames are accepted on a
 * grid of the cap's interval with half an image period of tolerance, so a
 * 15 fps cap on a 30 fps sensor takes exactly every other frame and a 25 fps
 * cap takes 5 of 6, evenly spread. Caller holds the slot lock.
 */
static bool grid_due(web_cam_video_t *video, const frame_waiter_t *w, int64_t due, int64_t ts)
{
    int64_t iv = w->capped ? stream_interval_us(video) : 0;
    if (iv == 0 || due == 0) {
        return true;
    }
    double f = video_image_fps(video);
    int64_t tol = f > 0 ? (int64_t)(0.5e6 / f) : 16000;
    return ts >= due - tol;
}

static void grid_take(web_cam_video_t *video, const frame_waiter_t *w, int64_t *due, int64_t ts)
{
    int64_t iv = w->capped ? stream_interval_us(video) : 0;
    if (iv == 0) {
        *due = 0;
    } else if (*due == 0 || ts - *due > iv) {
        *due = ts + iv;                 /* first image, or fell behind: restart the grid */
    } else {
        *due += iv;
    }
}

static bool waiter_due(web_cam_video_t *video, const frame_waiter_t *w, int64_t ts)
{
    return grid_due(video, w, w->next_due_us, ts);
}

/* Accept an image for waiter @p w: move its grid on by one interval. Caller holds the slot lock. */
static void waiter_take(web_cam_video_t *video, frame_waiter_t *w, int64_t ts)
{
    grid_take(video, w, &w->next_due_us, ts);
}

/*
 * Does any consumer want an image taken at @p ts? Checked by the capture task
 * before encoding, against a copy of each sender's grid that advances when the
 * frame is published (the sender itself may still be busy with the previous
 * image), so frames a capped sender would skip are never encoded.
 */
static bool frame_wanted(web_cam_video_t *video, int64_t ts)
{
    bool want = false;
    slot_lock(video);
    for (int i = 0; i < MAX_FRAME_WAITERS && !want; i++) {
        want = video->waiters[i].sem && grid_due(video, &video->waiters[i], video->waiters[i].enc_due_us, ts);
    }
    slot_unlock(video);
    return want;
}

/* Reference the newest frame if it is newer than after_seq; -1 if none. */
static int frame_get_latest(web_cam_video_t *video, uint32_t after_seq)
{
    int idx = -1;
    slot_lock(video);
    if (video->latest_slot >= 0 && video->latest_seq != after_seq) {
        idx = video->latest_slot;
        video->slots[idx].refs++;
    }
    slot_unlock(video);
    return idx;
}

static void frame_release(web_cam_video_t *video, int idx)
{
    slot_lock(video);
    if (video->slots[idx].refs) {
        video->slots[idx].refs--;
    }
    slot_unlock(video);
}

/* A slot nobody reads and that is not the newest frame, claimed for writing. */
static int slot_claim_free(web_cam_video_t *video)
{
    int idx = -1;
    slot_lock(video);
    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        if (video->slots[i].buf && video->slots[i].refs == 0 && i != video->latest_slot) {
            video->slots[i].refs = 1;
            idx = i;
            break;
        }
    }
    slot_unlock(video);
    return idx;
}

static void slot_publish(web_cam_video_t *video, int idx, uint32_t len, int64_t ts_us)
{
    slot_lock(video);
    jpeg_slot_t *slot = &video->slots[idx];
    slot->refs--;
    slot->len = len;
    slot->ts_us = ts_us;
    slot->seq = ++video->latest_seq;
    video->latest_slot = idx;
    for (int i = 0; i < MAX_FRAME_WAITERS; i++) {
        frame_waiter_t *w = &video->waiters[i];
        if (w->sem) {
            if (grid_due(video, w, w->enc_due_us, ts_us)) {
                grid_take(video, w, &w->enc_due_us, ts_us);
            }
            xSemaphoreGive(w->sem);
        }
    }
    slot_unlock(video);
}

static void slot_abandon(web_cam_video_t *video, int idx)
{
    frame_release(video, idx);
}

/* /api/capture_binary: hand over a copy of this (unencoded) frame if asked. */
static void serve_raw_request(web_cam_video_t *video, const struct v4l2_buffer *buf)
{
    if (!video->raw_pending) {
        return;
    }
    const uint8_t *src = video->buffer[buf->index];
    uint32_t len = buf->bytesused ? buf->bytesused : video->buffer_length[buf->index];
    if (video->tiling.count) {
        /* Tile scan: the last composed image (the canvas not being filled). */
        if (!video->tile_published) {
            return;
        }
        src = video->canvas[video->tile_cw ^ 1];
        len = video->canvas_size;
    }
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy) {
        memcpy(copy, src, len);
    }
    slot_lock(video);
    if (video->raw_pending && !video->raw_data && copy) {
        video->raw_data = copy;
        video->raw_len = len;
        copy = NULL;
        xSemaphoreGive(video->raw_done);
    }
    slot_unlock(video);
    heap_caps_free(copy);
}

/* ------------------------------------------------------------------ */
/* IMX708 full-view tile scan                                          */
/* ------------------------------------------------------------------ */
/*
 * The sensor delivers one 1168x664 tile of the 2304x1296 binned field per
 * frame; after every dequeued frame the capture task moves the digital crop
 * (0x0408-0x040B, under group hold) to the next tile. Frame k leaves V4L2
 * after the sensor has already started frame k+1 (the ISP and DMA add a few
 * ms), so the write lands for frame k+2 (s_tile_latency = 2, measured on
 * hardware: with 1 every tile was pasted one quarter off). The used
 * quarter of each tile is pasted into a canvas; when all four are in, the
 * canvas goes to the tile encoder task and the next one is filled.
 *
 * A frame's tile is only trusted when the crop for it was written in time:
 * the frame it was written after must have been fresh (DQBUF had to wait for it, so the
 * task is not running behind) and the write must have finished within
 * TILE_WRITE_DEADLINE_US of that frame leaving V4L2 - i.e. well inside the
 * vertical blanking before the next frame starts. Anything else marks the
 * frame "unknown" and it is skipped (tile_errors).
 */
#define TILE_WRITE_DEADLINE_US  8000
/*
 * Highest JPEG quality of the full-view image. Its slots hold 1 B/px (3 MB);
 * quality 95 gives about 1.1 MB, quality 100 overflows them - and an overflowing
 * hardware JPEG encode was followed, twice, by a crash in the ISP's AWB
 * interrupt (corrupted controller), so it must not happen at all.
 */
#define TILE_JPEG_QUALITY_MAX   95
#define TILE_ENC_TASK_PRIO      15
#define TILE_ENC_TASK_STACK     (4 * 1024)
static int s_tile_latency = 2;

static esp_err_t tile_write_crop(web_cam_video_t *video, int tile)
{
    esp_cam_sensor_device_t *sensor = video->sensor_dev;
    if (!sensor) {
        return ESP_ERR_INVALID_STATE;
    }
    const imx708_tiling_t *t = &video->tiling;
    const uint16_t regs[6] = { 0x0104, 0x0408, 0x0409, 0x040a, 0x040b, 0x0104 };
    const uint8_t vals[6] = { 1, (uint8_t)(t->crop_x[tile] >> 8), (uint8_t)t->crop_x[tile],
                              (uint8_t)(t->crop_y[tile] >> 8), (uint8_t)t->crop_y[tile], 0
                            };
    esp_err_t ret = ESP_OK;
    for (int i = 0; i < 6; i++) {
        esp_cam_sensor_reg_val_t r = { .regaddr = regs[i], .value = vals[i] };
        esp_err_t e = esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_REG, &r);
        if (e != ESP_OK && ret == ESP_OK) {
            ret = e;
        }
    }
    return ret;
}

/*
 * Paste the used quarter of a tile frame into the canvas being filled, with
 * the PPA (2D-DMA block copy, scale 1). It takes 25-27 ms for the 1.5 MB
 * quarter (PSRAM to PSRAM, shared with the ISP writing the next frame) - a
 * CPU memcpy is slower still - so it runs in the background: the frame's
 * V4L2 buffer is re-queued when the paste has finished, at the next frame
 * (three V4L2 buffers: one being filled, one being pasted, one queued). The
 * driver writes back the input and invalidates the output rows; the canvas is
 * only ever written by the PPA and read by the JPEG encoder's DMA.
 */
static bool IRAM_ATTR tile_paste_done_cb(ppa_client_handle_t client, ppa_event_data_t *event, void *user_data)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_data, &hp);
    return hp == pdTRUE;
}

static esp_err_t tile_paste_start(web_cam_video_t *video, const uint8_t *frame, int tile)
{
    static ppa_client_handle_t s_ppa;
    if (!s_ppa) {
        ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
        ESP_RETURN_ON_ERROR(ppa_register_client(&cfg, &s_ppa), TAG, "no PPA client");
        ppa_event_callbacks_t cbs = { .on_trans_done = tile_paste_done_cb };
        ppa_client_register_event_callbacks(s_ppa, &cbs);
    }
    const imx708_tiling_t *t = &video->tiling;
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = frame, .pic_w = t->tile_w, .pic_h = t->tile_h,
            .block_w = t->part_w, .block_h = t->part_h,
            .block_offset_x = t->src_x[tile], .block_offset_y = t->src_y[tile],
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = video->canvas[video->tile_cw], .buffer_size = video->canvas_size,
            .pic_w = t->canvas_w, .pic_h = t->canvas_h,
            .block_offset_x = t->dst_x[tile], .block_offset_y = t->dst_y[tile],
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
        .user_data = video->tile_paste_sem,
    };
    return ppa_do_scale_rotate_mirror(s_ppa, &op);
}

/* Finish the paste in flight (if any): wait for it, re-queue its V4L2 buffer, account the tile. */
static void tile_paste_finish(web_cam_video_t *video)
{
    if (video->tile_paste_tile < 0) {
        return;
    }
    bool ok = xSemaphoreTake(video->tile_paste_sem, pdMS_TO_TICKS(100)) == pdTRUE;
    video->tile_paste_us = (uint32_t)(esp_timer_get_time() - video->tile_paste_t0);
    ioctl(video->fd, VIDIOC_QBUF, &video->tile_paste_buf);
    if (ok) {
        video->tile_mask |= (uint8_t)(1u << video->tile_paste_tile);
    } else {
        ESP_LOGW(TAG, "video%d: tile paste timed out", video->index);
    }
    video->tile_paste_tile = -1;
}

static void tile_reset_schedule(web_cam_video_t *video)
{
    for (int i = 0; i < 8; i++) {
        video->tile_sched[i] = -1;
    }
}

/* Hand a complete canvas to the encoder task (or drop it if nobody is due / the encoder is busy). */
static void tile_maybe_publish(web_cam_video_t *video, int64_t ts)
{
    uint8_t full = (uint8_t)((1u << video->tiling.count) - 1);
    if (video->tile_mask != full) {
        return;
    }
    if (video->tile_enc_idx < 0 && frame_wanted(video, ts)) {
        video->tile_enc_ts = ts;
        video->tile_enc_idx = video->tile_cw;
        video->tile_cw ^= 1;
        video->tile_published = true;
        xSemaphoreGive(video->tile_enc_go);
    } else if (video->tile_enc_idx < 0) {
        stream_stats_not_wanted();
    }
    video->tile_mask = 0;   /* next image: four fresh tiles */
}

/* One dequeued frame of the tile scan. Its V4L2 buffer is re-queued here or when its paste finishes. */
static void tile_frame(web_cam_video_t *video, struct v4l2_buffer *buf, int64_t t_dq, int64_t t_frame)
{
    uint32_t fc = video->tile_fc++;
    int tile = video->tile_sched[fc % 8];
    video->tile_sched[fc % 8] = -1;

    double fps = video->sensor_fps > 0 ? video->sensor_fps : 30.0;
    int64_t period = (int64_t)(1e6 / fps);
    bool fresh = (t_frame - t_dq) >= 1500 &&
                 (video->tile_prev_us == 0 || t_frame - video->tile_prev_us < period * 3 / 2);
    video->tile_prev_us = t_frame;
    if (!fresh) {
        /* Behind, or a frame went missing: nothing scheduled can be trusted. */
        tile_reset_schedule(video);
        tile = -1;
    }

    /* Program the next tile first: it has to land before the next frame starts. */
    int next = video->tile_next;
    esp_err_t wr = tile_write_crop(video, next);
    int64_t t_w = esp_timer_get_time();
    bool on_time = wr == ESP_OK && fresh && (t_w - t_frame) < TILE_WRITE_DEADLINE_US;
    video->tile_sched[(fc + s_tile_latency) % 8] = on_time ? next : -1;
    if (wr == ESP_OK) {
        video->tile_next = (next + 1) % video->tiling.count;
    }

    /* The previous frame's paste has had a whole frame time; collect it. */
    int64_t prev_ts = video->tile_paste_ts;
    bool had_paste = video->tile_paste_tile >= 0;
    tile_paste_finish(video);
    if (had_paste) {
        tile_maybe_publish(video, prev_ts);
    }

    serve_raw_request(video, buf);
    bool queued = false;
    if (tile < 0) {
        video->tile_errors++;
    } else if (video->consumers > 0) {
        video->tile_paste_t0 = esp_timer_get_time();
        if (tile_paste_start(video, video->buffer[buf->index], tile) == ESP_OK) {
            video->tile_paste_buf = *buf;
            video->tile_paste_tile = tile;
            video->tile_paste_ts = t_frame;
            queued = true;      /* re-queued by tile_paste_finish() */
        }
    }
    if (!queued) {
        ioctl(video->fd, VIDIOC_QBUF, buf);
    }
    stream_stats_capture((uint32_t)(t_frame - t_dq), 0, 0, false);

    /* Diagnostics every 300 frames (10 s): logged when tiles were lost, else at debug level. */
    static uint32_t s_late, s_wmax, s_err_logged;
    uint32_t w_us = (uint32_t)(t_w - t_frame);
    s_late += on_time ? 0 : 1;
    s_wmax = w_us > s_wmax ? w_us : s_wmax;
    if (fc % 300 == 299) {
        bool lost = video->tile_errors > s_err_logged + 1;     /* the first frame of a scan is always unknown */
        s_err_logged = video->tile_errors;
        ESP_LOG_LEVEL_LOCAL(lost ? ESP_LOG_WARN : ESP_LOG_DEBUG, TAG, "tile scan: %" PRIu32 " frames, unknown tile %" PRIu32 ", late writes %" PRIu32
                 " (max write %" PRIu32 " us), paste %" PRIu32 " us", fc + 1, video->tile_errors, s_late,
                 s_wmax, video->tile_paste_us);
        s_wmax = 0;
    }
}

/* Encodes composed canvases (the capture task must not block on a 43 ms encode). */
static void tile_encode_task(void *arg)
{
    web_cam_video_t *video = (web_cam_video_t *)arg;
    while (!video->capture_stop) {
        if (xSemaphoreTake(video->tile_enc_go, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        int ci = video->tile_enc_idx;
        if (ci < 0) {
            continue;
        }
        int idx = slot_claim_free(video);
        if (idx < 0) {
            stream_stats_drop_no_slot();
        } else {
            jpeg_slot_t *slot = &video->slots[idx];
            uint32_t len = 0;
            int64_t t0 = esp_timer_get_time();
            xSemaphoreTake(video->sem, portMAX_DELAY);
            esp_err_t ret = example_encoder_process(video->encoder_handle, video->canvas[ci], video->canvas_size,
                                                    slot->buf, slot->size, &len);
            xSemaphoreGive(video->sem);
            int64_t t1 = esp_timer_get_time();
            if (ret == ESP_OK && len > 0) {
                slot_publish(video, idx, len, video->tile_enc_ts);
                stream_stats_encoded((uint32_t)(t1 - t0), len);
            } else {
                slot_abandon(video, idx);
                ESP_LOGW(TAG, "video%d: JPEG encode of the full-view image failed (%s)", video->index,
                         esp_err_to_name(ret));
            }
        }
        video->tile_enc_idx = -1;
    }
    xSemaphoreGive(video->tile_enc_exited);
    vTaskDelete(NULL);
}

static void capture_task(void *arg)
{
    web_cam_video_t *video = (web_cam_video_t *)arg;
    /* A bounded DQBUF so a stop request is noticed even with no frames. */
    struct timeval tmo = { .tv_sec = 0, .tv_usec = 500 * 1000 };
    ioctl(video->fd, VIDIOC_S_DQBUF_TIMEOUT, &tmo);

    while (!video->capture_stop) {
        if (video->ae_ceiling_pending && isp_hook_sensor_max_exposure()) {
            apply_ae_ceiling(video);    /* the ISP pipeline is running now */
        }
        struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        int64_t t_dq = esp_timer_get_time();
        if (ioctl(video->fd, VIDIOC_DQBUF, &buf) != 0) {
            continue;   /* timeout: no frame within 500 ms */
        }
        int64_t t_frame = esp_timer_get_time();
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        if (video->tiling.count) {
            tile_frame(video, &buf, t_dq, t_frame);
            continue;
        }

        serve_raw_request(video, &buf);

        if (video->consumers == 0) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            stream_stats_capture((uint32_t)(t_frame - t_dq), 0, 0, false);
            continue;
        }
        if (!frame_wanted(video, t_frame)) {
            /* Every consumer is capped and none is due: save the encoder the work. */
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            stream_stats_not_wanted();
            stream_stats_capture((uint32_t)(t_frame - t_dq), 0, 0, false);
            continue;
        }

        int idx = slot_claim_free(video);
        if (idx < 0) {
            ioctl(video->fd, VIDIOC_QBUF, &buf);
            stream_stats_drop_no_slot();
            stream_stats_capture((uint32_t)(t_frame - t_dq), 0, 0, false);
            continue;
        }

        jpeg_slot_t *slot = &video->slots[idx];
        uint32_t len = 0;
        esp_err_t ret = ESP_OK;
        if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
            len = buf.bytesused;
            if (len <= slot->size) {
                memcpy(slot->buf, video->buffer[buf.index], len);
            } else {
                ret = ESP_ERR_NO_MEM;
            }
        } else {
            xSemaphoreTake(video->sem, portMAX_DELAY);
            ret = example_encoder_process(video->encoder_handle, video->buffer[buf.index], video->buffer_size,
                                          slot->buf, slot->size, &len);
            xSemaphoreGive(video->sem);
        }
        int64_t t_enc = esp_timer_get_time();
        ioctl(video->fd, VIDIOC_QBUF, &buf);

        if (ret == ESP_OK && len > 0) {
            slot_publish(video, idx, len, t_frame);
            stream_stats_capture((uint32_t)(t_frame - t_dq), (uint32_t)(t_enc - t_frame), len, true);
        } else {
            slot_abandon(video, idx);
            ESP_LOGW(TAG, "video%d: JPEG encode failed (%s)", video->index, esp_err_to_name(ret));
            stream_stats_capture((uint32_t)(t_frame - t_dq), 0, 0, false);
        }
    }

    tile_paste_finish(video);
    xSemaphoreGive(video->capture_exited);
    vTaskDelete(NULL);
}

static esp_err_t start_capture_task(web_cam_video_t *video)
{
    if (video->capture_task) {
        return ESP_OK;
    }
    if (!video->slot_lock) {
        video->slot_lock = xSemaphoreCreateMutex();
        video->capture_exited = xSemaphoreCreateBinary();
        video->raw_done = xSemaphoreCreateBinary();
        video->tile_enc_go = xSemaphoreCreateBinary();
        video->tile_enc_exited = xSemaphoreCreateBinary();
        video->tile_paste_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(video->slot_lock && video->capture_exited && video->raw_done && video->tile_enc_go &&
                            video->tile_enc_exited && video->tile_paste_sem, ESP_ERR_NO_MEM, TAG,
                            "no memory for capture task sync");
    }
    slot_lock(video);
    video->latest_slot = -1;
    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        video->slots[i].refs = 0;
    }
    slot_unlock(video);
    video->capture_stop = false;
    xSemaphoreTake(video->capture_exited, 0);
    xSemaphoreTake(video->tile_enc_exited, 0);
    xSemaphoreTake(video->tile_enc_go, 0);

    /* Tile scan: start from a clean schedule; the first frames have an unknown tile. */
    video->tile_fc = 0;
    video->tile_next = 0;
    video->tile_prev_us = 0;
    video->tile_mask = 0;
    video->tile_cw = 0;
    video->tile_published = false;
    video->tile_enc_idx = -1;
    video->tile_errors = 0;
    video->tile_paste_tile = -1;
    xSemaphoreTake(video->tile_paste_sem, 0);
    tile_reset_schedule(video);

    update_stream_stats_info(video);
    stream_stats_reset();

    char name[16];
    snprintf(name, sizeof(name), "capture%d", video->index);
    BaseType_t ok = xTaskCreate(capture_task, name, CAPTURE_TASK_STACK, video, CAPTURE_TASK_PRIO, &video->capture_task);
    if (ok != pdPASS) {
        video->capture_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (video->tiling.count) {
        snprintf(name, sizeof(name), "tilejpg%d", video->index);
        ok = xTaskCreate(tile_encode_task, name, TILE_ENC_TASK_STACK, video, TILE_ENC_TASK_PRIO, &video->tile_enc_task);
        if (ok != pdPASS) {
            video->tile_enc_task = NULL;
            video->capture_stop = true;
            xSemaphoreTake(video->capture_exited, pdMS_TO_TICKS(3000));
            video->capture_task = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void stop_capture_task(web_cam_video_t *video)
{
    if (!video->capture_task) {
        return;
    }
    video->capture_stop = true;
    if (xSemaphoreTake(video->capture_exited, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "video%d: capture task did not stop", video->index);
    }
    video->capture_task = NULL;
    if (video->tile_enc_task) {
        if (xSemaphoreTake(video->tile_enc_exited, pdMS_TO_TICKS(3000)) != pdTRUE) {
            ESP_LOGE(TAG, "video%d: tile encoder task did not stop", video->index);
        }
        video->tile_enc_task = NULL;
    }
}

/* Wait for a frame newer than after_seq and reference it; -1 on timeout/stop. */
static int frame_wait_newer(web_cam_video_t *video, SemaphoreHandle_t sem, uint32_t after_seq, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        int idx = frame_get_latest(video, after_seq);
        if (idx >= 0 || video->stop_stream_requested || !video->capture_task) {
            return idx;
        }
        int64_t left = deadline - esp_timer_get_time();
        if (left <= 0) {
            return -1;
        }
        xSemaphoreTake(sem, pdMS_TO_TICKS(left / 1000 < 200 ? left / 1000 + 1 : 200));
    }
}

static esp_err_t sock_send_all(int fd, const void *data, size_t len, int flags)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        int n = send(fd, p, len, flags);
        if (n <= 0) {
            return ESP_FAIL;
        }
        p += n;
        len -= n;
    }
    return ESP_OK;
}

static esp_err_t capture_video_image(httpd_req_t *req, web_cam_video_t *video, bool is_jpeg)
{
    const char *type_str = is_jpeg ? "JPEG" : "binary";
    ESP_RETURN_ON_FALSE(video->capture_task, ESP_ERR_INVALID_STATE, TAG, "camera is not streaming");

    if (!is_jpeg) {
        /* One unencoded frame (RGB565/YUV as captured), copied by the capture task. */
        slot_lock(video);
        heap_caps_free(video->raw_data);
        video->raw_data = NULL;
        xSemaphoreTake(video->raw_done, 0);
        video->raw_pending = true;
        slot_unlock(video);
        xSemaphoreTake(video->raw_done, pdMS_TO_TICKS(3000));
        slot_lock(video);
        uint8_t *data = video->raw_data;
        uint32_t len = video->raw_len;
        video->raw_data = NULL;
        video->raw_pending = false;
        slot_unlock(video);
        ESP_RETURN_ON_FALSE(data, ESP_ERR_TIMEOUT, TAG, "no frame for %s capture", type_str);
        esp_err_t ret = httpd_resp_send(req, (const char *)data, len);
        heap_caps_free(data);
        ESP_LOGD(TAG, "send %s image%d size: %" PRIu32, type_str, video->index, len);
        return ret;
    }

    /* JPEG: the next frame finished after this request (never an older one). */
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(sem, ESP_ERR_NO_MEM, TAG, "no memory");
    esp_err_t ret = ESP_ERR_TIMEOUT;
    slot_lock(video);
    uint32_t seq_now = video->latest_seq;
    slot_unlock(video);
    if (frame_waiter_add(video, sem, false) >= 0) {
        int idx = frame_wait_newer(video, sem, seq_now, 3000);
        if (idx >= 0) {
            ret = httpd_resp_send(req, (const char *)video->slots[idx].buf, video->slots[idx].len);
            ESP_LOGD(TAG, "send %s image%d size: %" PRIu32, type_str, video->index, video->slots[idx].len);
            frame_release(video, idx);
        }
        frame_waiter_remove(video, sem);
    } else {
        ret = ESP_ERR_NO_MEM;
    }
    vSemaphoreDelete(sem);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to capture %s image", type_str);
    return ESP_OK;
}

static void add_cap_json(cJSON *obj, const char *key, double cap)
{
    if (cap > 0) {
        cJSON_AddNumberToObject(obj, key, round2(cap));
    } else {
        cJSON_AddNullToObject(obj, key);
    }
}

/* Lens window a format would use (tile scan: the whole sensor). */
static bool format_window(const esp_cam_sensor_format_t *format, lens_window_t *win)
{
    bool ok = lens_window_compute(format, lens_calib_get(), win);
    if (ok && format_tiles(format) > 1) {
        ok = lens_window_full_sensor(lens_calib_get(), win->scale, win);
    }
    return ok;
}

/*
 * "fieldOfView": the share of the sensor's active area a mode shows, and how
 * ("crop": a window of the binned field; "binned": OV5647 2x2-binned window;
 * "tiled": IMX708 4-frame scan of the whole field).
 */
static cJSON *field_of_view_json(web_cam_video_t *video, const esp_cam_sensor_format_t *format, const lens_window_t *win)
{
    const lens_sensor_geometry_t *geo = lens_sensor_geometry();
    if (!geo || !win || !win->valid) {
        return cJSON_CreateNull();
    }
    cJSON *fov = cJSON_CreateObject();
    cJSON *cov = cJSON_AddObjectToObject(fov, "coverage");
    cJSON_AddNumberToObject(cov, "horizontal", round(1e4 * win->width / geo->active_width) / 1e4);
    cJSON_AddNumberToObject(cov, "vertical", round(1e4 * win->height / geo->active_height) / 1e4);
    const char *method = "crop";
    if (geo->kind == LENS_SENSOR_IMX708) {
        method = imx708_modes_method(format);
    } else if (win->scale > 1.0) {
        method = "binned";
    }
    cJSON_AddStringToObject(fov, "method", method);
    return fov;
}

static char *get_cameras_json(web_cam_t *web_cam)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cameras = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "cameras", cameras);

    for (int i = 0; i < web_cam->video_count; i++) {
        char src_str[96];
        char fps_str[16];

        if (!is_valid_web_cam(&web_cam->video[i])) {
            continue;
        }

        bool auto_brightness_enabled = true;
        bool supports_auto_brightness = camera_auto_brightness_get(&web_cam->video[i], &auto_brightness_enabled);

        cJSON *camera = cJSON_CreateObject();
        cJSON_AddNumberToObject(camera, "index", i);
        esp_cam_sensor_device_t *sensor = camera_sensor(&web_cam->video[i]);
        if (sensor && sensor->name) {
            cJSON_AddStringToObject(camera, "sensorName", sensor->name);
            cJSON_AddNumberToObject(camera, "sensorPid", sensor->id.pid);
        }
        cJSON_AddBoolToObject(camera, "supportsAutoBrightness", supports_auto_brightness);
        cJSON_AddBoolToObject(camera, "autoBrightnessEnabled", auto_brightness_enabled);
        assert(snprintf(src_str, sizeof(src_str), ":%d/stream", i + 81) > 0);
        cJSON_AddStringToObject(camera, "src", src_str);
        web_cam_video_t *v = &web_cam->video[i];
        /* The sensor's actual rate (with the sensor cap); images = sensor / framesPerImage. */
        cJSON_AddNumberToObject(camera, "currentFrameRate", round2(v->sensor_fps));
        cJSON *frame_rate = cJSON_AddObjectToObject(camera, "frameRate");
        add_cap_json(frame_rate, "sensorCap", v->sensor_cap);
        add_cap_json(frame_rate, "streamCap", v->stream_cap);
        cJSON_AddNumberToObject(frame_rate, "sensorFps", round2(v->sensor_fps));
        cJSON_AddNumberToObject(frame_rate, "modeMaxFps", round2(v->mode_max_fps));
        cJSON_AddNumberToObject(frame_rate, "framesPerImage", v->tiling.count ? v->tiling.count : 1);
        cJSON_AddNumberToObject(frame_rate, "imageFps", round2(video_image_fps(v)));
        cJSON_AddNumberToObject(frame_rate, "streamFps", round2(video_stream_fps(v)));
        cJSON_AddNumberToObject(frame_rate, "capMin", FPS_CAP_MIN);
        int current_format_index = current_capture_format_index(&web_cam->video[i]);
        cJSON_AddNumberToObject(camera, "currentImageFormat", current_format_index >= 0 ? current_format_index : 0);
        assert(snprintf(src_str, sizeof(src_str), "JPEG %" PRIu32 "x%" PRIu32, v->img_width, v->img_height) > 0);
        cJSON_AddStringToObject(camera, "currentImageFormatDescription", src_str);

        if (web_cam->video[i].support_control_jpeg_quality) {
            cJSON_AddNumberToObject(camera, "currentQuality", web_cam->video[i].jpeg_quality);
        }

        cJSON *image_controls = cJSON_CreateArray();
        for (size_t j = 0; j < sizeof(s_camera_image_controls) / sizeof(s_camera_image_controls[0]); j++) {
            const camera_image_control_t *setting = &s_camera_image_controls[j];
            int fd;
            struct v4l2_query_ext_ctrl qctrl;
            if (!query_camera_image_control(&web_cam->video[i], setting, &fd, &qctrl)) {
                continue;
            }

            int value = qctrl.default_value;
            if (!get_camera_image_control_value(&web_cam->video[i], j, fd, &value)) {
                web_cam->video[i].image_control_values[j] = value;
                web_cam->video[i].image_control_values_valid |= (1U << j);
            }

            cJSON *image_control = cJSON_CreateObject();
            cJSON_AddStringToObject(image_control, "key", setting->key);
            cJSON_AddStringToObject(image_control, "label", setting->label);
            cJSON_AddNumberToObject(image_control, "min", qctrl.minimum);
            cJSON_AddNumberToObject(image_control, "max", qctrl.maximum);
            cJSON_AddNumberToObject(image_control, "step", qctrl.step);
            cJSON_AddNumberToObject(image_control, "default", qctrl.default_value);
            cJSON_AddNumberToObject(image_control, "value", value);
            cJSON_AddItemToArray(image_controls, image_control);
        }
        cJSON_AddItemToObject(camera, "imageControls", image_controls);

        cJSON *current_resolution = cJSON_CreateObject();
        cJSON_AddNumberToObject(current_resolution, "width", v->img_width);
        cJSON_AddNumberToObject(current_resolution, "height", v->img_height);
        cJSON_AddItemToObject(camera, "currentResolution", current_resolution);

        bool lens_windows = camera_lens_windows_supported(&web_cam->video[i]);
        if (lens_windows) {
            cJSON_AddItemToObject(camera, "sensor", lens_sensor_json());
            cJSON_AddItemToObject(camera, "lensCalibration", lens_calibration_json(lens_calib_get()));
            lens_window_t current_window = web_cam->video[i].applied_window;
            if (!current_window.valid && current_format_index >= 0) {
                struct v4l2_sensor_format_enum sensor_enum = { .index = (uint32_t)current_format_index };
                if (enum_sensor_format(&web_cam->video[i], &sensor_enum) == 0) {
                    format_window(&sensor_enum.format, &current_window);
                }
            }
            cJSON_AddItemToObject(camera, "window", lens_window_json(&current_window));
        }

        cJSON *image_formats = cJSON_CreateArray();
        for (uint32_t format_index = 0; ; format_index++) {
            struct v4l2_sensor_format_enum sensor_enum = { .index = format_index };
            if (enum_sensor_format(&web_cam->video[i], &sensor_enum) != 0) {
                break;
            }

            const esp_cam_sensor_format_t *sensor_format = &sensor_enum.format;
            cJSON *image_format = cJSON_CreateObject();
            cJSON_AddNumberToObject(image_format, "id", format_index);
            uint32_t img_w, img_h;
            format_image_size(sensor_format, &img_w, &img_h);
            int tiles = format_tiles(sensor_format);
            double max_fps = format_max_fps(v, sensor_format);
            format_fps(fps_str, sizeof(fps_str), max_fps / tiles);
            assert(snprintf(src_str, sizeof(src_str), "%" PRIu32 "x%" PRIu32 " %s @%s fps%s", img_w, img_h,
                            sensor_format_name(sensor_format->format), fps_str,
                            tiles > 1 ? " (4-frame scan)" : "") > 0);
            cJSON_AddStringToObject(image_format, "description", src_str);
            cJSON_AddNumberToObject(image_format, "width", img_w);
            cJSON_AddNumberToObject(image_format, "height", img_h);
            /* Highest sensor rate of the mode (range of the sensor cap); images = maxFps / framesPerImage. */
            cJSON_AddNumberToObject(image_format, "maxFps", round2(max_fps));
            if (tiles > 1) {
                cJSON_AddNumberToObject(image_format, "framesPerImage", tiles);
            }

            if (web_cam->video[i].support_control_jpeg_quality) {
                cJSON *image_format_quality = cJSON_CreateObject();
                cJSON_AddNumberToObject(image_format_quality, "min", 1);
                cJSON_AddNumberToObject(image_format_quality, "max", tiles > 1 ? TILE_JPEG_QUALITY_MAX : 100);
                cJSON_AddNumberToObject(image_format_quality, "step", 1);
                int default_quality = default_jpeg_quality(img_w, img_h, max_fps / tiles);
                cJSON_AddNumberToObject(image_format_quality, "default", default_quality);
                if (default_quality != EXAMPLE_JPEG_ENC_QUALITY) {
                    char note[96];
                    snprintf(note, sizeof(note), "Default %d at this size keeps %s fps within 100 Mbit/s Ethernet",
                             default_quality, fps_str);
                    cJSON_AddStringToObject(image_format_quality, "note", note);
                }
                cJSON_AddItemToObject(image_format, "quality", image_format_quality);
            }
            if (lens_windows) {
                lens_window_t fwin;
                format_window(sensor_format, &fwin);
                cJSON_AddItemToObject(image_format, "window", lens_window_json(&fwin));
                cJSON_AddItemToObject(image_format, "fieldOfView", field_of_view_json(v, sensor_format, &fwin));
            }
            cJSON_AddItemToArray(image_formats, image_format);
        }
        cJSON_AddItemToObject(camera, "imageFormats", image_formats);
        cJSON_AddItemToArray(cameras, camera);
    }

    char *output = cJSON_Print(root);
    cJSON_Delete(root);
    return output;
}

static esp_err_t set_camera_jpeg_quality(web_cam_video_t *video, int quality)
{
    esp_err_t ret = ESP_OK;
    int quality_reset = quality;

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        struct v4l2_ext_controls controls = {0};
        struct v4l2_ext_control control[1];
        struct v4l2_query_ext_ctrl qctrl = {0};

        qctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
        if (ioctl(video->fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) == 0) {
            if ((quality > qctrl.maximum) || (quality < qctrl.minimum) ||
                    (((quality - qctrl.minimum) % qctrl.step) != 0)) {

                if (quality > qctrl.maximum) {
                    quality_reset = qctrl.maximum;
                } else if (quality < qctrl.minimum) {
                    quality_reset = qctrl.minimum;
                } else {
                    quality_reset = qctrl.minimum + ((quality - qctrl.minimum) / qctrl.step) * qctrl.step;
                }

                ESP_LOGW(TAG, "video%d: JPEG compression quality=%d is out of sensor's range, reset to %d", video->index, quality, quality_reset);
            }

            controls.ctrl_class = V4L2_CID_JPEG_CLASS;
            controls.count = 1;
            controls.controls = control;
            control[0].id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
            control[0].value = quality_reset;
            ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_S_EXT_CTRLS, &controls), TAG, "failed to set jpeg compression quality");

            video->jpeg_quality = quality_reset;
            video->support_control_jpeg_quality = 1;
        } else {
            video->support_control_jpeg_quality = 0;
            ESP_LOGW(TAG, "video%d: JPEG compression quality control is not supported", video->index);
        }
    } else {
        if (video->tiling.count && quality_reset > TILE_JPEG_QUALITY_MAX) {
            /* Full-view scan: its JPEG slots are 1 B/px; see TILE_JPEG_QUALITY_MAX. */
            ESP_LOGW(TAG, "video%d: JPEG quality %d limited to %d in the full-view mode", video->index, quality_reset,
                     TILE_JPEG_QUALITY_MAX);
            quality_reset = TILE_JPEG_QUALITY_MAX;
        }
        if (video->sem) {
            xSemaphoreTake(video->sem, portMAX_DELAY);
        }
        ret = example_encoder_set_jpeg_quality(video->encoder_handle, quality_reset);
        if (video->sem) {
            xSemaphoreGive(video->sem);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "failed to set jpeg quality");
        video->jpeg_quality = quality_reset;
    }

    if (video->support_control_jpeg_quality) {
        ESP_LOGI(TAG, "video%d: set jpeg quality %d success", video->index, quality_reset);
    }
    update_stream_stats_info(video);

    return ret;
}

static esp_err_t camera_info_handler(httpd_req_t *req)
{
    esp_err_t ret;
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;
    char *output = get_cameras_json(web_cam);

    httpd_resp_set_type(req, "application/json");
    ret = httpd_resp_sendstr(req, output);
    free(output);

    return ret;
}

static esp_err_t reconfigure_video_format(web_cam_video_t *video, int format_index);
static void restore_camera_user_settings(web_cam_video_t *video, uint8_t jpeg_quality);

/* "sensor_fps_cap" / "stream_fps_cap": a number in [1, 120] or null (no cap); absent = unchanged. */
static esp_err_t parse_fps_cap(const cJSON *root, const char *key, bool *present, double *value)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(root, key);
    *present = j != NULL;
    if (!j) {
        return ESP_OK;
    }
    if (cJSON_IsNull(j)) {
        *value = 0;
        return ESP_OK;
    }
    if (!cJSON_IsNumber(j) || !isfinite(j->valuedouble) || j->valuedouble < FPS_CAP_MIN || j->valuedouble > FPS_CAP_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *value = round(j->valuedouble * 1000.0) / 1000.0;
    return ESP_OK;
}

static esp_err_t camera_settings_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_FAIL;
    char *content;
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    content = (char *)calloc(1, req->content_len + 1);
    ESP_RETURN_ON_FALSE(content, ESP_ERR_NO_MEM, TAG, "failed to allocate memory");

    ESP_GOTO_ON_FALSE(httpd_req_recv(req, content, req->content_len) > 0, ESP_FAIL, fail0, TAG, "failed to recv content");
    ESP_LOGD(TAG, "content: %s", content);

    cJSON *json_root = cJSON_Parse(content);
    free(content);
    content = NULL;
    ESP_GOTO_ON_FALSE(json_root, ESP_FAIL, fail0, TAG, "failed to parse JSON");

    cJSON *json_index = cJSON_GetObjectItem(json_root, "index");
    ESP_GOTO_ON_FALSE(json_index && cJSON_IsNumber(json_index), ESP_ERR_INVALID_ARG, fail1, TAG, "missing or invalid index field");
    int index = json_index->valueint;
    ESP_GOTO_ON_FALSE(index >= 0 && index < web_cam->video_count && is_valid_web_cam(&web_cam->video[index]), ESP_ERR_INVALID_ARG, fail1, TAG, "invalid index");

    cJSON *json_image_format = cJSON_GetObjectItem(json_root, "image_format");
    ESP_GOTO_ON_FALSE(json_image_format && cJSON_IsNumber(json_image_format), ESP_ERR_INVALID_ARG, fail1, TAG, "missing or invalid image_format field");
    int image_format = json_image_format->valueint;

    cJSON *json_jpeg_quality = cJSON_GetObjectItem(json_root, "jpeg_quality");
    ESP_GOTO_ON_FALSE(json_jpeg_quality && cJSON_IsNumber(json_jpeg_quality), ESP_ERR_INVALID_ARG, fail1, TAG, "missing or invalid jpeg_quality field");
    int jpeg_quality = json_jpeg_quality->valueint;

    cJSON *json_image_controls = cJSON_GetObjectItemCaseSensitive(json_root, "image_controls");
    cJSON *json_auto_brightness = cJSON_GetObjectItemCaseSensitive(json_root, "auto_brightness_enabled");

    web_cam_video_t *video = &web_cam->video[index];
    bool sc_present = false, tc_present = false;
    double sc = 0, tc = 0;
    ESP_GOTO_ON_ERROR(parse_fps_cap(json_root, "sensor_fps_cap", &sc_present, &sc), fail1, TAG,
                      "invalid sensor_fps_cap (number 1..120 or null)");
    ESP_GOTO_ON_ERROR(parse_fps_cap(json_root, "stream_fps_cap", &tc_present, &tc), fail1, TAG,
                      "invalid stream_fps_cap (number 1..120 or null)");

    ESP_LOGI(TAG, "JSON parse success - index:%d, image_format:%d, jpeg_quality:%d", index, image_format, jpeg_quality);
    ESP_GOTO_ON_FALSE(image_format >= 0, ESP_ERR_INVALID_ARG, fail1, TAG, "invalid image format index");

    /* Caps first, so a mode change applies the new sensor cap in one go. */
    double old_sc = video->sensor_cap, old_tc = video->stream_cap;
    bool sc_changed = sc_present && fabs(sc - old_sc) > 1e-6;
    bool tc_changed = tc_present && fabs(tc - old_tc) > 1e-6;
    video->sensor_cap = sc_present ? sc : old_sc;
    video->stream_cap = tc_present ? tc : old_tc;
    if (image_format != current_capture_format_index(video)) {
        ret = reconfigure_video_format(video, image_format);
        if (ret != ESP_OK) {
            video->sensor_cap = old_sc;
            video->stream_cap = old_tc;
            apply_sensor_frame_rate(video);
            ESP_LOGE(TAG, "failed to change camera capture size");
            goto fail1;
        }
    } else if (sc_changed) {
        const lens_sensor_geometry_t *geo = lens_sensor_geometry();
        if (geo && geo->kind == LENS_SENSOR_OV5647) {
            /* OV5647: the cap lives in the mode table (VTS), so the mode is re-applied. */
            uint8_t q = video->jpeg_quality;
            ret = reconfigure_video_format(video, image_format);
            if (ret != ESP_OK) {
                video->sensor_cap = old_sc;
                ESP_LOGE(TAG, "failed to apply the sensor frame-rate cap");
                goto fail1;
            }
            restore_camera_user_settings(video, q);
        } else {
            apply_sensor_frame_rate(video);     /* IMX708: live frame-length change */
        }
    }
    if (sc_changed || tc_changed) {
        update_stream_stats_info(video);
        if (fps_caps_save(video) != ESP_OK) {
            ESP_LOGW(TAG, "video%d: failed to save the frame-rate caps", index);
        }
        ESP_LOGI(TAG, "video%d: frame-rate caps: sensor %.3g, stream %.3g fps (0 = none); sensor runs at %.3f fps",
                 index, video->sensor_cap, video->stream_cap, video->sensor_fps);
    }
    ESP_GOTO_ON_ERROR(set_camera_jpeg_quality(&web_cam->video[index], jpeg_quality), fail1, TAG, "failed to set camera jpeg quality");
    ESP_GOTO_ON_ERROR(set_camera_image_controls(&web_cam->video[index], json_image_controls), fail1, TAG, "failed to set camera image controls");
    if (json_auto_brightness) {
        ESP_GOTO_ON_FALSE(cJSON_IsBool(json_auto_brightness), ESP_ERR_INVALID_ARG, fail1, TAG,
                          "invalid auto_brightness_enabled field");
        ESP_GOTO_ON_ERROR(camera_auto_brightness_set(&web_cam->video[index], cJSON_IsTrue(json_auto_brightness)),
                          fail1, TAG, "failed to set automatic brightness");
    }
    cJSON_Delete(json_root);
    json_root = NULL;

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;

fail1:
    if (json_root) {
        cJSON_Delete(json_root);
    }
fail0:
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
    }
    if (content) {
        free(content);
    }
    return ret;
}

/* Re-apply JPEG quality and the cached image controls after a mode re-apply
 * (the sensor driver resets e.g. the AE target when it loads a mode). */
static void restore_camera_user_settings(web_cam_video_t *video, uint8_t jpeg_quality)
{
    if (set_camera_jpeg_quality(video, jpeg_quality) != ESP_OK) {
        ESP_LOGW(TAG, "video%d: failed to restore JPEG quality", video->index);
    }
    for (size_t i = 0; i < sizeof(s_camera_image_controls) / sizeof(s_camera_image_controls[0]); i++) {
        if (!(video->image_control_values_valid & (1U << i))) {
            continue;
        }
        int fd;
        struct v4l2_query_ext_ctrl qctrl;
        if (!query_camera_image_control(video, &s_camera_image_controls[i], &fd, &qctrl)) {
            continue;
        }
        struct v4l2_ext_control control = { .id = s_camera_image_controls[i].id,
                                            .value = video->image_control_values[i] };
        struct v4l2_ext_controls controls = { .ctrl_class = V4L2_CID_USER_CLASS, .count = 1, .controls = &control };
        if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
            ESP_LOGW(TAG, "video%d: failed to restore %s", video->index, s_camera_image_controls[i].key);
        }
    }
}

static bool json_get_finite(const cJSON *obj, const char *key, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

/*
 * POST /api/set_lens_calibration
 *   {"index":0, "center":{"x":num,"y":num}, "distortion": null | {fx,fy,k1,k2,p1,p2,k3}}
 *   {"index":0, "reset":true}
 * Validates, re-applies the current sensor mode with the new window (same
 * stream stop/restart path as a format change) and then saves to NVS.
 */
static esp_err_t lens_calibration_handler(httpd_req_t *req)
{
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;
    const char *error = NULL;
    static char error_buf[96];
    int status_500 = 0;
    cJSON *root = NULL;
    char *content = NULL;

    if (req->content_len == 0 || req->content_len > 2048) {
        error = "invalid request body size";
        goto done;
    }
    content = calloc(1, req->content_len + 1);
    if (!content) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, content + received, req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(content);
            return ESP_FAIL;
        }
        received += r;
    }

    root = cJSON_Parse(content);
    if (!root || !cJSON_IsObject(root)) {
        error = "invalid JSON";
        goto done;
    }

    const cJSON *json_index = cJSON_GetObjectItemCaseSensitive(root, "index");
    if (!cJSON_IsNumber(json_index)) {
        error = "missing or invalid index";
        goto done;
    }
    int index = json_index->valueint;
    if (index < 0 || index >= web_cam->video_count || !is_valid_web_cam(&web_cam->video[index])) {
        error = "invalid camera index";
        goto done;
    }
    web_cam_video_t *video = &web_cam->video[index];
    if (!camera_lens_windows_supported(video)) {
        error = "lens calibration is not supported for this camera";
        goto done;
    }

    lens_calibration_t cal;
    const cJSON *json_reset = cJSON_GetObjectItemCaseSensitive(root, "reset");
    if (json_reset && !cJSON_IsBool(json_reset)) {
        error = "reset must be a boolean";
        goto done;
    }
    if (cJSON_IsTrue(json_reset)) {
        lens_calib_get_default(&cal);
    } else {
        const cJSON *center = cJSON_GetObjectItemCaseSensitive(root, "center");
        if (!cJSON_IsObject(center) || !json_get_finite(center, "x", &cal.cx) || !json_get_finite(center, "y", &cal.cy)) {
            error = "center must be an object with finite numbers x and y";
            goto done;
        }
        const lens_sensor_geometry_t *geo = lens_sensor_geometry();
        if (cal.cx < geo->active_x || cal.cx > geo->active_x + geo->active_width ||
                cal.cy < geo->active_y || cal.cy > geo->active_y + geo->active_height) {
            snprintf(error_buf, sizeof(error_buf), "center is outside the %s active area (x %d..%d, y %d..%d)",
                     geo->name, geo->active_x, geo->active_x + geo->active_width,
                     geo->active_y, geo->active_y + geo->active_height);
            error = error_buf;
            goto done;
        }
        cal.calibrated = true;
        cal.has_distortion = false;
        memset(&cal.distortion, 0, sizeof(cal.distortion));
        const cJSON *dist = cJSON_GetObjectItemCaseSensitive(root, "distortion");
        if (dist && !cJSON_IsNull(dist)) {
            lens_distortion_t *d = &cal.distortion;
            if (!cJSON_IsObject(dist) ||
                    !json_get_finite(dist, "fx", &d->fx) || !json_get_finite(dist, "fy", &d->fy) ||
                    !json_get_finite(dist, "k1", &d->k1) || !json_get_finite(dist, "k2", &d->k2) ||
                    !json_get_finite(dist, "p1", &d->p1) || !json_get_finite(dist, "p2", &d->p2) ||
                    !json_get_finite(dist, "k3", &d->k3)) {
                error = "distortion must be null or an object with finite numbers fx, fy, k1, k2, p1, p2, k3";
                goto done;
            }
            if (d->fx <= 0 || d->fy <= 0) {
                error = "distortion fx and fy must be positive";
                goto done;
            }
            cal.has_distortion = true;
        }
    }

    int format_index = current_capture_format_index(video);
    if (format_index < 0) {
        error = "current sensor mode is unknown";
        status_500 = 1;
        goto done;
    }

    lens_calibration_t previous = *lens_calib_get();
    uint8_t saved_quality = video->jpeg_quality;
    lens_calib_set(&cal);
    esp_err_t ret = reconfigure_video_format(video, format_index);
    if (ret == ESP_OK) {
        /* Re-applying the mode resets the sensor; keep the user's settings. */
        restore_camera_user_settings(video, saved_quality);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video%d: failed to apply lens calibration (%s); reverting", index, esp_err_to_name(ret));
        lens_calib_set(&previous);
        if (is_valid_web_cam(video) && reconfigure_video_format(video, format_index) != ESP_OK) {
            ESP_LOGE(TAG, "video%d: failed to restore previous lens window", index);
        }
        error = "failed to apply the new sensor window";
        status_500 = 1;
        goto done;
    }
    if (lens_calib_save() != ESP_OK) {
        error = "window applied but saving to NVS failed";
        status_500 = 1;
        goto done;
    }
    ESP_LOGI(TAG, "video%d: lens centre set to (%.2f, %.2f) [%s], window error (%.1f, %.1f)", index,
             cal.cx, cal.cy, cal.calibrated ? "calibrated" : "default",
             video->applied_window.err_x, video->applied_window.err_y);

done:
    if (root) {
        cJSON_Delete(root);
    }
    if (content) {
        free(content);
    }
    if (error) {
        ESP_LOGW(TAG, "set_lens_calibration: %s", error);
        httpd_resp_send_err(req, status_500 ? HTTPD_500_INTERNAL_SERVER_ERROR : HTTPD_400_BAD_REQUEST, error);
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    /* Route to appropriate static file based on URI */
    if (strcmp(uri, "/") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
    } else if (strcmp(uri, "/loading.jpg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)loading_jpg_gz_start, loading_jpg_gz_end - loading_jpg_gz_start);
    } else if (strcmp(uri, "/favicon.ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)favicon_ico_gz_start, favicon_ico_gz_end - favicon_ico_gz_start);
    } else if (strcmp(uri, "/assets/index.js") == 0) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_js_gz_start, assets_index_js_gz_end - assets_index_js_gz_start);
    } else if (strcmp(uri, "/assets/index.css") == 0) {
        httpd_resp_set_type(req, "text/css");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_css_gz_start, assets_index_css_gz_end - assets_index_css_gz_start);
    } else if (strcmp(uri, "/overlays/Master_overlay.png") == 0) {
        /* httpd_resp_send() loops until the whole (~800 KB) buffer is written, with a Content-Length header. */
        httpd_resp_set_type(req, "image/png");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
        return httpd_resp_send(req, (const char *)overlays_master_png_gz_start, overlays_master_png_gz_end - overlays_master_png_gz_start);
    } else if (strcmp(uri, "/overlays/Blank_overlay.png") == 0) {
        httpd_resp_set_type(req, "image/png");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
        return httpd_resp_send(req, (const char *)overlays_blank_png_gz_start, overlays_blank_png_gz_end - overlays_blank_png_gz_start);
    }

    /* If no static file matches, return 404 */
    ESP_LOGW(TAG, "File not found: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t image_stream_handler(httpd_req_t *req)
{
    web_cam_video_t *video = (web_cam_video_t *)req->user_ctx;
    int fd = httpd_req_to_sockfd(req);
    char part[320];
    esp_err_t ret = ESP_OK;

    /* Each part is written as two sends; don't let Nagle hold the tail of a
     * frame back waiting for the client's (delayed) ACK. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(sem, ESP_ERR_NO_MEM, TAG, "no memory");
    int wi = frame_waiter_add(video, sem, true);
    if (wi < 0) {
        vSemaphoreDelete(sem);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "too many viewers");
        return ESP_FAIL;
    }

    /* X-Framerate: what this client will receive (image rate, limited by the stream cap). */
    char fps_str[16];
    format_fps(fps_str, sizeof(fps_str), video_stream_fps(video));
    int hlen = snprintf(part, sizeof(part), STREAM_RESPONSE_HEADER, fps_str);
    if (sock_send_all(fd, part, hlen, 0) != ESP_OK) {
        ret = ESP_FAIL;
    }

    uint32_t last_seq = 0;
    while (ret == ESP_OK && !video->stop_stream_requested) {
        int64_t t_wait = esp_timer_get_time();
        int idx = frame_wait_newer(video, sem, last_seq, 1000);
        if (idx < 0) {
            if (!video->capture_task) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            continue;
        }
        const jpeg_slot_t *slot = &video->slots[idx];
        /* Stream cap: send only frames on the cap's time grid, drop the rest evenly. */
        slot_lock(video);
        bool due = waiter_due(video, &video->waiters[wi], slot->ts_us);
        if (due) {
            waiter_take(video, &video->waiters[wi], slot->ts_us);
        }
        slot_unlock(video);
        if (!due) {
            last_seq = slot->seq;
            frame_release(video, idx);
            stream_stats_cap_skip();
            continue;
        }
        uint32_t skipped = (last_seq && slot->seq > last_seq + 1) ? slot->seq - last_seq - 1 : 0;
        int64_t t_send = esp_timer_get_time();
        hlen = snprintf(part, sizeof(part), STREAM_PART, slot->len,
                        (long long)(slot->ts_us / 1000000), (long long)(slot->ts_us % 1000000));
        if (sock_send_all(fd, part, hlen, MSG_MORE) != ESP_OK ||
                sock_send_all(fd, slot->buf, slot->len, 0) != ESP_OK) {
            ret = ESP_FAIL;
        }
        int64_t t_done = esp_timer_get_time();
        last_seq = slot->seq;
        uint32_t bytes = slot->len + hlen;
        frame_release(video, idx);
        if (ret == ESP_OK) {
            stream_stats_sent((uint32_t)(t_send - t_wait), (uint32_t)(t_done - t_send), bytes, skipped);
        }
    }

    frame_waiter_remove(video, sem);
    vSemaphoreDelete(sem);
    /* The response was written raw and has no length: end it by closing. */
    httpd_sess_trigger_close(req->handle, fd);
    return ESP_OK;
}

static esp_err_t capture_image_handler(httpd_req_t *req)
{
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    request_desc_t desc;
    ESP_RETURN_ON_ERROR(decode_request(web_cam, req, &desc), TAG, "failed to decode request");

    char type_ptr[32];
    ESP_RETURN_ON_FALSE(snprintf(type_ptr, sizeof(type_ptr), "image/jpeg;name=image%d.jpg", desc.index) > 0, ESP_FAIL, TAG, "failed to format buffer");
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, type_ptr), TAG, "failed to set content type");

    return capture_video_image(req, &web_cam->video[desc.index], true);
}

static esp_err_t capture_binary_handler(httpd_req_t *req)
{
    web_cam_t *web_cam = (web_cam_t *)req->user_ctx;

    request_desc_t desc;
    ESP_RETURN_ON_ERROR(decode_request(web_cam, req, &desc), TAG, "failed to decode request");

    char type_ptr[56];
    ESP_RETURN_ON_FALSE(snprintf(type_ptr, sizeof(type_ptr), "application/octet-stream;name=image_binary%d.bin", desc.index) > 0, ESP_FAIL, TAG, "failed to format buffer");
    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, type_ptr), TAG, "failed to set content type");

    return capture_video_image(req, &web_cam->video[desc.index], false);
}

static esp_err_t deinit_web_cam_video(web_cam_video_t *video)
{
    if (video->sem) {
        vSemaphoreDelete(video->sem);
        video->sem = NULL;
    }

    for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
        if (video->slots[i].buf) {
            if (video->encoder_handle) {
                example_encoder_free_output_buffer(video->encoder_handle, video->slots[i].buf);
            } else {
                heap_caps_free(video->slots[i].buf);
            }
        }
        memset(&video->slots[i], 0, sizeof(video->slots[i]));
    }
    video->latest_slot = -1;

    if (video->encoder_handle) {
        example_encoder_deinit(video->encoder_handle);
        video->encoder_handle = NULL;
    }

    if (video->fd >= 0) {
        for (int i = 0; i < EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER; i++) {
            if (video->buffer[i] && video->buffer[i] != MAP_FAILED && video->buffer_length[i]) {
                munmap(video->buffer[i], video->buffer_length[i]);
            }
            video->buffer[i] = NULL;
            video->buffer_length[i] = 0;
        }
        struct v4l2_requestbuffers req = {
            .count = 0,
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        ioctl(video->fd, VIDIOC_REQBUFS, &req);
        close(video->fd);
        video->fd = -1;
    }

    if (video->isp_fd >= 0) {
        close(video->isp_fd);
        video->isp_fd = -1;
    }
    for (int i = 0; i < 2; i++) {
        heap_caps_free(video->canvas[i]);
        video->canvas[i] = NULL;
    }
    video->canvas_size = 0;
    memset(&video->tiling, 0, sizeof(video->tiling));
    isp_hook_set_stats_average(1);
    video->buffer_size = 0;
    video->width = 0;
    video->height = 0;
    video->img_width = 0;
    video->img_height = 0;
    video->pixel_format = 0;
    video->frame_rate = 0;
    video->sensor_fps = 0;
    video->mode_max_fps = 0;
    video->support_control_jpeg_quality = 0;
    return ESP_OK;
}

static esp_err_t init_web_cam_video(web_cam_video_t *video, const web_cam_video_config_t *config, int index,
                                    int sensor_format_index, uint32_t output_width, uint32_t output_height)
{
    int ret = ESP_OK;
    struct v4l2_format format = {0};
    struct v4l2_streamparm sparm = {0};
    struct v4l2_requestbuffers req = {0};
    struct v4l2_captureparm *cparam = &sparm.parm.capture;
    struct v4l2_fract *timeperframe = &cparam->timeperframe;

    video->fd = -1;
    video->isp_fd = -1;
    video->index = index;
    video->dev_name = config->dev_name;
    video->fd = open(config->dev_name, O_RDWR);
    ESP_GOTO_ON_FALSE(video->fd >= 0, ESP_ERR_NOT_FOUND, fail, TAG, "Open video device %s failed", config->dev_name);
    {
        /* Name the sensor to the IPA hook before the ISP pipeline sees a frame. */
        esp_cam_sensor_device_t *sensor = camera_sensor(video);
        video->sensor_dev = sensor;
        if (sensor && sensor->name) {
            isp_hook_set_sensor(sensor->name);
            if (strcmp(sensor->name, "IMX708") == 0) {
                imx708_modes_init();
            }
        }
        /* Frame-rate caps of this sensor, before its first mode is applied. */
        if (!video->caps_loaded) {
            fps_caps_load(video);
            video->caps_loaded = true;
        }
    }

    if (sensor_format_index < 0 && camera_lens_windows_supported(video)) {
        /* Boot: re-apply the driver's default mode so its window is lens-centred too. */
        esp_cam_sensor_format_t current = {0};
        if (ioctl(video->fd, VIDIOC_G_SENSOR_FMT, &current) == 0) {
            sensor_format_index = find_sensor_format_index(video, &current);
        }
    }

    if (sensor_format_index >= 0) {
        struct v4l2_sensor_format_enum sensor_enum = { .index = (uint32_t)sensor_format_index };
        ESP_GOTO_ON_ERROR(enum_sensor_format(video, &sensor_enum), fail, TAG,
                          "unsupported sensor format index %d", sensor_format_index);
        ESP_GOTO_ON_ERROR(apply_lens_centred_sensor_format(video, &sensor_enum.format), fail, TAG,
                          "failed to switch sensor format to %ux%u", sensor_enum.format.width, sensor_enum.format.height);
        if (!imx708_modes_tiling(&sensor_enum.format, &video->tiling)) {
            memset(&video->tiling, 0, sizeof(video->tiling));
        }

        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_G_FMT, &format), fail, TAG, "Failed get fmt from %s", config->dev_name);
        format.fmt.pix.width = output_width ? output_width : sensor_enum.format.width;
        format.fmt.pix.height = output_height ? output_height : sensor_enum.format.height;
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_S_FMT, &format), fail, TAG,
                          "failed to set capture format to %ux%u", format.fmt.pix.width, format.fmt.pix.height);
        ESP_GOTO_ON_FALSE(format.fmt.pix.width == (output_width ? output_width : sensor_enum.format.width) &&
                          format.fmt.pix.height == (output_height ? output_height : sensor_enum.format.height),
                          ESP_ERR_NOT_SUPPORTED, fail, TAG, "capture size not available for this sensor mode");
    } else {
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_G_FMT, &format), fail, TAG, "Failed get fmt from %s", config->dev_name);
    }

#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565X) {
#if CONFIG_ESP_VIDEO_ENABLE_SWAP_BYTE
        ESP_LOGW(TAG, "The hardware JPEG encoder does not support RGB565 big endian. Instead, use RGB565 little endian by enabling the byte swap function.");
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_S_FMT, &format), fail, TAG, "failed to set fmt to %s", config->dev_name);
#else
        ESP_GOTO_ON_ERROR(ESP_FAIL, fail, TAG, "The hardware JPEG encoder does not support RGB565 big endian. Please enable the byte swap function ESP_VIDEO_ENABLE_SWAP_BYTE in menuconfig.");
#endif
    }
#endif

    video->width = format.fmt.pix.width;
    video->height = format.fmt.pix.height;
    video->pixel_format = format.fmt.pix.pixelformat;
    video->img_width = video->width;
    video->img_height = video->height;
    if (video->tiling.count) {
        ESP_GOTO_ON_FALSE(video->pixel_format == V4L2_PIX_FMT_RGB565 &&
                          video->width == (uint32_t)video->tiling.tile_w && video->height == (uint32_t)video->tiling.tile_h,
                          ESP_ERR_NOT_SUPPORTED, fail, TAG, "tile scan needs RGB565 frames of the tile size");
        video->img_width = video->tiling.canvas_w;
        video->img_height = video->tiling.canvas_h;
        video->canvas_size = video->img_width * video->img_height * 2;
        for (int i = 0; i < 2; i++) {
            video->canvas[i] = heap_caps_aligned_calloc(128, 1, video->canvas_size, MALLOC_CAP_SPIRAM);
            ESP_GOTO_ON_FALSE(video->canvas[i], ESP_ERR_NO_MEM, fail, TAG, "no memory for the full-view canvas");
        }
    }
    /* AE/AWB of the tile scan see the mean of the last four tiles = the whole scene. */
    isp_hook_set_stats_average(video->tiling.count ? video->tiling.count : 1);

    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_G_PARM, &sparm), fail, TAG, "failed to get frame rate from %s", config->dev_name);
    if (timeperframe->numerator && video->sensor_fps <= 0) {
        /* Not set by apply_lens_centred_sensor_format (boot without a mode change, other sensors). */
        video->frame_rate = timeperframe->denominator / timeperframe->numerator;
        video->sensor_fps = video->mode_max_fps = video->frame_rate;
    }

#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CROP
    struct v4l2_selection selection = {0};
    selection.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selection.target = V4L2_SEL_TGT_CROP;
    selection.r.left = CONFIG_EXAMPLE_MIPI_CSI_CROP_LEFT;
    selection.r.width = CONFIG_EXAMPLE_MIPI_CSI_CROP_WIDTH;
    selection.r.top = CONFIG_EXAMPLE_MIPI_CSI_CROP_TOP;
    selection.r.height = CONFIG_EXAMPLE_MIPI_CSI_CROP_HEIGHT;
    if (ioctl(video->fd, VIDIOC_S_SELECTION, &selection) != 0) {
        ESP_LOGE(TAG, "failed to set selection");
    }
#endif

    req.count = EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_REQBUFS, &req), fail, TAG, "failed to req buffers from %s", config->dev_name);

    for (int i = 0; i < EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_QUERYBUF, &buf), fail, TAG, "failed to query vbuf from %s", config->dev_name);

        video->buffer_length[i] = buf.length;
        video->buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, video->fd, buf.m.offset);
        ESP_GOTO_ON_FALSE(video->buffer[i] != MAP_FAILED, ESP_ERR_NO_MEM, fail, TAG, "failed to mmap buffer");
        video->buffer_size = buf.length;
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), fail, TAG, "failed to queue frame vbuf from %s", config->dev_name);
    }

    video->isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (video->isp_fd < 0) {
        video->isp_fd = -1;
        ESP_LOGW(TAG, "ISP image controls are unavailable (could not open %s)", ESP_VIDEO_ISP1_DEVICE_NAME);
    }
    int initial_quality = default_jpeg_quality(video->img_width, video->img_height,
                          video->mode_max_fps / (video->tiling.count ? video->tiling.count : 1));
    video->jpeg_quality = initial_quality;

    video->latest_slot = -1;
    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        ESP_GOTO_ON_ERROR(set_camera_jpeg_quality(video, initial_quality), fail, TAG, "failed to set jpeg quality");
        for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
            video->slots[i].buf = heap_caps_malloc(video->buffer_size, MALLOC_CAP_SPIRAM);
            ESP_GOTO_ON_FALSE(video->slots[i].buf, ESP_ERR_NO_MEM, fail, TAG, "failed to alloc jpeg slot");
            video->slots[i].size = video->buffer_size;
        }
    } else {
        example_encoder_config_t encoder_config = {0};
        encoder_config.width = video->img_width;     /* tile scan: the canvas */
        encoder_config.height = video->img_height;
        encoder_config.pixel_format = video->pixel_format;
        encoder_config.quality = initial_quality;
        ESP_GOTO_ON_ERROR(example_encoder_init(&encoder_config, &video->encoder_handle), fail, TAG, "failed to init encoder");
        for (int i = 0; i < JPEG_SLOT_COUNT; i++) {
#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
            if (video->tiling.count) {
                /*
                 * Full-view scan: the canvases take 12 MB of PSRAM, so the JPEG slots
                 * get 1 B/px (3 MB) instead of the example's 1.5 B/px (4.5 MB, which
                 * fails to allocate next to them once PSRAM is fragmented). Measured
                 * frames: 0.30 / 0.40 / 0.67 / 1.12 MB at quality 70 / 80 / 90 / 95; a
                 * larger one fails to encode (skipped) rather than overflowing.
                 */
                jpeg_encode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
                size_t got = 0;
                video->slots[i].buf = jpeg_alloc_encoder_mem(video->img_width * video->img_height, &mcfg, &got);
                ESP_GOTO_ON_FALSE(video->slots[i].buf, ESP_ERR_NO_MEM, fail, TAG, "failed to alloc jpeg output buf");
                video->slots[i].size = got;
                continue;
            }
#endif
            ESP_GOTO_ON_ERROR(example_encoder_alloc_output_buffer(video->encoder_handle, &video->slots[i].buf,
                              &video->slots[i].size), fail, TAG, "failed to alloc jpeg output buf");
        }
        video->support_control_jpeg_quality = 1;
    }
    ESP_LOGI(TAG, "video%d: %d V4L2 buffers of %" PRIu32 " B, %d JPEG slots of %" PRIu32 " B; free PSRAM %u KB, internal %u KB",
             index, EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER, video->buffer_size, JPEG_SLOT_COUNT, video->slots[0].size,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    video->sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(video->sem, ESP_ERR_NO_MEM, fail, TAG, "failed to create semaphore");
    xSemaphoreGive(video->sem);
    return ESP_OK;

fail:
    deinit_web_cam_video(video);
    return ret;
}

static esp_err_t start_video_stream_server(web_cam_video_t *video)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    video->stop_stream_requested = false;
    ESP_RETURN_ON_ERROR(start_capture_task(video), TAG, "failed to start capture task");
    config.stack_size = 1024 * 6;
    /* The stream sender is the network-bound stage: keep it ahead of
     * ordinary tasks (control server, mDNS) so it never leaves the link idle,
     * but below the EMAC RX (15) and tcpip (18) tasks it depends on. */
    config.task_priority = STREAM_TASK_PRIO;
    config.server_port += video->index + 1;
    config.ctrl_port += video->index + 1;

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = image_stream_handler,
        .user_ctx = video,
    };

    esp_err_t ret = httpd_start(&video->stream_httpd, &config);
    if (ret != ESP_OK) {
        video->stream_httpd = NULL;
        stop_capture_task(video);
        return ret;
    }
    ret = httpd_register_uri_handler(video->stream_httpd, &stream_uri);
    if (ret != ESP_OK) {
        httpd_stop(video->stream_httpd);
        video->stream_httpd = NULL;
        stop_capture_task(video);
    }
    return ret;
}

/* Stops the stream server and then the capture task, after which nothing
 * touches the V4L2 device until start_video_stream_server(). */
static esp_err_t stop_video_stream_server(web_cam_video_t *video)
{
    esp_err_t ret = ESP_OK;
    if (video->stream_httpd) {
        ret = httpd_stop(video->stream_httpd);
        if (ret == ESP_OK) {
            video->stream_httpd = NULL;
        }
    }
    if (ret == ESP_OK) {
        stop_capture_task(video);
    }
    return ret;
}

static esp_err_t reconfigure_video_format(web_cam_video_t *video, int format_index)
{
    struct v4l2_sensor_format_enum requested = { .index = (uint32_t)format_index };
    esp_cam_sensor_format_t current = {0};
    uint32_t old_width = video->width;
    uint32_t old_height = video->height;
    int old_format_index = -1;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(enum_sensor_format(video, &requested), TAG, "invalid image format index");
    if (ioctl(video->fd, VIDIOC_G_SENSOR_FMT, &current) == 0) {
        old_format_index = find_sensor_format_index(video, &current);
    }
    ESP_RETURN_ON_FALSE(old_format_index >= 0, ESP_ERR_NOT_SUPPORTED, TAG, "current sensor mode cannot be restored");

    /* Let any active MJPEG request leave its frame loop before stopping the
     * HTTP server. This avoids tearing down the V4L2 buffers underneath a
     * stream handler while it is blocked waiting for or sending a frame. */
    video->stop_stream_requested = true;
    ret = stop_video_stream_server(video);
    if (ret != ESP_OK) {
        video->stop_stream_requested = false;
        return ret;
    }
    ret = ioctl(video->fd, VIDIOC_STREAMOFF, &type);
    if (ret != ESP_OK) {
        video->stop_stream_requested = false;
        if (start_video_stream_server(video) != ESP_OK) {
            ESP_LOGE(TAG, "video%d: failed to restart image stream server", video->index);
        }
        return ret;
    }
    ESP_GOTO_ON_ERROR(deinit_web_cam_video(video), restore_server, TAG, "failed to release camera buffers");

    web_cam_video_config_t config = { .dev_name = video->dev_name };
    ret = init_web_cam_video(video, &config, video->index, format_index,
                             requested.format.width, requested.format.height);
    if (ret == ESP_OK) {
        ret = ioctl(video->fd, VIDIOC_STREAMON, &type);
    }
    if (ret == ESP_OK) {
        ret = start_video_stream_server(video);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "video%d: changed capture format to %ux%u @%u fps (%s)", video->index,
                 video->width, video->height, video->frame_rate, sensor_format_name(requested.format.format));
        return ESP_OK;
    }

    ESP_LOGE(TAG, "video%d: new sensor format failed (%s); restoring previous format", video->index, esp_err_to_name(ret));
    if (video->fd >= 0) {
        ioctl(video->fd, VIDIOC_STREAMOFF, &type);
        stop_video_stream_server(video);
        deinit_web_cam_video(video);
    }

    ret = init_web_cam_video(video, &config, video->index, old_format_index, old_width, old_height);
    if (ret == ESP_OK) {
        ret = ioctl(video->fd, VIDIOC_STREAMON, &type);
    }
    if (ret == ESP_OK) {
        ret = start_video_stream_server(video);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video%d: failed to restore camera format; previous stream is unavailable", video->index);
        return ret;
    }
    return ESP_FAIL;

restore_server:
    if (video->fd >= 0) {
        ioctl(video->fd, VIDIOC_STREAMON, &type);
    }
    if (start_video_stream_server(video) != ESP_OK) {
        ESP_LOGE(TAG, "video%d: failed to restart image stream server", video->index);
    }
    return ret;
}

static esp_err_t new_web_cam(const web_cam_video_config_t *config, int config_count, web_cam_t **ret_wc)
{
    int i;
    web_cam_t *wc;
    esp_err_t ret = ESP_FAIL;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    wc = calloc(1, sizeof(web_cam_t) + config_count * sizeof(web_cam_video_t));
    ESP_RETURN_ON_FALSE(wc, ESP_ERR_NO_MEM, TAG, "failed to alloc web cam");
    wc->video_count = config_count;

    for (i = 0; i < config_count; i++) {
        wc->video[i].index = i;
        wc->video[i].fd = -1;

        ret = init_web_cam_video(&wc->video[i], &config[i], i, -1, 0, 0);
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "failed to find web_cam %d", i);
            continue;
        } else if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to initialize web_cam %d", i);
            goto fail0;
        }

        ESP_LOGI(TAG, "video%d: width=%" PRIu32 " height=%" PRIu32 " format=" V4L2_FMT_STR, i, wc->video[i].width,
                 wc->video[i].height, V4L2_FMT_STR_ARG(wc->video[i].pixel_format));
    }

    for (i = 0; i < config_count; i++) {
        if (is_valid_web_cam(&wc->video[i])) {
            ESP_GOTO_ON_ERROR(ioctl(wc->video[i].fd, VIDIOC_STREAMON, &type), fail1, TAG, "failed to start stream");
        }
    }

    *ret_wc = wc;

    return ESP_OK;

fail1:
    for (int j = i - 1; j >= 0; j--) {
        if (is_valid_web_cam(&wc->video[j])) {
            ioctl(wc->video[j].fd, VIDIOC_STREAMOFF, &type);
        }
    }
    i = config_count; // deinit all web_cam
fail0:
    for (int j = i - 1; j >= 0; j--) {
        if (is_valid_web_cam(&wc->video[j])) {
            deinit_web_cam_video(&wc->video[j]);
        }
    }
    free(wc);
    return ret;
}

static void free_web_cam(web_cam_t *web_cam)
{
    for (int i = 0; i < web_cam->video_count; i++) {
        if (is_valid_web_cam(&web_cam->video[i])) {
            deinit_web_cam_video(&web_cam->video[i]);
        }
    }
    free(web_cam);
}

#if CONFIG_EXAMPLE_ENABLE_ISP_DEBUG_API
/* ------------------------------------------------------------------ */
/* ISP debug API glue (isp_debug_api.c)                               */
/* ------------------------------------------------------------------ */

static web_cam_video_t *debug_csi_video(web_cam_t *wc)
{
    for (int i = 0; i < wc->video_count; i++) {
        web_cam_video_t *v = &wc->video[i];
        if (v->dev_name && strcmp(v->dev_name, ESP_VIDEO_MIPI_CSI_DEVICE_NAME) == 0) {
            return v;
        }
    }
    return NULL;
}

static int debug_isp_fd(void *ctx)
{
    web_cam_video_t *v = debug_csi_video((web_cam_t *)ctx);
    return v ? v->isp_fd : -1;
}

static esp_cam_sensor_device_t *debug_sensor(void *ctx)
{
    web_cam_video_t *v = debug_csi_video((web_cam_t *)ctx);
    return v ? camera_sensor(v) : NULL;
}

/*
 * One frame straight from the sensor: the CSI receiver is reopened with a
 * RAW10 capture format (SBGGR10 is esp_video's name for "10-bit Bayer, any
 * order"), which makes esp_video put the ISP in bypass, so no ISP block (WB,
 * CCM, gamma, ...) touches the data. The sensor mode, exposure and gain stay
 * what they were (the IPA gets no statistics while the ISP is bypassed, so AE
 * holds). The MJPEG stream is stopped for the duration and restarted after.
 */
static esp_err_t debug_raw_capture(void *ctx, int skip_frames, uint8_t **data, size_t *len,
                                   uint32_t *width, uint32_t *height, uint32_t *pixfmt)
{
    web_cam_video_t *video = debug_csi_video((web_cam_t *)ctx);
    ESP_RETURN_ON_FALSE(video && is_valid_web_cam(video), ESP_ERR_INVALID_STATE, TAG, "no MIPI-CSI camera");

    esp_cam_sensor_format_t current = {0};
    ESP_RETURN_ON_FALSE(ioctl(video->fd, VIDIOC_G_SENSOR_FMT, &current) == 0, ESP_FAIL, TAG, "no sensor format");
    int format_index = find_sensor_format_index(video, &current);
    ESP_RETURN_ON_FALSE(format_index >= 0, ESP_ERR_NOT_SUPPORTED, TAG, "current sensor mode unknown");
    uint32_t w = video->width, h = video->height;
    uint8_t quality = video->jpeg_quality;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t ret = ESP_OK;
    uint8_t *bufs[2] = {0};
    uint32_t lens[2] = {0};
    uint8_t *out = NULL;
    size_t out_len = 0;

    /* Hold AE so the RAW frame has the exposure the stream had. */
    esp_video_isp_pipeline_agc_status_t agc = ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE;
    esp_video_isp_pipeline_get_agc_status(&agc);
    esp_video_isp_pipeline_set_agc_status(ESP_VIDEO_ISP_PIPELINE_AGC_DISABLE);

    video->stop_stream_requested = true;
    stop_video_stream_server(video);
    ioctl(video->fd, VIDIOC_STREAMOFF, &type);
    deinit_web_cam_video(video);

    video->fd = open(video->dev_name, O_RDWR);
    ESP_GOTO_ON_FALSE(video->fd >= 0, ESP_FAIL, restore, TAG, "reopen failed");
    {
        struct v4l2_sensor_format_enum se = { .index = (uint32_t)format_index };
        ESP_GOTO_ON_FALSE(enum_sensor_format(video, &se) == 0, ESP_FAIL, raw_close, TAG, "enum");
        ESP_GOTO_ON_ERROR(apply_lens_centred_sensor_format(video, &se.format), raw_close, TAG, "sensor format");
        struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
        ESP_GOTO_ON_FALSE(ioctl(video->fd, VIDIOC_G_FMT, &f) == 0, ESP_FAIL, raw_close, TAG, "G_FMT");
        f.fmt.pix.width = w;
        f.fmt.pix.height = h;
        f.fmt.pix.pixelformat = *pixfmt ? *pixfmt : V4L2_PIX_FMT_SBGGR10;
        ESP_GOTO_ON_FALSE(ioctl(video->fd, VIDIOC_S_FMT, &f) == 0, ESP_ERR_NOT_SUPPORTED, raw_close, TAG,
                          "RAW capture format rejected");
        *pixfmt = f.fmt.pix.pixelformat;
        *width = f.fmt.pix.width;
        *height = f.fmt.pix.height;

        struct v4l2_requestbuffers req = { .count = 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        ESP_GOTO_ON_FALSE(ioctl(video->fd, VIDIOC_REQBUFS, &req) == 0, ESP_ERR_NO_MEM, raw_close, TAG, "REQBUFS");
        for (int i = 0; i < 2; i++) {
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
            ESP_GOTO_ON_FALSE(ioctl(video->fd, VIDIOC_QUERYBUF, &b) == 0, ESP_FAIL, raw_unmap, TAG, "QUERYBUF");
            bufs[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, video->fd, b.m.offset);
            ESP_GOTO_ON_FALSE(bufs[i] != MAP_FAILED, ESP_ERR_NO_MEM, raw_unmap, TAG, "mmap");
            lens[i] = b.length;
            ESP_GOTO_ON_FALSE(ioctl(video->fd, VIDIOC_QBUF, &b) == 0, ESP_FAIL, raw_unmap, TAG, "QBUF");
        }
        /* Never block the control server forever if the path delivers no frames. */
        struct timeval tmo = { .tv_sec = 2, .tv_usec = 0 };
        ioctl(video->fd, VIDIOC_S_DQBUF_TIMEOUT, &tmo);
        ESP_GOTO_ON_FALSE(ioctl(video->fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, raw_unmap, TAG, "STREAMON");
        for (int n = 0; n <= skip_frames; n++) {
            struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
            if (ioctl(video->fd, VIDIOC_DQBUF, &b) != 0) {
                ESP_LOGW(TAG, "RAW capture: no frame from the " V4L2_FMT_STR " path", V4L2_FMT_STR_ARG(*pixfmt));
                ret = ESP_ERR_TIMEOUT;
                break;
            }
            if (n == skip_frames) {
                out_len = b.bytesused ? b.bytesused : lens[b.index];
                out = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM);
                if (out) {
                    memcpy(out, bufs[b.index], out_len);
                } else {
                    ret = ESP_ERR_NO_MEM;
                }
            }
            ioctl(video->fd, VIDIOC_QBUF, &b);
        }
        ioctl(video->fd, VIDIOC_STREAMOFF, &type);
    }
raw_unmap:
    for (int i = 0; i < 2; i++) {
        if (bufs[i] && bufs[i] != MAP_FAILED) {
            munmap(bufs[i], lens[i]);
        }
    }
    {
        struct v4l2_requestbuffers req = { .count = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        ioctl(video->fd, VIDIOC_REQBUFS, &req);
    }
raw_close:
    close(video->fd);
    video->fd = -1;
restore: {
        web_cam_video_config_t config = { .dev_name = video->dev_name };
        esp_err_t r = init_web_cam_video(video, &config, video->index, format_index, w, h);
        if (r == ESP_OK) {
            r = ioctl(video->fd, VIDIOC_STREAMON, &type);
        }
        if (r == ESP_OK) {
            r = start_video_stream_server(video);
        }
        if (r == ESP_OK) {
            restore_camera_user_settings(video, quality);
        } else {
            ESP_LOGE(TAG, "video%d: failed to restore the stream after RAW capture", video->index);
        }
    }
    esp_video_isp_pipeline_set_agc_status(agc);
    if (ret == ESP_OK && !out) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        heap_caps_free(out);
        return ret;
    }
    ESP_LOGI(TAG, "RAW capture: %" PRIu32 "x%" PRIu32 " " V4L2_FMT_STR ", %u bytes", *width, *height,
             V4L2_FMT_STR_ARG(*pixfmt), (unsigned)out_len);
    *data = out;
    *len = out_len;
    return ESP_OK;
}

/*
 * POST /api/debug/imx708_mode: define the experimental IMX708 mode (appended
 * after the built-in extra modes) for hardware tests of binning, subsampling
 * and the on-sensor scaler. Body:
 *   {"out":[w,h], "crop":[w,h], "binning":34, "x_odd_inc":1, "y_odd_inc":1,
 *    "scale_mode":0, "scale_m":16, "method":"scaled", "regs":[[addr,val],...]}
 * Responds {"index": n}; select it with /api/set_camera_config.
 */
static esp_err_t debug_imx708_mode_handler(httpd_req_t *req)
{
    char buf[512];
    int len = req->content_len < sizeof(buf) - 1 ? req->content_len : sizeof(buf) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    imx708_mode_spec_t spec = { .binning = 0x22, .x_odd_inc = 1, .y_odd_inc = 1, .scale_m = 16 };
    const char *error = NULL;
    if (!root) {
        error = "invalid JSON";
        goto done;
    }
    cJSON *j;
    if ((j = cJSON_GetObjectItem(root, "tile_latency")) && cJSON_IsNumber(j) && j->valueint >= 1 && j->valueint <= 3) {
        /* Only change the tile-scan latency (frames from a crop write to the frame showing it). */
        s_tile_latency = j->valueint;
        snprintf(buf, sizeof(buf), "{\"tile_latency\":%d}", s_tile_latency);
        goto done;
    }
    cJSON *out = cJSON_GetObjectItem(root, "out"), *crop = cJSON_GetObjectItem(root, "crop");
    if (!cJSON_IsArray(out) || cJSON_GetArraySize(out) != 2) {
        error = "out must be [w,h]";
        goto done;
    }
    spec.out_w = cJSON_GetArrayItem(out, 0)->valueint;
    spec.out_h = cJSON_GetArrayItem(out, 1)->valueint;
    spec.crop_w = spec.out_w;
    spec.crop_h = spec.out_h;
    if (cJSON_IsArray(crop) && cJSON_GetArraySize(crop) == 2) {
        spec.crop_w = cJSON_GetArrayItem(crop, 0)->valueint;
        spec.crop_h = cJSON_GetArrayItem(crop, 1)->valueint;
    }
    if ((j = cJSON_GetObjectItem(root, "tiles")) && cJSON_IsNumber(j)) spec.tiles = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "binning")) && cJSON_IsNumber(j)) spec.binning = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "x_odd_inc")) && cJSON_IsNumber(j)) spec.x_odd_inc = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "y_odd_inc")) && cJSON_IsNumber(j)) spec.y_odd_inc = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "scale_mode")) && cJSON_IsNumber(j)) spec.scale_mode = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "scale_m")) && cJSON_IsNumber(j)) spec.scale_m = j->valueint;
    if ((j = cJSON_GetObjectItem(root, "method")) && cJSON_IsString(j)) {
        strlcpy(spec.method, j->valuestring, sizeof(spec.method));
    } else {
        strlcpy(spec.method, "test", sizeof(spec.method));
    }
    cJSON *regs = cJSON_GetObjectItem(root, "regs"), *r;
    if (cJSON_IsArray(regs)) {
        cJSON_ArrayForEach(r, regs) {
            if (spec.n_extra >= IMX708_MODE_EXTRA_REGS || !cJSON_IsArray(r) || cJSON_GetArraySize(r) != 2) {
                error = "regs: up to 8 [addr,val] pairs";
                goto done;
            }
            spec.extra_reg[spec.n_extra] = cJSON_GetArrayItem(r, 0)->valueint;
            spec.extra_val[spec.n_extra] = cJSON_GetArrayItem(r, 1)->valueint;
            spec.n_extra++;
        }
    }
    size_t idx = 0;
    if (imx708_modes_set_experimental(&spec, &idx) != ESP_OK) {
        error = "mode rejected (see log)";
        goto done;
    }
    snprintf(buf, sizeof(buf), "{\"index\":%u}", (unsigned)(idx
#if CONFIG_CAMERA_IMX708
                                                           + imx708_format_count()
#endif
                                                          ));
done:
    cJSON_Delete(root);
    if (error) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}
#endif /* CONFIG_EXAMPLE_ENABLE_ISP_DEBUG_API */

static esp_err_t http_server_init(web_cam_t *web_cam)
{
    httpd_handle_t control_httpd = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 9 + ISP_DEBUG_API_URI_COUNT;

    /* Unified static file handler for all static resources */
    httpd_uri_t static_file_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
        .user_ctx = (void *)web_cam
    };

    /* API handlers */
    httpd_uri_t capture_image_uri = {
        .uri = "/api/capture_image",
        .method = HTTP_GET,
        .handler = capture_image_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t capture_binary_uri = {
        .uri = "/api/capture_binary",
        .method = HTTP_GET,
        .handler = capture_binary_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t camera_info_uri = {
        .uri = "/api/get_camera_info",
        .method = HTTP_GET,
        .handler = camera_info_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t camera_settings_uri = {
        .uri = "/api/set_camera_config",
        .method = HTTP_POST,
        .handler = camera_settings_handler,
        .user_ctx = (void *)web_cam
    };

    httpd_uri_t lens_calibration_uri = {
        .uri = "/api/set_lens_calibration",
        .method = HTTP_POST,
        .handler = lens_calibration_handler,
        .user_ctx = (void *)web_cam
    };

    config.stack_size = 1024 * 6;
    ESP_LOGI(TAG, "Starting stream server on port: '%d'", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_start(&control_httpd, &config), TAG, "failed to start control server");
    /* Register API handlers (more specific URIs) */
    httpd_register_uri_handler(control_httpd, &capture_image_uri);
    httpd_register_uri_handler(control_httpd, &capture_binary_uri);
    httpd_register_uri_handler(control_httpd, &camera_info_uri);
    httpd_register_uri_handler(control_httpd, &camera_settings_uri);
    httpd_register_uri_handler(control_httpd, &lens_calibration_uri);
#if CONFIG_EXAMPLE_ENABLE_ISP_DEBUG_API
    isp_debug_api_ops_t debug_ops = {
        .ctx = web_cam,
        .isp_fd = debug_isp_fd,
        .sensor = debug_sensor,
        .raw_capture = debug_raw_capture,
    };
    if (isp_debug_api_register(control_httpd, &debug_ops) != ESP_OK) {
        ESP_LOGW(TAG, "ISP debug API not available");
    }
    httpd_uri_t imx708_mode_uri = {
        .uri = "/api/debug/imx708_mode",
        .method = HTTP_POST,
        .handler = debug_imx708_mode_handler,
        .user_ctx = (void *)web_cam
    };
    httpd_register_uri_handler(control_httpd, &imx708_mode_uri);
#endif

    /* Register wildcard static file handler to catch all other requests */
    httpd_register_uri_handler(control_httpd, &static_file_uri);

    for (int i = 0; i < web_cam->video_count; i++) {
        if (!is_valid_web_cam(&web_cam->video[i])) {
            continue;
        }
        esp_err_t ret = start_video_stream_server(&web_cam->video[i]);
        if (ret != ESP_OK) {
            for (int j = 0; j < i; j++) {
                stop_video_stream_server(&web_cam->video[j]);
            }
            httpd_stop(control_httpd);
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t start_cam_web_server(const web_cam_video_config_t *config, int config_count)
{
    esp_err_t ret;
    web_cam_t *web_cam;

    ESP_RETURN_ON_ERROR(new_web_cam(config, config_count, &web_cam), TAG, "Failed to new web cam");
    ESP_GOTO_ON_ERROR(http_server_init(web_cam), fail0, TAG, "Failed to init http server");

    return ESP_OK;

fail0:
    free_web_cam(web_cam);
    return ret;
}

#if CONFIG_EXAMPLE_ETH_DHCP_FALLBACK
/*
 * Ethernet IPv4 fallback. example_connect() waits for a DHCP lease forever, so with no DHCP server on the cable
 * (board plugged straight into a PC that isn't sharing its connection) the web server never starts. If no lease
 * arrives within the timeout after link-up, stop the DHCP client and set a fallback address; setting it posts
 * IP_EVENT_ETH_GOT_IP, which also lets example_connect() return. DHCP is retried on every link-up.
 */
static esp_timer_handle_t s_dhcp_fallback_timer;

static void dhcp_fallback_timer_cb(void *arg)
{
    esp_netif_t *netif = get_example_netif_from_desc(EXAMPLE_NETIF_DESC_ETH);
    if (netif == NULL) {
        return;
    }
    esp_netif_ip_info_t current = { 0 };
    if (esp_netif_get_ip_info(netif, &current) == ESP_OK && current.ip.addr != 0) {
        return; /* A lease arrived meanwhile. */
    }
    esp_err_t err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "DHCP fallback: could not stop the DHCP client (%s)", esp_err_to_name(err));
        return;
    }

    esp_netif_ip_info_t ip_info = { 0 };
    if (strlen(CONFIG_EXAMPLE_ETH_FALLBACK_IP) > 0) {
        ip_info.ip.addr = esp_ip4addr_aton(CONFIG_EXAMPLE_ETH_FALLBACK_IP);
        ip_info.netmask.addr = esp_ip4addr_aton(CONFIG_EXAMPLE_ETH_FALLBACK_NETMASK);
    } else {
        /* Link-local (RFC 3927) address from the MAC, in the usable 169.254.1.0 - 169.254.254.255 range. */
        uint8_t mac[6] = { 0 };
        esp_netif_get_mac(netif, mac);
        esp_netif_set_ip4_addr(&ip_info.ip, 169, 254, 1 + (mac[4] % 254), mac[5]);
        esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 0, 0);
    }
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCP fallback: could not set " IPSTR " (%s)", IP2STR(&ip_info.ip), esp_err_to_name(err));
        esp_netif_dhcpc_start(netif);
        return;
    }
    ESP_LOGW(TAG, "No DHCP lease within %d ms: using fallback IPv4 " IPSTR "/" IPSTR " - open http://" IPSTR "/",
             CONFIG_EXAMPLE_ETH_DHCP_FALLBACK_TIMEOUT_MS, IP2STR(&ip_info.ip), IP2STR(&ip_info.netmask),
             IP2STR(&ip_info.ip));
}

static void dhcp_fallback_eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                            void *event_data)
{
    if (event_id == ETHERNET_EVENT_CONNECTED) {
        /* After a fallback the DHCP client is stopped: start it again so a DHCP server found later wins. */
        esp_netif_t *netif = get_example_netif_from_desc(EXAMPLE_NETIF_DESC_ETH);
        esp_netif_dhcp_status_t status;
        if (netif != NULL && esp_netif_dhcpc_get_status(netif, &status) == ESP_OK
                && status == ESP_NETIF_DHCP_STOPPED) {
            esp_netif_dhcpc_start(netif);
        }
        esp_timer_stop(s_dhcp_fallback_timer);
        esp_timer_start_once(s_dhcp_fallback_timer, (uint64_t)CONFIG_EXAMPLE_ETH_DHCP_FALLBACK_TIMEOUT_MS * 1000);
    } else if (event_id == ETHERNET_EVENT_DISCONNECTED) {
        esp_timer_stop(s_dhcp_fallback_timer);
    }
}

static void dhcp_fallback_got_ip_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                         void *event_data)
{
    esp_timer_stop(s_dhcp_fallback_timer);
}

static void init_dhcp_fallback(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = dhcp_fallback_timer_cb,
        .name = "dhcp_fallback",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_dhcp_fallback_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_CONNECTED,
                                               dhcp_fallback_eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED,
                                               dhcp_fallback_eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, dhcp_fallback_got_ip_handler, NULL));
}
#endif /* CONFIG_EXAMPLE_ETH_DHCP_FALLBACK */

static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(EXAMPLE_MDNS_HOST_NAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(EXAMPLE_MDNS_INSTANCE));

    mdns_txt_item_t serviceTxtData[] = {
        {"board", CONFIG_IDF_TARGET},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    lens_calib_init();
    stream_stats_init();

    /*For camera devices that require the host to provide XCLK, the video_init() must be called immediately after the device is restarted,
    otherwise the camera device may not be able to start due to the lack of the main clock.*/
    isp_hook_prepare_ipa_configs();
    ESP_ERROR_CHECK(example_video_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(EXAMPLE_MDNS_HOST_NAME);

#if CONFIG_EXAMPLE_ETH_DHCP_FALLBACK
    /* Registered before example_connect(), which blocks until the Ethernet interface has an IPv4 address. */
    init_dhcp_fallback();
#endif

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    web_cam_video_config_t config[] = {
#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
        },
#endif /* EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR */
#if EXAMPLE_ENABLE_DVP_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_DVP_DEVICE_NAME,
        },
#endif /* EXAMPLE_ENABLE_DVP_CAM_SENSOR */
#if EXAMPLE_ENABLE_SPI_CAM_0_SENSOR
        {
            .dev_name = ESP_VIDEO_SPI_DEVICE_NAME,
        },
#endif /* EXAMPLE_ENABLE_SPI_CAM_0_SENSOR */
#if EXAMPLE_ENABLE_SPI_CAM_1_SENSOR
        {
            .dev_name = ESP_VIDEO_SPI_DEVICE_1_NAME,
        },
#endif /* EXAMPLE_ENABLE_SPI_CAM_1_SENSOR */
#if EXAMPLE_ENABLE_USB_UVC_CAM_SENSOR
        {
            .dev_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(0),
        },
#endif /* EXAMPLE_ENABLE_USB_UVC_CAM_SENSOR */
    };

    int config_count = sizeof(config) / sizeof(config[0]);

    assert(config_count > 0);
    ESP_ERROR_CHECK(start_cam_web_server(config, config_count));

    ESP_LOGI(TAG, "Camera web server starts");
}
