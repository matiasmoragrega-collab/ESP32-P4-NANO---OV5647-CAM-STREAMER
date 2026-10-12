/*
 * Lens-centred sensor readout windows and persistent, per-sensor lens
 * calibration. See lens_calibration.h and docs/lens-calibration.md for the
 * coordinate systems and the window model.
 */
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "lens_calibration.h"

static const char *TAG = "lens_cal";

/* Register-table entry layout used by the OV5647 driver (ov5647_types.h) and
 * by the IMX708 driver (imx708_settings.h): 16-bit address, 8-bit value. */
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

/* ---- IMX708 (mushbraindave/esp_cam_sensor_imx 0.4.x mode tables) ---------- */
typedef lens_ov5647_reg_t lens_imx708_reg_t;   /* same {u16 reg, u8 val} layout */

#define LENS_IMX708_REG_DELAY        0xfffe
#define LENS_IMX708_REG_END          0xffff
#define LENS_IMX708_REG_X_ADDR_START 0x0344   /* 16-bit analog crop, full-res px */
#define LENS_IMX708_REG_Y_ADDR_START 0x0346
#define LENS_IMX708_REG_X_ADDR_END   0x0348
#define LENS_IMX708_REG_Y_ADDR_END   0x034a
#define LENS_IMX708_REG_BINNING_MODE 0x0900   /* 1 = binning on */
#define LENS_IMX708_REG_BINNING_TYPE 0x0901   /* [7:4] horizontal, [3:0] vertical factor */
#define LENS_IMX708_REG_DIG_CROP_X   0x0408   /* 16-bit digital crop, binned px */
#define LENS_IMX708_REG_DIG_CROP_Y   0x040a
#define LENS_IMX708_REG_DIG_CROP_W   0x040c
#define LENS_IMX708_REG_DIG_CROP_H   0x040e
#define LENS_IMX708_REG_X_ODD_INC    0x0383   /* subsampling: 1 = none, 3 = 1/2, 7 = 1/4 */
#define LENS_IMX708_REG_Y_ODD_INC    0x0387
#define LENS_IMX708_REG_SCALE_MODE   0x0401   /* CCS scaler (hardwired off on the IMX708, see docs) */
#define LENS_IMX708_REG_SCALE_M      0x0405

/* The digital-crop offsets move in steps of this many binned pixels. The binned
 * field is a plain 2x2 Bayer mosaic, so an even offset keeps the RGGB phase the
 * driver reports (verified on hardware with an offset of 2 mod 4). */
#define LENS_IMX708_CROP_STEP        2

/* Sensor orientation programmed for the IMX708 (driver HMIRROR / VFLIP, i.e.
 * register 0x0101; Kconfig EXAMPLE_IMX708_HMIRROR / EXAMPLE_IMX708_VFLIP).
 * Default: both on, the Raspberry Pi convention for IMX708 modules (the
 * sensor sits rotated 180 deg on Camera Module 3-style boards such as the
 * Arducam B0310), giving an upright, unmirrored picture for a module mounted
 * like the OV5647 was - see docs/lens-calibration.md. With a bit set the
 * readout runs from high to low sensor addresses, i.e. the window is
 * mirrored / flipped. */
#ifdef CONFIG_EXAMPLE_IMX708_HMIRROR
#define LENS_IMX708_HMIRROR          1
#else
#define LENS_IMX708_HMIRROR          0
#endif
#ifdef CONFIG_EXAMPLE_IMX708_VFLIP
#define LENS_IMX708_VFLIP            1
#else
#define LENS_IMX708_VFLIP            0
#endif

/* 1: the digital-crop offsets count in readout order, i.e. after the 0x0101
 * mirror/flip (a larger crop_x then selects lower sensor x when mirrored).
 * Verified on hardware: with mirror + flip, moving the lens centre by
 * (+384, +216) sensor px moved the picture content by (+192, +108) output px. */
#define LENS_IMX708_CROP_IN_READOUT_ORDER 1

