/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "protocol_examples_common.h"
#include "mdns.h"
#include "lwip/inet.h"
#include "lwip/apps/netbiosns.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "esp_cam_sensor.h"
#include "esp_video_device_common.h"
#include "esp_video_device_internal.h"
#include "example_video_common.h"

#define EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER  CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER

#define EXAMPLE_JPEG_ENC_QUALITY            CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY

#define EXAMPLE_MDNS_INSTANCE               CONFIG_EXAMPLE_MDNS_INSTANCE
#define EXAMPLE_MDNS_HOST_NAME              CONFIG_EXAMPLE_MDNS_HOST_NAME

#define EXAMPLE_PART_BOUNDARY               CONFIG_EXAMPLE_HTTP_PART_BOUNDARY

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" EXAMPLE_PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" EXAMPLE_PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\nX-Timestamp: %d.%06d\r\n\r\n";

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
    uint8_t *jpeg_out_buf;
    uint32_t jpeg_out_size;

    uint8_t *buffer[EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_length[EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER];
    uint32_t buffer_size;

    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint8_t jpeg_quality;

    uint32_t frame_rate;

    int32_t image_control_values[4];
    uint8_t image_control_values_valid;

    SemaphoreHandle_t sem;

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

static bool query_camera_image_control(web_cam_video_t *video, const camera_image_control_t *setting,
                                       int *fd, struct v4l2_query_ext_ctrl *qctrl)
{
    *fd = camera_image_control_fd(video, setting);
    if (*fd < 0) {
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
        if (ioctl(video->fd, VIDIOC_ENUM_SENSOR_FMT, &sensor_enum) != 0) {
            break;
        }

        const esp_cam_sensor_format_t *candidate = &sensor_enum.format;
        if ((wanted->regs && candidate->regs == wanted->regs) ||
                (candidate->width == wanted->width && candidate->height == wanted->height &&
                 candidate->fps == wanted->fps && candidate->format == wanted->format &&
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
        if (ioctl(video->fd, VIDIOC_ENUM_SENSOR_FMT, &sensor_enum) != 0) {
            break;
        }
        const esp_cam_sensor_format_t *format = &sensor_enum.format;
        if (format->width == video->width && format->height == video->height) {
            if (format->fps == video->frame_rate) {
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

static esp_err_t capture_video_image(httpd_req_t *req, web_cam_video_t *video, bool is_jpeg)
{
    esp_err_t ret;
    struct v4l2_buffer buf;
    const char *type_str = is_jpeg ? "JPEG" : "binary";
    uint32_t jpeg_encoded_size;

    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_DQBUF, &buf), TAG, "failed to receive video frame");
    if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    if (!is_jpeg || video->pixel_format == V4L2_PIX_FMT_JPEG) {
        /* Directly send the buffer of raw data */
        ESP_GOTO_ON_ERROR(httpd_resp_send(req, (char *)video->buffer[buf.index], buf.bytesused), fail0, TAG, "failed to send %s", type_str);
        jpeg_encoded_size = buf.bytesused;
    } else {
        ESP_GOTO_ON_FALSE(xSemaphoreTake(video->sem, portMAX_DELAY) == pdPASS, ESP_FAIL, fail0, TAG, "failed to take semaphore");
        ret = example_encoder_process(video->encoder_handle, video->buffer[buf.index], video->buffer_size,
                                      video->jpeg_out_buf, video->jpeg_out_size, &jpeg_encoded_size);
        xSemaphoreGive(video->sem);
        ESP_GOTO_ON_ERROR(ret, fail0, TAG, "failed to encode video frame");
        ESP_GOTO_ON_ERROR(httpd_resp_send(req, (char *)video->jpeg_out_buf, jpeg_encoded_size), fail0, TAG, "failed to send %s", type_str);
    }

    ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), TAG, "failed to queue video frame");

    ESP_GOTO_ON_ERROR(httpd_resp_sendstr_chunk(req, NULL), fail0, TAG, "failed to send null");

    ESP_LOGD(TAG, "send %s image%d size: %" PRIu32, type_str, video->index, jpeg_encoded_size);

    return ESP_OK;

fail0:
    ioctl(video->fd, VIDIOC_QBUF, &buf);
    return ret;
}

static char *get_cameras_json(web_cam_t *web_cam)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cameras = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "cameras", cameras);

    for (int i = 0; i < web_cam->video_count; i++) {
        char src_str[32];

        if (!is_valid_web_cam(&web_cam->video[i])) {
            continue;
        }

        bool auto_brightness_enabled = true;
        bool supports_auto_brightness = camera_auto_brightness_get(&web_cam->video[i], &auto_brightness_enabled);

        cJSON *camera = cJSON_CreateObject();
        cJSON_AddNumberToObject(camera, "index", i);
        cJSON_AddBoolToObject(camera, "supportsAutoBrightness", supports_auto_brightness);
        cJSON_AddBoolToObject(camera, "autoBrightnessEnabled", auto_brightness_enabled);
        assert(snprintf(src_str, sizeof(src_str), ":%d/stream", i + 81) > 0);
        cJSON_AddStringToObject(camera, "src", src_str);
        cJSON_AddNumberToObject(camera, "currentFrameRate", web_cam->video[i].frame_rate);
        int current_format_index = current_capture_format_index(&web_cam->video[i]);
        cJSON_AddNumberToObject(camera, "currentImageFormat", current_format_index >= 0 ? current_format_index : 0);
        assert(snprintf(src_str, sizeof(src_str), "JPEG %" PRIu32 "x%" PRIu32, web_cam->video[i].width, web_cam->video[i].height) > 0);
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
        cJSON_AddNumberToObject(current_resolution, "width", web_cam->video[i].width);
        cJSON_AddNumberToObject(current_resolution, "height", web_cam->video[i].height);
        cJSON_AddItemToObject(camera, "currentResolution", current_resolution);

        cJSON *image_formats = cJSON_CreateArray();
        for (uint32_t format_index = 0; ; format_index++) {
            struct v4l2_sensor_format_enum sensor_enum = { .index = format_index };
            if (ioctl(web_cam->video[i].fd, VIDIOC_ENUM_SENSOR_FMT, &sensor_enum) != 0) {
                break;
            }

            const esp_cam_sensor_format_t *sensor_format = &sensor_enum.format;
            cJSON *image_format = cJSON_CreateObject();
            cJSON_AddNumberToObject(image_format, "id", format_index);
            assert(snprintf(src_str, sizeof(src_str), "%ux%u %s @%u fps",
                            sensor_format->width, sensor_format->height,
                            sensor_format_name(sensor_format->format), sensor_format->fps) > 0);
            cJSON_AddStringToObject(image_format, "description", src_str);

            if (web_cam->video[i].support_control_jpeg_quality) {
                cJSON *image_format_quality = cJSON_CreateObject();
                cJSON_AddNumberToObject(image_format_quality, "min", 1);
                cJSON_AddNumberToObject(image_format_quality, "max", 100);
                cJSON_AddNumberToObject(image_format_quality, "step", 1);
                cJSON_AddNumberToObject(image_format_quality, "default", EXAMPLE_JPEG_ENC_QUALITY);
                cJSON_AddItemToObject(image_format, "quality", image_format_quality);
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
        ESP_RETURN_ON_ERROR(example_encoder_set_jpeg_quality(video->encoder_handle, quality_reset), TAG, "failed to set jpeg quality");
        video->jpeg_quality = quality_reset;
    }

    if (video->support_control_jpeg_quality) {
        ESP_LOGI(TAG, "video%d: set jpeg quality %d success", video->index, quality_reset);
    }

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

    ESP_LOGI(TAG, "JSON parse success - index:%d, image_format:%d, jpeg_quality:%d", index, image_format, jpeg_quality);
    ESP_GOTO_ON_FALSE(image_format >= 0, ESP_ERR_INVALID_ARG, fail1, TAG, "invalid image format index");
    if (image_format != current_capture_format_index(&web_cam->video[index])) {
        ESP_GOTO_ON_ERROR(reconfigure_video_format(&web_cam->video[index], image_format), fail1, TAG,
                          "failed to change camera capture size");
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
    }

    /* If no static file matches, return 404 */
    ESP_LOGW(TAG, "File not found: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t image_stream_handler(httpd_req_t *req)
{
    esp_err_t ret;
    struct v4l2_buffer buf;
    char http_string[128];
    bool locked = false;
    web_cam_video_t *video = (web_cam_video_t *)req->user_ctx;

    ESP_RETURN_ON_FALSE(snprintf(http_string, sizeof(http_string), "%" PRIu32, video->frame_rate) > 0,
                        ESP_FAIL, TAG, "failed to format framerate buffer");

    ESP_RETURN_ON_ERROR(httpd_resp_set_type(req, STREAM_CONTENT_TYPE), TAG, "failed to set content type");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*"), TAG, "failed to set access control allow origin");
    ESP_RETURN_ON_ERROR(httpd_resp_set_hdr(req, "X-Framerate", http_string), TAG, "failed to set x framerate");

    while (!video->stop_stream_requested) {
        int hlen;
        struct timespec ts;
        uint32_t jpeg_encoded_size;

        locked = false;

        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_DQBUF, &buf), TAG, "failed to receive video frame");
        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), TAG, "failed to queue video frame");
            continue;
        }

        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)), fail0, TAG, "failed to send boundary");

        if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
            video->jpeg_out_buf = video->buffer[buf.index];
            jpeg_encoded_size = buf.bytesused;
        } else {
            ESP_GOTO_ON_FALSE(xSemaphoreTake(video->sem, portMAX_DELAY) == pdPASS, ESP_FAIL, fail0, TAG, "failed to take semaphore");
            locked = true;

            ESP_GOTO_ON_ERROR(example_encoder_process(video->encoder_handle, video->buffer[buf.index], video->buffer_size,
                              video->jpeg_out_buf, video->jpeg_out_size, &jpeg_encoded_size),
                              fail0, TAG, "failed to encode video frame");
        }

        ESP_GOTO_ON_ERROR(clock_gettime(CLOCK_MONOTONIC, &ts), fail0, TAG, "failed to get time");
        ESP_GOTO_ON_FALSE((hlen = snprintf(http_string, sizeof(http_string), STREAM_PART, jpeg_encoded_size, ts.tv_sec, ts.tv_nsec)) > 0,
                          ESP_FAIL, fail0, TAG, "failed to format part buffer");
        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, http_string, hlen), fail0, TAG, "failed to send boundary");

        ESP_GOTO_ON_ERROR(httpd_resp_send_chunk(req, (char *)video->jpeg_out_buf, jpeg_encoded_size), fail0, TAG, "failed to send jpeg");
        if (locked) {
            xSemaphoreGive(video->sem);
            locked = false;
        }

        ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_QBUF, &buf), TAG, "failed to queue video frame");
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;

