/*
 * Lens-centred OV5647 readout windows and persistent lens calibration.
 * See lens_calibration.h and docs/lens-calibration.md for the coordinate
 * system and the window model.
 */
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "esp_log.h"
#include "nvs.h"
#include "lens_calibration.h"

static const char *TAG = "lens_cal";

/* Register-table entry layout used by the OV5647 driver (ov5647_types.h). */
typedef struct {
    uint16_t reg;
    uint8_t val;
} lens_ov5647_reg_t;

#define LENS_OV5647_REG_DELAY   0xeeee
#define LENS_OV5647_REG_END     0xffff

/* Symmetric margin (in output pixels per side) between the array window and
 * the output region, skipped by the ISP offset registers 0x3810-0x3813.
 * Must be even so the Bayer phase of the first output pixel is unchanged. */
#define LENS_ISP_MARGIN_X       4
#define LENS_ISP_MARGIN_Y       2

/* Unified orientation = the 1280x960 mode: sensor mirror on (0x3821 bit1),
 * no flip (0x3820 bit1), ISP mirror/flip bits (bit2) off. */
#define LENS_REG_3820_ORIENT_MASK   0x06
#define LENS_REG_3820_ORIENT_VAL    0x00
#define LENS_REG_3821_ORIENT_MASK   0x06
#define LENS_REG_3821_ORIENT_VAL    0x02

/* How the output image relates to the sensor address space with the
 * orientation above (verified on hardware, see docs/lens-calibration.md):
 * mirror = image x grows while sensor x decreases. */
#define LENS_IMAGE_MIRROR       true
#define LENS_IMAGE_FLIP         false

#define LENS_MAX_REGS           320
#define LENS_SLOT_COUNT         2

#define LENS_NVS_NAMESPACE      "lens_cal"
#define LENS_NVS_KEY            "calib"
#define LENS_NVS_VERSION        0x4C430001u   /* 'LC' v1 */

typedef struct {
    uint32_t version;
    double cx, cy;
    uint32_t has_distortion;
    double fx, fy, k1, k2, p1, p2, k3;
} lens_nvs_record_t;

static lens_calibration_t s_cal;
static lens_ov5647_reg_t s_slot_regs[LENS_SLOT_COUNT][LENS_MAX_REGS];
static esp_cam_sensor_format_t s_slot_fmt[LENS_SLOT_COUNT];

void lens_calib_get_default(lens_calibration_t *cal)
{
    memset(cal, 0, sizeof(*cal));
    cal->cx = LENS_SENSOR_ACTIVE_X + LENS_SENSOR_ACTIVE_WIDTH / 2.0;
    cal->cy = LENS_SENSOR_ACTIVE_Y + LENS_SENSOR_ACTIVE_HEIGHT / 2.0;
    cal->calibrated = false;
    cal->has_distortion = false;
}

esp_err_t lens_calib_init(void)
{
    lens_calib_get_default(&s_cal);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LENS_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored lens calibration; using default centre (%.1f, %.1f)", s_cal.cx, s_cal.cy);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s); using defaults", esp_err_to_name(ret));
        return ret;
    }

    lens_nvs_record_t rec = {0};
    size_t size = sizeof(rec);
    ret = nvs_get_blob(nvs, LENS_NVS_KEY, &rec, &size);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no stored lens calibration; using default centre (%.1f, %.1f)", s_cal.cx, s_cal.cy);
        return ESP_OK;
    }
    if (ret != ESP_OK || size != sizeof(rec) || rec.version != LENS_NVS_VERSION ||
            !isfinite(rec.cx) || !isfinite(rec.cy)) {
        ESP_LOGW(TAG, "stored lens calibration is invalid (%s); using defaults", esp_err_to_name(ret));
        return ESP_OK;
    }

    s_cal.cx = rec.cx;
    s_cal.cy = rec.cy;
    s_cal.calibrated = true;
    s_cal.has_distortion = rec.has_distortion != 0;
    if (s_cal.has_distortion) {
        s_cal.distortion = (lens_distortion_t) {
            rec.fx, rec.fy, rec.k1, rec.k2, rec.p1, rec.p2, rec.k3
        };
    }
    ESP_LOGI(TAG, "loaded lens calibration: centre (%.2f, %.2f), distortion %s", s_cal.cx, s_cal.cy,
             s_cal.has_distortion ? "set" : "none");
    return ESP_OK;
}

const lens_calibration_t *lens_calib_get(void)
{
    return &s_cal;
}

void lens_calib_set(const lens_calibration_t *cal)
{
    s_cal = *cal;
}