#define LENS_NVS_NAMESPACE      "lens_cal"
#define LENS_NVS_KEY_LEGACY     "calib"       /* single-sensor (OV5647) firmware */
#define LENS_NVS_VERSION        0x4C430001u   /* 'LC' v1 */

typedef struct {
    uint32_t version;
    double cx, cy;
    uint32_t has_distortion;
    double fx, fy, k1, k2, p1, p2, k3;
} lens_nvs_record_t;

static const lens_sensor_geometry_t s_geometries[] = {
    {
        .kind = LENS_SENSOR_OV5647, .name = "OV5647",
        .array_width = LENS_SENSOR_ARRAY_WIDTH, .array_height = LENS_SENSOR_ARRAY_HEIGHT,
        .active_x = LENS_SENSOR_ACTIVE_X, .active_y = LENS_SENSOR_ACTIVE_Y,
        .active_width = LENS_SENSOR_ACTIVE_WIDTH, .active_height = LENS_SENSOR_ACTIVE_HEIGHT,
    },
    {
        .kind = LENS_SENSOR_IMX708, .name = "IMX708",
        .array_width = LENS_IMX708_ARRAY_WIDTH, .array_height = LENS_IMX708_ARRAY_HEIGHT,
        .active_x = 0, .active_y = 0,
        .active_width = LENS_IMX708_ARRAY_WIDTH, .active_height = LENS_IMX708_ARRAY_HEIGHT,
    },
};

static const lens_sensor_geometry_t *s_geo;     /* selected sensor; NULL = none */
static lens_calibration_t s_cal;
static lens_ov5647_reg_t s_slot_regs[LENS_SLOT_COUNT][LENS_MAX_REGS];
static esp_cam_sensor_format_t s_slot_fmt[LENS_SLOT_COUNT];
static esp_cam_sensor_isp_info_t s_slot_isp[LENS_SLOT_COUNT];   /* IMX708: flip-corrected Bayer phase */

static const lens_sensor_geometry_t *find_geometry(const char *sensor_name)
{
    if (!sensor_name) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_geometries) / sizeof(s_geometries[0]); i++) {
        if (strcmp(sensor_name, s_geometries[i].name) == 0) {
            return &s_geometries[i];
        }
    }
    return NULL;
}

/* NVS key of a sensor: "calib_" + lower-case sensor name (NVS keys are <= 15 chars). */
static void nvs_key_for(const lens_sensor_geometry_t *geo, char *key, size_t size)
{
    size_t n = (size_t)snprintf(key, size, "calib_");
    for (const char *c = geo->name; *c && n + 1 < size; c++) {
        key[n++] = (char)tolower((unsigned char)*c);
    }
    key[n] = '\0';
}

static void default_calibration(const lens_sensor_geometry_t *geo, lens_calibration_t *cal)
{
    memset(cal, 0, sizeof(*cal));
    cal->cx = geo->active_x + geo->active_width / 2.0;
    cal->cy = geo->active_y + geo->active_height / 2.0;
    cal->calibrated = false;
    cal->has_distortion = false;
}

void lens_calib_get_default(lens_calibration_t *cal)
{
    default_calibration(s_geo ? s_geo : &s_geometries[0], cal);
}

esp_err_t lens_calib_init(void)
{
    s_geo = NULL;
    lens_calib_get_default(&s_cal);

    /* Migrate the record of the single-sensor firmware, which always belonged
     * to an OV5647, to that sensor's own key. */
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LENS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s)", esp_err_to_name(ret));
        return ret;
    }
    size_t legacy_size = 0;
    if (nvs_get_blob(nvs, LENS_NVS_KEY_LEGACY, NULL, &legacy_size) == ESP_OK) {
        char key[16];
        nvs_key_for(&s_geometries[0], key, sizeof(key));
        size_t existing = 0;
        ret = ESP_OK;
        if (nvs_get_blob(nvs, key, NULL, &existing) == ESP_ERR_NVS_NOT_FOUND) {
            lens_nvs_record_t rec = {0};
            size_t size = sizeof(rec);
            ret = nvs_get_blob(nvs, LENS_NVS_KEY_LEGACY, &rec, &size);
            if (ret == ESP_OK && size == sizeof(rec)) {
                ret = nvs_set_blob(nvs, key, &rec, sizeof(rec));
            }
        }
        if (ret == ESP_OK) {
            ret = nvs_erase_key(nvs, LENS_NVS_KEY_LEGACY);
        }
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "migrated lens calibration \"%s\" -> \"%s\"", LENS_NVS_KEY_LEGACY, key);
        } else {
            ESP_LOGW(TAG, "failed to migrate the legacy lens calibration (%s)", esp_err_to_name(ret));
        }
    }
    nvs_close(nvs);
    return ESP_OK;
}

