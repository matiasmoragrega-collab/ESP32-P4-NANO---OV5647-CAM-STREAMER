/*
 * Extra IMX708 sensor modes built in RAM (see imx708_modes.h).
 */
#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "imx708_modes.h"
#if CONFIG_CAMERA_IMX708
#include "imx708.h"
#endif

static const char *TAG = "imx708_modes";

typedef struct {
    uint16_t reg;
    uint8_t val;
} mode_reg_t;

#define MODE_REG_END        0xffff
#define MODE_MAX_REGS       160
#define MODE_BUILTIN_MAX    4
#define MODE_SLOTS          (MODE_BUILTIN_MAX + 1)  /* + the debug experimental slot */

/* Full binned field of the driver's modes (analog window = whole array, 2x2 binned). */
#define FIELD_W_BINNED      2304
#define FIELD_H_BINNED      1296

/*
 * Built-in full-view modes. The IMX708's own ways of shrinking the field all
 * failed on hardware (docs/camera-formats.md, "Full field of view"): the CCS
 * scaler (0x0401/0x0405) is hardwired to 1:1, the subsampling increments
 * (0x0383/0x0387) are ignored, and 0x0901 only knows 2x2 binning (anything
 * else reads the array unbinned). The ESP32-P4 ISP takes at most 1920x1080.
 * What remains is to scan the 2304x1296 binned field in four tiles.
 */
static const imx708_mode_spec_t s_builtin[] = {
    {
        .out_w = 2304 / 2 + IMX708_TILE_OVERLAP, .out_h = 1296 / 2 + IMX708_TILE_OVERLAP,
        .crop_w = 2304 / 2 + IMX708_TILE_OVERLAP, .crop_h = 1296 / 2 + IMX708_TILE_OVERLAP,
        .binning = 0x22, .x_odd_inc = 1, .y_odd_inc = 1, .scale_mode = 0, .scale_m = 16,
        .method = "tiled", .tiles = 2,
    },
};
_Static_assert(sizeof(s_builtin) / sizeof(s_builtin[0]) <= MODE_BUILTIN_MAX, "too many built-in modes");

static mode_reg_t s_regs[MODE_SLOTS][MODE_MAX_REGS];
static esp_cam_sensor_format_t s_fmt[MODE_SLOTS];
static imx708_mode_spec_t s_spec[MODE_SLOTS];
static char s_name[MODE_SLOTS][64];
static size_t s_builtin_count;
static bool s_have_experimental;
static bool s_inited;

static void put16(mode_reg_t *out, int *n, uint16_t reg, uint16_t val)
{
    out[(*n)++] = (mode_reg_t) { reg, (uint8_t)(val >> 8) };
    out[(*n)++] = (mode_reg_t) { (uint16_t)(reg + 1), (uint8_t)(val & 0xff) };
}

static bool is_replaced(uint16_t reg)
{
    return reg == 0x0900 || reg == 0x0901 ||
           (reg >= 0x0408 && reg <= 0x040f) ||   /* digital crop */
           (reg >= 0x034c && reg <= 0x034f);     /* output size */
}