esp_err_t lens_calib_save(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LENS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed (%s)", esp_err_to_name(ret));
        return ret;
    }

    if (!s_cal.calibrated) {
        ret = nvs_erase_key(nvs, LENS_NVS_KEY);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    } else {
        lens_nvs_record_t rec = {
            .version = LENS_NVS_VERSION,
            .cx = s_cal.cx,
            .cy = s_cal.cy,
            .has_distortion = s_cal.has_distortion ? 1 : 0,
        };
        if (s_cal.has_distortion) {
            rec.fx = s_cal.distortion.fx;
            rec.fy = s_cal.distortion.fy;
            rec.k1 = s_cal.distortion.k1;
            rec.k2 = s_cal.distortion.k2;
            rec.p1 = s_cal.distortion.p1;
            rec.p2 = s_cal.distortion.p2;
            rec.k3 = s_cal.distortion.k3;
        }
        ret = nvs_set_blob(nvs, LENS_NVS_KEY, &rec, sizeof(rec));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to save lens calibration (%s)", esp_err_to_name(ret));
    }
    return ret;
}

bool lens_sensor_supported(const char *sensor_name)
{
    return sensor_name && strcmp(sensor_name, "OV5647") == 0;
}

/* Look up the value a register table writes to @p reg (last write wins). */
static bool table_get(const lens_ov5647_reg_t *regs, int max, uint16_t reg, uint8_t *val)
{
    bool found = false;
    for (int i = 0; i < max && regs[i].reg != LENS_OV5647_REG_END; i++) {
        if (regs[i].reg == reg) {
            *val = regs[i].val;
            found = true;
        }
    }
    return found;
}

static int table_len(const esp_cam_sensor_format_t *fmt)
{
    const lens_ov5647_reg_t *regs = (const lens_ov5647_reg_t *)fmt->regs;
    int max = fmt->regs_size > 0 ? fmt->regs_size : LENS_MAX_REGS;
    int n = 0;
    while (n < max && regs[n].reg != LENS_OV5647_REG_END) {
        n++;
    }
    return n;
}

/* Subsampling increment register (0x3814 / 0x3815): odd_inc in [7:4],
 * even_inc in [3:0]; the readout advances (odd + even) pixels per 2 output
 * pixels, so the scale is (odd + even) / 2. 0x11 -> 1, 0x31 -> 2. */
static int inc_to_scale(uint8_t inc)
{
    int s = (((inc >> 4) & 0x0f) + (inc & 0x0f)) / 2;
    return s > 0 ? s : 1;
}

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Largest even value <= v (v may be negative). */
static int floor_even(int v)
{
    return (v % 2 == 0) ? v : v - 1;
}

static int ceil_even(int v)
{
    return (v % 2 == 0) ? v : v + 1;
}

bool lens_window_compute(const esp_cam_sensor_format_t *fmt, const lens_calibration_t *cal, lens_window_t *win)
{
    memset(win, 0, sizeof(*win));
    if (!fmt || !fmt->regs || !cal || fmt->width == 0 || fmt->height == 0) {
        return false;
    }

    const lens_ov5647_reg_t *regs = (const lens_ov5647_reg_t *)fmt->regs;
    int max = fmt->regs_size > 0 ? fmt->regs_size : LENS_MAX_REGS;
    uint8_t inc_x = 0x11, inc_y = 0x11;
    table_get(regs, max, 0x3814, &inc_x);
    table_get(regs, max, 0x3815, &inc_y);

    win->scale_x = inc_to_scale(inc_x);
    win->scale_y = inc_to_scale(inc_y);
    win->width = fmt->width * win->scale_x;
    win->height = fmt->height * win->scale_y;
    win->mirror = LENS_IMAGE_MIRROR;
    win->flip = LENS_IMAGE_FLIP;

    if (win->width > LENS_SENSOR_ACTIVE_WIDTH || win->height > LENS_SENSOR_ACTIVE_HEIGHT) {
        return false;
    }

    /* Start addresses must stay even: the colour of the first pixel read out
     * (and so the Bayer order reported to the ISP) depends on the parity of
     * the window start/end; widths are even, so even starts keep the phase.
     * In 2x2-binned modes an even shift of 2 sensor px = 1 output px also
     * keeps the phase (same-colour pixels are 2 apart). */
    int x_min = ceil_even(LENS_SENSOR_ACTIVE_X);
    int x_max = floor_even(LENS_SENSOR_ACTIVE_X + LENS_SENSOR_ACTIVE_WIDTH - win->width);
    int y_min = ceil_even(LENS_SENSOR_ACTIVE_Y);
    int y_max = floor_even(LENS_SENSOR_ACTIVE_Y + LENS_SENSOR_ACTIVE_HEIGHT - win->height);
    if (x_max < x_min || y_max < y_min) {
        return false;
    }

    double ideal_x = cal->cx - win->width / 2.0;
    double ideal_y = cal->cy - win->height / 2.0;
    win->x = clamp_int(2 * (int)lround(ideal_x / 2.0), x_min, x_max);
    win->y = clamp_int(2 * (int)lround(ideal_y / 2.0), y_min, y_max);

    win->err_x = (win->x + win->width / 2.0) - cal->cx;
    win->err_y = (win->y + win->height / 2.0) - cal->cy;
    win->cx_min = x_min + win->width / 2.0;
    win->cx_max = x_max + win->width / 2.0;
    win->cy_min = y_min + win->height / 2.0;
    win->cy_max = y_max + win->height / 2.0;

    /* Array window = output region + symmetric ISP margin, kept inside the array. */
    int mx = LENS_ISP_MARGIN_X;
    int my = LENS_ISP_MARGIN_Y;
    while (mx > 0 && (win->x - mx * win->scale_x < 0 ||
                      win->x + win->width + mx * win->scale_x > LENS_SENSOR_ARRAY_WIDTH)) {
        mx -= 2;
    }
    while (my > 0 && (win->y - my * win->scale_y < 0 ||
                      win->y + win->height + my * win->scale_y > LENS_SENSOR_ARRAY_HEIGHT)) {
        my -= 2;
    }
    win->isp_off_x = mx;
    win->isp_off_y = my;
    win->array_x_start = win->x - mx * win->scale_x;
    win->array_x_end = win->x + win->width + mx * win->scale_x - 1;
    win->array_y_start = win->y - my * win->scale_y;
    win->array_y_end = win->y + win->height + my * win->scale_y - 1;
    win->valid = true;
    return true;
}