/* Load the selected sensor's calibration from NVS into s_cal (defaults when unset). */
static void load_calibration(void)
{
    char key[16];
    nvs_key_for(s_geo, key, sizeof(key));
    default_calibration(s_geo, &s_cal);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LENS_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "%s: no stored lens calibration; using default centre (%.1f, %.1f)", s_geo->name,
                 s_cal.cx, s_cal.cy);
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed (%s); using defaults", esp_err_to_name(ret));
        return;
    }

    lens_nvs_record_t rec = {0};
    size_t size = sizeof(rec);
    ret = nvs_get_blob(nvs, key, &rec, &size);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "%s: no stored lens calibration; using default centre (%.1f, %.1f)", s_geo->name,
                 s_cal.cx, s_cal.cy);
        return;
    }
    if (ret != ESP_OK || size != sizeof(rec) || rec.version != LENS_NVS_VERSION ||
            !isfinite(rec.cx) || !isfinite(rec.cy) ||
            rec.cx < s_geo->active_x || rec.cx > s_geo->active_x + s_geo->active_width ||
            rec.cy < s_geo->active_y || rec.cy > s_geo->active_y + s_geo->active_height) {
        ESP_LOGW(TAG, "%s: stored lens calibration is invalid (%s); using defaults", s_geo->name,
                 esp_err_to_name(ret));
        return;
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
    ESP_LOGI(TAG, "%s: loaded lens calibration \"%s\": centre (%.2f, %.2f), distortion %s", s_geo->name, key,
             s_cal.cx, s_cal.cy, s_cal.has_distortion ? "set" : "none");
}

bool lens_sensor_select(const char *sensor_name)
{
    const lens_sensor_geometry_t *geo = find_geometry(sensor_name);
    if (geo == s_geo) {
        return geo != NULL;
    }
    s_geo = geo;
    if (!geo) {
        lens_calib_get_default(&s_cal);
        return false;
    }
    load_calibration();
    return true;
}

const lens_sensor_geometry_t *lens_sensor_geometry(void)
{
    return s_geo;
}

bool lens_sensor_orientation(int *hmirror, int *vflip)
{
    if (!s_geo || s_geo->kind != LENS_SENSOR_IMX708) {
        return false;
    }
    *hmirror = LENS_IMX708_HMIRROR;
    *vflip = LENS_IMX708_VFLIP;
    return true;
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
    if (!s_geo) {
        return ESP_ERR_INVALID_STATE;
    }
    char key[16];
    nvs_key_for(s_geo, key, sizeof(key));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(LENS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed (%s)", esp_err_to_name(ret));
        return ret;
    }

    if (!s_cal.calibrated) {
        ret = nvs_erase_key(nvs, key);
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
        ret = nvs_set_blob(nvs, key, &rec, sizeof(rec));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to save lens calibration \"%s\" (%s)", key, esp_err_to_name(ret));
    }
    return ret;
}