fail0:
    if (locked) {
        xSemaphoreGive(video->sem);
    }
    ioctl(video->fd, VIDIOC_QBUF, &buf);
    return ret;
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

    if (video->encoder_handle) {
        if (video->jpeg_out_buf) {
            example_encoder_free_output_buffer(video->encoder_handle, video->jpeg_out_buf);
            video->jpeg_out_buf = NULL;
        }
        example_encoder_deinit(video->encoder_handle);
        video->encoder_handle = NULL;
        video->jpeg_out_size = 0;
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
    video->buffer_size = 0;
    video->width = 0;
    video->height = 0;
    video->pixel_format = 0;
    video->frame_rate = 0;
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

    if (sensor_format_index >= 0) {
        struct v4l2_sensor_format_enum sensor_enum = { .index = (uint32_t)sensor_format_index };
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_ENUM_SENSOR_FMT, &sensor_enum), fail, TAG,
                          "unsupported sensor format index %d", sensor_format_index);
        ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_S_SENSOR_FMT, &sensor_enum.format), fail, TAG,
                          "failed to switch sensor format to %ux%u", sensor_enum.format.width, sensor_enum.format.height);

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

    sparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_GOTO_ON_ERROR(ioctl(video->fd, VIDIOC_G_PARM, &sparm), fail, TAG, "failed to get frame rate from %s", config->dev_name);
    if (timeperframe->numerator) {
        video->frame_rate = timeperframe->denominator / timeperframe->numerator;
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
    video->jpeg_quality = EXAMPLE_JPEG_ENC_QUALITY;

    if (video->pixel_format == V4L2_PIX_FMT_JPEG) {
        ESP_GOTO_ON_ERROR(set_camera_jpeg_quality(video, EXAMPLE_JPEG_ENC_QUALITY), fail, TAG, "failed to set jpeg quality");
    } else {
        example_encoder_config_t encoder_config = {0};
        encoder_config.width = video->width;
        encoder_config.height = video->height;
        encoder_config.pixel_format = video->pixel_format;
        encoder_config.quality = EXAMPLE_JPEG_ENC_QUALITY;
        ESP_GOTO_ON_ERROR(example_encoder_init(&encoder_config, &video->encoder_handle), fail, TAG, "failed to init encoder");
        ESP_GOTO_ON_ERROR(example_encoder_alloc_output_buffer(video->encoder_handle, &video->jpeg_out_buf, &video->jpeg_out_size),
                          fail, TAG, "failed to alloc jpeg output buf");
        video->support_control_jpeg_quality = 1;
    }

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
    config.stack_size = 1024 * 6;
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
        return ret;
    }
    ret = httpd_register_uri_handler(video->stream_httpd, &stream_uri);
    if (ret != ESP_OK) {
        httpd_stop(video->stream_httpd);
        video->stream_httpd = NULL;
    }
    return ret;
}