static bool is_window_reg(uint16_t reg)
{
    return (reg >= 0x3800 && reg <= 0x3807) || (reg >= 0x3810 && reg <= 0x3813);
}

const esp_cam_sensor_format_t *lens_window_build_format(const esp_cam_sensor_format_t *base,
                                                        const lens_calibration_t *cal,
                                                        const void *in_use_regs, lens_window_t *win)
{
    if (!lens_window_compute(base, cal, win)) {
        return NULL;
    }

    int slot = (s_slot_regs[0] == in_use_regs) ? 1 : 0;
    lens_ov5647_reg_t *out = s_slot_regs[slot];
    const lens_ov5647_reg_t *in = (const lens_ov5647_reg_t *)base->regs;
    int n_in = table_len(base);
    int n = 0;
    bool have_3820 = false, have_3821 = false;

    /* 8 window regs + 4 offset regs + 2 orientation regs + END */
    if (n_in + 15 > LENS_MAX_REGS) {
        ESP_LOGE(TAG, "register table of %s too large (%d)", base->name ? base->name : "?", n_in);
        return NULL;
    }

    for (int i = 0; i < n_in; i++) {
        lens_ov5647_reg_t r = in[i];
        if (r.reg != LENS_OV5647_REG_DELAY && is_window_reg(r.reg)) {
            continue;
        }
        if (r.reg == 0x3820) {
            r.val = (r.val & ~LENS_REG_3820_ORIENT_MASK) | LENS_REG_3820_ORIENT_VAL;
            have_3820 = true;
        } else if (r.reg == 0x3821) {
            r.val = (r.val & ~LENS_REG_3821_ORIENT_MASK) | LENS_REG_3821_ORIENT_VAL;
            have_3821 = true;
        }
        out[n++] = r;
    }
    if (!have_3820) {
        out[n++] = (lens_ov5647_reg_t) { 0x3820, LENS_REG_3820_ORIENT_VAL };
    }
    if (!have_3821) {
        out[n++] = (lens_ov5647_reg_t) { 0x3821, LENS_REG_3821_ORIENT_VAL };
    }

    out[n++] = (lens_ov5647_reg_t) { 0x3800, (uint8_t)((win->array_x_start >> 8) & 0x0f) };
    out[n++] = (lens_ov5647_reg_t) { 0x3801, (uint8_t)(win->array_x_start & 0xff) };
    out[n++] = (lens_ov5647_reg_t) { 0x3802, (uint8_t)((win->array_y_start >> 8) & 0x07) };
    out[n++] = (lens_ov5647_reg_t) { 0x3803, (uint8_t)(win->array_y_start & 0xff) };
    out[n++] = (lens_ov5647_reg_t) { 0x3804, (uint8_t)((win->array_x_end >> 8) & 0x0f) };
    out[n++] = (lens_ov5647_reg_t) { 0x3805, (uint8_t)(win->array_x_end & 0xff) };
    out[n++] = (lens_ov5647_reg_t) { 0x3806, (uint8_t)((win->array_y_end >> 8) & 0x07) };
    out[n++] = (lens_ov5647_reg_t) { 0x3807, (uint8_t)(win->array_y_end & 0xff) };
    out[n++] = (lens_ov5647_reg_t) { 0x3810, (uint8_t)((win->isp_off_x >> 8) & 0x0f) };
    out[n++] = (lens_ov5647_reg_t) { 0x3811, (uint8_t)(win->isp_off_x & 0xff) };
    out[n++] = (lens_ov5647_reg_t) { 0x3812, (uint8_t)((win->isp_off_y >> 8) & 0x07) };
    out[n++] = (lens_ov5647_reg_t) { 0x3813, (uint8_t)(win->isp_off_y & 0xff) };
    out[n++] = (lens_ov5647_reg_t) { LENS_OV5647_REG_END, 0x00 };

    esp_cam_sensor_format_t *fmt = &s_slot_fmt[slot];
    *fmt = *base;
    fmt->regs = out;
    fmt->regs_size = n;

    ESP_LOGI(TAG, "%ux%u: output region x %d..%d y %d..%d (scale %d), array %d..%d x %d..%d, isp offset %d/%d, "
             "centre error (%.1f, %.1f)", base->width, base->height, win->x, win->x + win->width - 1,
             win->y, win->y + win->height - 1, win->scale_x, win->array_x_start, win->array_x_end,
             win->array_y_start, win->array_y_end, win->isp_off_x, win->isp_off_y, win->err_x, win->err_y);
    return fmt;
}