bool lens_sensor_supported(const char *sensor_name)
{
    return find_geometry(sensor_name) != NULL;
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

/* ===================================================================== */
/* OV5647                                                                 */
/* ===================================================================== */

static bool ov5647_window_compute(const esp_cam_sensor_format_t *fmt, const lens_calibration_t *cal, lens_window_t *win)
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
    win->scale = win->scale_x;
    win->scale_v = win->scale_y;
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

/* VTS (0x380E/0x380F) of an OV5647 table, 0 if absent. */
static int ov5647_table_vts(const esp_cam_sensor_format_t *fmt)
{
    const lens_ov5647_reg_t *regs = (const lens_ov5647_reg_t *)fmt->regs;
    int max = fmt->regs_size > 0 ? fmt->regs_size : LENS_MAX_REGS;
    uint8_t hi = 0, lo = 0;
    if (!regs || !table_get(regs, max, 0x380e, &hi) || !table_get(regs, max, 0x380f, &lo)) {
        return 0;
    }
    return (hi << 8) | lo;
}

double lens_ov5647_capped_fps(const esp_cam_sensor_format_t *base, double fps_cap, int *vts_out)
{
    int vts = base ? ov5647_table_vts(base) : 0;
    double fps = base ? base->fps : 0;
    if (vts_out) {
        *vts_out = vts;
    }
    if (!base || vts <= 0 || fps <= 0 || fps_cap <= 0 || fps_cap >= fps) {
        return fps;
    }
    /* Longer frame, same line time (HTS and PCLK unchanged); rounded up so the rate never exceeds the cap. */
    int vts_new = (int)ceil(vts * fps / fps_cap - 1e-6);
    if (vts_new > 0xffff) {
        vts_new = 0xffff;
    }
    if (vts_out) {
        *vts_out = vts_new;
    }
    return fps * vts / vts_new;
}

static const esp_cam_sensor_format_t *ov5647_build_format(const esp_cam_sensor_format_t *base,
                                                         const lens_calibration_t *cal,
                                                         const void *in_use_regs, lens_window_t *win,
                                                         double fps_cap)
{
    if (!ov5647_window_compute(base, cal, win)) {
        return NULL;
    }
    int vts_base = ov5647_table_vts(base), vts_new = 0;
    double fps_new = lens_ov5647_capped_fps(base, fps_cap, &vts_new);
    bool retime = vts_base > 0 && vts_new > vts_base;

    int slot = (s_slot_regs[0] == in_use_regs) ? 1 : 0;
    lens_ov5647_reg_t *out = s_slot_regs[slot];
    const lens_ov5647_reg_t *in = (const lens_ov5647_reg_t *)base->regs;
    int n_in = table_len(base);
    int n = 0;
    bool have_3820 = false, have_3821 = false;

    /* 8 window regs + 4 offset regs + 2 orientation regs + 4 AEC regs + END */
    if (n_in + 19 > LENS_MAX_REGS) {
        ESP_LOGE(TAG, "register table of %s too large (%d)", base->name ? base->name : "?", n_in);
        return NULL;
    }

    for (int i = 0; i < n_in; i++) {
        lens_ov5647_reg_t r = in[i];
        if (r.reg != LENS_OV5647_REG_DELAY && is_window_reg(r.reg)) {
            continue;
        }
        if (retime && (r.reg == 0x380e || r.reg == 0x380f)) {
            r.val = r.reg == 0x380e ? (uint8_t)(vts_new >> 8) : (uint8_t)(vts_new & 0xff);
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
    if (retime) {
        /* Frame-rate cap: let the sensor's own AEC expose up to the longer
         * frame (AEC max exposure in lines: 60 Hz 0x3A02/03, 50 Hz 0x3A14/15).
         * The driver recomputes the banding steps and max bands (0x3A08-0x3A0E)
         * from the new VTS after loading the table, so with banding on the
         * ceiling stays in whole flicker periods. */
        int max_expo = vts_new - 4;
        out[n++] = (lens_ov5647_reg_t) { 0x3a02, (uint8_t)(max_expo >> 8) };
        out[n++] = (lens_ov5647_reg_t) { 0x3a03, (uint8_t)(max_expo & 0xff) };
        out[n++] = (lens_ov5647_reg_t) { 0x3a14, (uint8_t)(max_expo >> 8) };
        out[n++] = (lens_ov5647_reg_t) { 0x3a15, (uint8_t)(max_expo & 0xff) };
    }
    out[n++] = (lens_ov5647_reg_t) { LENS_OV5647_REG_END, 0x00 };

    esp_cam_sensor_format_t *fmt = &s_slot_fmt[slot];
    *fmt = *base;
    fmt->regs = out;
    fmt->regs_size = n;
    if (retime) {
        fmt->fps = (uint8_t)(fps_new < 1.0 ? 1 : (int)floor(fps_new + 1e-6));
        ESP_LOGI(TAG, "%ux%u: frame-rate cap %.2f fps: VTS %d -> %d lines, %.2f fps", base->width, base->height,
                 fps_cap, vts_base, vts_new, fps_new);
    }

    ESP_LOGI(TAG, "%ux%u: output region x %d..%d y %d..%d (scale %d), array %d..%d x %d..%d, isp offset %d/%d, "
             "centre error (%.1f, %.1f)", base->width, base->height, win->x, win->x + win->width - 1,
             win->y, win->y + win->height - 1, win->scale_x, win->array_x_start, win->array_x_end,
             win->array_y_start, win->array_y_end, win->isp_off_x, win->isp_off_y, win->err_x, win->err_y);
    return fmt;
}

/* ===================================================================== */
/* IMX708                                                                 */
/* ===================================================================== */

/* Value of a 16-bit big-endian register pair in an IMX708 table (last write wins). */
static bool imx708_table_get16(const lens_imx708_reg_t *regs, int max, uint16_t reg, int *val)
{
    uint8_t hi = 0, lo = 0;
    bool found_hi = false, found_lo = false;
    for (int i = 0; i < max && regs[i].reg != LENS_IMX708_REG_END; i++) {
        if (regs[i].reg == reg) {
            hi = regs[i].val;
            found_hi = true;
        } else if (regs[i].reg == reg + 1) {
            lo = regs[i].val;
            found_lo = true;
        }
    }
    if (found_hi && found_lo) {
        *val = (hi << 8) | lo;
    }
    return found_hi && found_lo;
}

static bool imx708_table_get8(const lens_imx708_reg_t *regs, int max, uint16_t reg, uint8_t *val)
{
    bool found = false;
    for (int i = 0; i < max && regs[i].reg != LENS_IMX708_REG_END; i++) {
        if (regs[i].reg == reg) {
            *val = regs[i].val;
            found = true;
        }
    }
    return found;
}

static int imx708_table_len(const esp_cam_sensor_format_t *fmt)
{
    const lens_imx708_reg_t *regs = (const lens_imx708_reg_t *)fmt->regs;
    int max = fmt->regs_size > 0 ? fmt->regs_size : LENS_MAX_REGS;
    int n = 0;
    while (n < max && regs[n].reg != LENS_IMX708_REG_END) {
        n++;
    }
    return n;
}

/*
 * One axis of the IMX708 window. The mode reduces the analog window
 * [a0, a0 + field * s) by s (binning x subsampling) into a field of `field`
 * px and crops `crop` of them at offset `off` (readout order); the crop is
 * then output (optionally scaled) as the image. Region start in sensor coords:
 *   not reversed: X0 = a0 + s * off
 *   reversed    : X0 = a0 + s * (field - off - crop)
 * The offset is chosen so the region centre is closest to `c`, in steps of
 * LENS_IMX708_CROP_STEP, clamped to [0, field - crop]. A crop of the whole
 * field has a single position (centre range of zero width).
 */
static bool imx708_axis(int a0, int field, int s, int crop, bool reversed, double c,
                        int *off, int *start, double *err, double *c_min, double *c_max)
{
    const int step = LENS_IMX708_CROP_STEP;
    int off_max = ((field - crop) / step) * step;
    if (crop <= 0 || off_max < 0) {
        return false;
    }
    /* field offset whose region centre lands on c */
    double centre_field = (c - a0) / s;               /* lens centre in field px, sensor order */
    double ideal = centre_field - crop / 2.0;         /* sensor-order offset */
    if (reversed) {
        ideal = field - crop - ideal;                 /* readout-order offset */
    }
    int o = clamp_int(step * (int)lround(ideal / step), 0, off_max);
    int sensor_off = reversed ? field - o - crop : o; /* back to sensor order */
    double width = (double)crop * s;

    *off = o;
    *start = a0 + s * sensor_off;
    *err = (*start + width / 2.0) - c;
    /* Every offset in [0, off_max] is reachable; the sensor-order offsets
     * cover the same set mirrored, so the centre range is [lo, hi] either way
     * (exact when field - crop is a multiple of the step, as in all modes). */
    int lo = reversed ? field - off_max - crop : 0;
    int hi = reversed ? field - crop : off_max;
    *c_min = a0 + s * lo + width / 2.0;
    *c_max = a0 + s * hi + width / 2.0;
    return true;
}

static bool imx708_window_compute(const esp_cam_sensor_format_t *fmt, const lens_calibration_t *cal, lens_window_t *win)
{
    memset(win, 0, sizeof(*win));
    if (!fmt || !fmt->regs || !cal || fmt->width == 0 || fmt->height == 0) {
        return false;
    }

    const lens_imx708_reg_t *regs = (const lens_imx708_reg_t *)fmt->regs;
    int max = fmt->regs_size > 0 ? fmt->regs_size : LENS_MAX_REGS;
    int x0 = 0, y0 = 0, x1 = LENS_IMX708_ARRAY_WIDTH - 1, y1 = LENS_IMX708_ARRAY_HEIGHT - 1;
    int crop_w = fmt->width, crop_h = fmt->height, crop_x = 0, crop_y = 0;
    uint8_t binning = 0, binning_type = 0x11, x_odd_inc = 1, y_odd_inc = 1, scale_mode = 0, scale_m = 16;
    imx708_table_get16(regs, max, LENS_IMX708_REG_X_ADDR_START, &x0);
    imx708_table_get16(regs, max, LENS_IMX708_REG_Y_ADDR_START, &y0);
    imx708_table_get16(regs, max, LENS_IMX708_REG_X_ADDR_END, &x1);
    imx708_table_get16(regs, max, LENS_IMX708_REG_Y_ADDR_END, &y1);
    imx708_table_get8(regs, max, LENS_IMX708_REG_BINNING_MODE, &binning);
    imx708_table_get8(regs, max, LENS_IMX708_REG_BINNING_TYPE, &binning_type);
    imx708_table_get8(regs, max, LENS_IMX708_REG_X_ODD_INC, &x_odd_inc);
    imx708_table_get8(regs, max, LENS_IMX708_REG_Y_ODD_INC, &y_odd_inc);
    imx708_table_get8(regs, max, LENS_IMX708_REG_SCALE_MODE, &scale_mode);
    imx708_table_get8(regs, max, LENS_IMX708_REG_SCALE_M, &scale_m);
    if (!imx708_table_get16(regs, max, LENS_IMX708_REG_DIG_CROP_X, &crop_x) ||
            !imx708_table_get16(regs, max, LENS_IMX708_REG_DIG_CROP_Y, &crop_y)) {
        return false;   /* not a table we know how to re-centre */
    }
    imx708_table_get16(regs, max, LENS_IMX708_REG_DIG_CROP_W, &crop_w);
    imx708_table_get16(regs, max, LENS_IMX708_REG_DIG_CROP_H, &crop_h);

    int sx = (binning ? ((binning_type >> 4) & 0x0f) : 1) * ((x_odd_inc + 1) / 2);
    int sy = (binning ? (binning_type & 0x0f) : 1) * ((y_odd_inc + 1) / 2);
    /* Scaler: output = crop * 16 / M (mode 1 horizontal only, 2 both axes). */
    double zx = (scale_mode == 1 || scale_mode == 2) && scale_m ? scale_m / 16.0 : 1.0;
    double zy = scale_mode == 2 && scale_m ? scale_m / 16.0 : 1.0;
    if (sx < 1 || sy < 1 || x0 < 0 || y0 < 0 ||
            x1 >= LENS_IMX708_ARRAY_WIDTH || y1 >= LENS_IMX708_ARRAY_HEIGHT ||
            fabs(crop_w / zx - fmt->width) > 1.0 || fabs(crop_h / zy - fmt->height) > 1.0) {
        return false;   /* output size does not match crop and scaler: unknown geometry */
    }
    int field_w = (x1 - x0 + 1) / sx;
    int field_h = (y1 - y0 + 1) / sy;

    win->scale_x = sx;
    win->scale_y = sy;
    win->width = crop_w * sx;
    win->height = crop_h * sy;
    win->scale = (double)win->width / fmt->width;
    win->scale_v = (double)win->height / fmt->height;
    win->mirror = LENS_IMX708_HMIRROR;
    win->flip = LENS_IMX708_VFLIP;

    bool rev_x = LENS_IMX708_CROP_IN_READOUT_ORDER && LENS_IMX708_HMIRROR;
    bool rev_y = LENS_IMX708_CROP_IN_READOUT_ORDER && LENS_IMX708_VFLIP;
    if (!imx708_axis(x0, field_w, sx, crop_w, rev_x, cal->cx, &win->crop_x, &win->x, &win->err_x,
                     &win->cx_min, &win->cx_max) ||
            !imx708_axis(y0, field_h, sy, crop_h, rev_y, cal->cy, &win->crop_y, &win->y, &win->err_y,
                         &win->cy_min, &win->cy_max)) {
        return false;
    }
    win->valid = true;
    return true;
}

static const esp_cam_sensor_format_t *imx708_build_format(const esp_cam_sensor_format_t *base,
                                                         const lens_calibration_t *cal,
                                                         const void *in_use_regs, lens_window_t *win)
{
    if (!imx708_window_compute(base, cal, win)) {
        return NULL;
    }

    int slot = (s_slot_regs[0] == in_use_regs) ? 1 : 0;
    lens_imx708_reg_t *out = s_slot_regs[slot];
    const lens_imx708_reg_t *in = (const lens_imx708_reg_t *)base->regs;
    int n_in = imx708_table_len(base);
    if (n_in + 1 > LENS_MAX_REGS) {
        ESP_LOGE(TAG, "register table of %s too large (%d)", base->name ? base->name : "?", n_in);
        return NULL;
    }

    /* Same table, same order; only the digital-crop offset bytes change. */
    for (int i = 0; i < n_in; i++) {
        lens_imx708_reg_t r = in[i];
        switch (r.reg) {
        case LENS_IMX708_REG_DIG_CROP_X:     r.val = (uint8_t)((win->crop_x >> 8) & 0xff); break;
        case LENS_IMX708_REG_DIG_CROP_X + 1: r.val = (uint8_t)(win->crop_x & 0xff); break;
        case LENS_IMX708_REG_DIG_CROP_Y:     r.val = (uint8_t)((win->crop_y >> 8) & 0xff); break;
        case LENS_IMX708_REG_DIG_CROP_Y + 1: r.val = (uint8_t)(win->crop_y & 0xff); break;
        default: break;
        }
        out[i] = r;
    }
    out[n_in] = (lens_imx708_reg_t) { LENS_IMX708_REG_END, 0x00 };

    esp_cam_sensor_format_t *fmt = &s_slot_fmt[slot];
    *fmt = *base;
    fmt->regs = out;
    fmt->regs_size = n_in + 1;

    /*
     * esp_video (2.5) latches the Bayer phase on VIDIOC_S_SENSOR_FMT from the
     * isp_info of the format it is handed - for the driver's table that is a
     * static RGGB entry, whatever 0x0101 says. A mirror/flip moves the 2x2
     * colour tile (RGGB -> GRBG / GBRG / BGGR, as the driver itself reports in
     * its shadow format), so hand esp_video an isp_info with the right phase.
     */
    if (base->isp_info) {
        static const esp_cam_sensor_bayer_pattern_t phase[4] = {
            ESP_CAM_SENSOR_BAYER_RGGB, ESP_CAM_SENSOR_BAYER_GRBG,
            ESP_CAM_SENSOR_BAYER_GBRG, ESP_CAM_SENSOR_BAYER_BGGR,
        };
        memcpy(&s_slot_isp[slot], base->isp_info, sizeof(s_slot_isp[slot]));
        s_slot_isp[slot].isp_v1_info.bayer_type = phase[(LENS_IMX708_VFLIP ? 2 : 0) | (LENS_IMX708_HMIRROR ? 1 : 0)];
        fmt->isp_info = &s_slot_isp[slot];
    }

    ESP_LOGI(TAG, "%ux%u: output region x %d..%d y %d..%d (scale %.3f), digital crop (%d, %d) field px, "
             "centre error (%.1f, %.1f)", base->width, base->height, win->x, win->x + win->width - 1,
             win->y, win->y + win->height - 1, win->scale, win->crop_x, win->crop_y, win->err_x, win->err_y);
    return fmt;
}

/* ===================================================================== */
/* Dispatch                                                               */
/* ===================================================================== */

bool lens_window_compute(const esp_cam_sensor_format_t *fmt, const lens_calibration_t *cal, lens_window_t *win)
{
    memset(win, 0, sizeof(*win));
    if (!s_geo) {
        return false;
    }
    switch (s_geo->kind) {
    case LENS_SENSOR_OV5647:
        return ov5647_window_compute(fmt, cal, win);
    case LENS_SENSOR_IMX708:
        return imx708_window_compute(fmt, cal, win);
    default:
        return false;
    }
}

const esp_cam_sensor_format_t *lens_window_build_format(const esp_cam_sensor_format_t *base,
                                                        const lens_calibration_t *cal,
                                                        const void *in_use_regs, lens_window_t *win,
                                                        double fps_cap)
{
    memset(win, 0, sizeof(*win));
    if (!s_geo) {
        return NULL;
    }
    switch (s_geo->kind) {
    case LENS_SENSOR_OV5647:
        return ov5647_build_format(base, cal, in_use_regs, win, fps_cap);
    case LENS_SENSOR_IMX708:
        return imx708_build_format(base, cal, in_use_regs, win);
    default:
        return NULL;
    }
}

bool lens_window_full_sensor(const lens_calibration_t *cal, double scale, lens_window_t *win)
{
    memset(win, 0, sizeof(*win));
    if (!s_geo || !cal || scale <= 0) {
        return false;
    }
    win->x = s_geo->active_x;
    win->y = s_geo->active_y;
    win->width = s_geo->active_width;
    win->height = s_geo->active_height;
    win->scale = win->scale_v = scale;
    win->scale_x = win->scale_y = (int)scale;
    if (s_geo->kind == LENS_SENSOR_IMX708) {
        win->mirror = LENS_IMX708_HMIRROR;
        win->flip = LENS_IMX708_VFLIP;
    } else {
        win->mirror = LENS_IMAGE_MIRROR;
        win->flip = LENS_IMAGE_FLIP;
    }
    win->cx_min = win->cx_max = win->x + win->width / 2.0;
    win->cy_min = win->cy_max = win->y + win->height / 2.0;
    win->err_x = win->cx_min - cal->cx;
    win->err_y = win->cy_min - cal->cy;
    win->valid = true;
    return true;
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
    const lens_sensor_geometry_t *geo = s_geo ? s_geo : &s_geometries[0];
    cJSON *sensor = cJSON_CreateObject();
    cJSON_AddNumberToObject(sensor, "arrayWidth", geo->array_width);
    cJSON_AddNumberToObject(sensor, "arrayHeight", geo->array_height);
    cJSON *active = cJSON_CreateObject();
    cJSON_AddNumberToObject(active, "x", geo->active_x);
    cJSON_AddNumberToObject(active, "y", geo->active_y);
    cJSON_AddNumberToObject(active, "width", geo->active_width);
    cJSON_AddNumberToObject(active, "height", geo->active_height);
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
    cJSON_AddNumberToObject(obj, "scale", round(win->scale * 1e6) / 1e6);
    if (fabs(win->scale_v - win->scale) > 1e-9) {
        cJSON_AddNumberToObject(obj, "scaleY", round(win->scale_v * 1e6) / 1e6);
    }
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