static esp_err_t stop_video_stream_server(web_cam_video_t *video)
{
    if (!video->stream_httpd) {
        return ESP_OK;
    }
    esp_err_t ret = httpd_stop(video->stream_httpd);
    if (ret == ESP_OK) {
        video->stream_httpd = NULL;
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

    ESP_RETURN_ON_ERROR(ioctl(video->fd, VIDIOC_ENUM_SENSOR_FMT, &requested), TAG, "invalid image format index");
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

static esp_err_t http_server_init(web_cam_t *web_cam)
{
    httpd_handle_t control_httpd = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

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

    config.stack_size = 1024 * 6;
    ESP_LOGI(TAG, "Starting stream server on port: '%d'", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_start(&control_httpd, &config), TAG, "failed to start control server");
    /* Register API handlers (more specific URIs) */
    httpd_register_uri_handler(control_httpd, &capture_image_uri);
    httpd_register_uri_handler(control_httpd, &capture_binary_uri);
    httpd_register_uri_handler(control_httpd, &camera_info_uri);
    httpd_register_uri_handler(control_httpd, &camera_settings_uri);

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

    /*For camera devices that require the host to provide XCLK, the video_init() must be called immediately after the device is restarted,
    otherwise the camera device may not be able to start due to the lack of the main clock.*/
    ESP_ERROR_CHECK(example_video_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(EXAMPLE_MDNS_HOST_NAME);

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