bool lens_window_is_built_regs(const void *regs)
{
    for (int i = 0; i < LENS_SLOT_COUNT; i++) {
        if (regs == s_slot_regs[i]) {
            return true;
        }
    }
    return false;
}

cJSON *lens_sensor_json(void)
{
    cJSON *sensor = cJSON_CreateObject();
    cJSON_AddNumberToObject(sensor, "arrayWidth", LENS_SENSOR_ARRAY_WIDTH);
    cJSON_AddNumberToObject(sensor, "arrayHeight", LENS_SENSOR_ARRAY_HEIGHT);
    cJSON *active = cJSON_CreateObject();
    cJSON_AddNumberToObject(active, "x", LENS_SENSOR_ACTIVE_X);
    cJSON_AddNumberToObject(active, "y", LENS_SENSOR_ACTIVE_Y);
    cJSON_AddNumberToObject(active, "width", LENS_SENSOR_ACTIVE_WIDTH);
    cJSON_AddNumberToObject(active, "height", LENS_SENSOR_ACTIVE_HEIGHT);
    cJSON_AddItemToObject(sensor, "activeArea", active);
    return sensor;
}

cJSON *lens_calibration_json(const lens_calibration_t *cal)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *center = cJSON_CreateObject();
    cJSON_AddNumberToObject(center, "x", cal->cx);
    cJSON_AddNumberToObject(center, "y", cal->cy);
    cJSON_AddItemToObject(obj, "center", center);
    cJSON_AddStringToObject(obj, "source", cal->calibrated ? "calibrated" : "default");
    if (cal->has_distortion) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "fx", cal->distortion.fx);
        cJSON_AddNumberToObject(d, "fy", cal->distortion.fy);
        cJSON_AddNumberToObject(d, "k1", cal->distortion.k1);
        cJSON_AddNumberToObject(d, "k2", cal->distortion.k2);
        cJSON_AddNumberToObject(d, "p1", cal->distortion.p1);
        cJSON_AddNumberToObject(d, "p2", cal->distortion.p2);
        cJSON_AddNumberToObject(d, "k3", cal->distortion.k3);
        cJSON_AddItemToObject(obj, "distortion", d);
    } else {
        cJSON_AddNullToObject(obj, "distortion");
    }
    return obj;
}

cJSON *lens_window_json(const lens_window_t *win)
{
    if (!win || !win->valid) {
        return cJSON_CreateNull();
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "x", win->x);
    cJSON_AddNumberToObject(obj, "y", win->y);
    cJSON_AddNumberToObject(obj, "width", win->width);
    cJSON_AddNumberToObject(obj, "height", win->height);
    cJSON_AddNumberToObject(obj, "scale", win->scale_x);
    cJSON_AddBoolToObject(obj, "mirror", win->mirror);
    cJSON_AddBoolToObject(obj, "flip", win->flip);
    cJSON *err = cJSON_CreateObject();
    /* round away float noise (e.g. 0.70000000000004) */
    cJSON_AddNumberToObject(err, "x", round(win->err_x * 1e6) / 1e6);
    cJSON_AddNumberToObject(err, "y", round(win->err_y * 1e6) / 1e6);
    cJSON_AddItemToObject(obj, "centerError", err);
    cJSON *range = cJSON_CreateObject();
    cJSON_AddNumberToObject(range, "xMin", win->cx_min);
    cJSON_AddNumberToObject(range, "xMax", win->cx_max);
    cJSON_AddNumberToObject(range, "yMin", win->cy_min);
    cJSON_AddNumberToObject(range, "yMax", win->cy_max);
    cJSON_AddItemToObject(obj, "centerRange", range);
    return obj;
}