static bool build(size_t slot, const imx708_mode_spec_t *spec, const char *suffix)
{
#if CONFIG_CAMERA_IMX708
    const esp_cam_sensor_format_t *base = imx708_format_by_index(0);
    if (!base || !base->regs) {
        return false;
    }
    int bx = spec->binning ? (spec->binning >> 4) & 0x0f : 1;
    int by = spec->binning ? spec->binning & 0x0f : 1;
    int sx = (spec->x_odd_inc + 1) / 2, sy = (spec->y_odd_inc + 1) / 2;
    if (bx < 1 || by < 1 || sx < 1 || sy < 1 || !spec->out_w || !spec->out_h ||
            !spec->crop_w || !spec->crop_h) {
        return false;
    }
    /* Field of the analog window (whole array) after binning and subsampling. */
    int field_w = FIELD_W_BINNED * 2 / (bx * sx);
    int field_h = FIELD_H_BINNED * 2 / (by * sy);
    if (spec->crop_w > field_w || spec->crop_h > field_h) {
        ESP_LOGE(TAG, "crop %ux%u exceeds the %dx%d field", spec->crop_w, spec->crop_h, field_w, field_h);
        return false;
    }
    /* Centred crop offset, even (Bayer phase); the lens window layer re-centres it. */
    int crop_x = ((field_w - spec->crop_w) / 2) & ~1;
    int crop_y = ((field_h - spec->crop_h) / 2) & ~1;

    const mode_reg_t *in = (const mode_reg_t *)base->regs;
    mode_reg_t *out = s_regs[slot];
    int n = 0;
    bool crop_done = false;
    for (int i = 0; in[i].reg != MODE_REG_END && n < MODE_MAX_REGS - 40; i++) {
        if (in[i].reg == 0x0900) {
            out[n++] = (mode_reg_t) { 0x0900, (uint8_t)(spec->binning != 0x11 ? 1 : 0) };
            continue;
        }
        if (in[i].reg == 0x0901) {
            out[n++] = (mode_reg_t) { 0x0901, spec->binning };
            continue;
        }
        if (is_replaced(in[i].reg)) {
            /* The driver's table writes crop + output size as one block; emit ours in its place. */
            if (!crop_done) {
                put16(out, &n, 0x0408, (uint16_t)crop_x);
                put16(out, &n, 0x040a, (uint16_t)crop_y);
                put16(out, &n, 0x040c, spec->crop_w);
                put16(out, &n, 0x040e, spec->crop_h);
                put16(out, &n, 0x034c, spec->out_w);
                put16(out, &n, 0x034e, spec->out_h);
                crop_done = true;
            }
            continue;
        }
        out[n++] = in[i];
    }
    if (!crop_done) {
        return false;
    }
    /* Subsampling increments and scaler: written only when not the reset default. */
    if (spec->x_odd_inc != 1 || spec->y_odd_inc != 1) {
        out[n++] = (mode_reg_t) { 0x0381, 1 };
        out[n++] = (mode_reg_t) { 0x0383, spec->x_odd_inc };
        out[n++] = (mode_reg_t) { 0x0385, 1 };
        out[n++] = (mode_reg_t) { 0x0387, spec->y_odd_inc };
    }
    if (spec->scale_mode != 0 || spec->scale_m != 16) {
        out[n++] = (mode_reg_t) { 0x0401, spec->scale_mode };
        out[n++] = (mode_reg_t) { 0x0404, 0 };
        out[n++] = (mode_reg_t) { 0x0405, spec->scale_m };
    }
    for (int i = 0; i < spec->n_extra && i < IMX708_MODE_EXTRA_REGS; i++) {
        out[n++] = (mode_reg_t) { spec->extra_reg[i], spec->extra_val[i] };
    }
    out[n++] = (mode_reg_t) { MODE_REG_END, 0 };

    s_spec[slot] = *spec;
    if (spec->tiles == 2) {
        snprintf(s_name[slot], sizeof(s_name[slot]), "IMX708_RAW10_%dx%d_%s_%s", field_w, field_h,
                 spec->method[0] ? spec->method : "tiled", suffix);
    } else {
        snprintf(s_name[slot], sizeof(s_name[slot]), "IMX708_RAW10_%ux%u_%s_%s", spec->out_w, spec->out_h,
                 spec->method[0] ? spec->method : "custom", suffix);
    }
    esp_cam_sensor_format_t *fmt = &s_fmt[slot];
    *fmt = *base;
    fmt->name = s_name[slot];
    fmt->width = spec->out_w;
    fmt->height = spec->out_h;
    fmt->regs = out;
    fmt->regs_size = n;
    ESP_LOGI(TAG, "mode %s: %d regs, field %dx%d, crop %ux%u at (%d, %d)", fmt->name, n, field_w, field_h,
             spec->crop_w, spec->crop_h, crop_x, crop_y);
    return true;
#else
    (void)slot;
    (void)spec;
    (void)suffix;
    return false;
#endif
}

void imx708_modes_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;
    s_builtin_count = 0;
    for (size_t i = 0; i < sizeof(s_builtin) / sizeof(s_builtin[0]); i++) {
        if (build(s_builtin_count, &s_builtin[i], "fullview")) {
            s_builtin_count++;
        }
    }
}

size_t imx708_modes_count(void)
{
    return s_builtin_count + (s_have_experimental ? 1 : 0);
}

const esp_cam_sensor_format_t *imx708_modes_get(size_t i)
{
    if (i < s_builtin_count) {
        return &s_fmt[i];
    }
    if (i == s_builtin_count && s_have_experimental) {
        return &s_fmt[MODE_BUILTIN_MAX];
    }
    return NULL;
}

const char *imx708_modes_method(const esp_cam_sensor_format_t *fmt)
{
    if (!fmt || !fmt->name) {
        return "crop";
    }
    for (size_t i = 0; i < MODE_SLOTS; i++) {
        if (s_fmt[i].name && strcmp(fmt->name, s_fmt[i].name) == 0) {
            return s_spec[i].method[0] ? s_spec[i].method : "custom";
        }
    }
    return "crop";
}

esp_err_t imx708_modes_set_experimental(const imx708_mode_spec_t *spec, size_t *index)
{
    imx708_modes_init();
    if (!build(MODE_BUILTIN_MAX, spec, "exp")) {
        return ESP_ERR_INVALID_ARG;
    }
    s_have_experimental = true;
    if (index) {
        *index = s_builtin_count;
    }
    return ESP_OK;
}

bool imx708_modes_tiling(const esp_cam_sensor_format_t *fmt, imx708_tiling_t *t)
{
    if (!fmt || !fmt->name) {
        return false;
    }
    for (size_t i = 0; i < MODE_SLOTS; i++) {
        const imx708_mode_spec_t *sp = &s_spec[i];
        if (!s_fmt[i].name || strcmp(fmt->name, s_fmt[i].name) != 0 || sp->tiles != 2) {
            continue;
        }
        if (!t) {
            return true;
        }
        int fw = FIELD_W_BINNED, fh = FIELD_H_BINNED;
        memset(t, 0, sizeof(*t));
        t->count = 4;
        t->tile_w = sp->crop_w;
        t->tile_h = sp->crop_h;
        t->canvas_w = fw;
        t->canvas_h = fh;
        t->part_w = fw / 2;
        t->part_h = fh / 2;
        /* Raster order in the image (readout order): top-left, top-right, bottom-left, bottom-right. */
        for (int k = 0; k < 4; k++) {
            int ix = k & 1, iy = k >> 1;
            t->crop_x[k] = ix ? fw - sp->crop_w : 0;
            t->crop_y[k] = iy ? fh - sp->crop_h : 0;
            t->dst_x[k] = ix * t->part_w;
            t->dst_y[k] = iy * t->part_h;
            t->src_x[k] = t->dst_x[k] - t->crop_x[k];
            t->src_y[k] = t->dst_y[k] - t->crop_y[k];
        }
        return true;
    }
    return false;
}
