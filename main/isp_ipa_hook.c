/*
 * See isp_ipa_hook.h. Runs in esp_video's ISP pipeline task.
 *
 * IMX708 colour pipeline
 * ======================
 *
 * The ESP32-P4 before rev 3.0 (this board is v1.3) has no black-level (BLC)
 * and no white-balance-gain (WBG) block in its ISP. esp_video therefore folds
 * the white-balance gains into the colour-correction matrix, and nothing ever
 * removes the sensor's black pedestal. For the IMX708 that causes two visible
 * faults, measured on hardware (see docs/camera-formats.md, "IMX708 image
 * quality"):
 *
 *  1. Magenta highlights. The green channel saturates first; the CCM then
 *     multiplies the unsaturated red/blue by gains of 1.6..3 and the matrix's
 *     negative terms subtract the over-range red/blue from green, so a clipped
 *     white comes out (1, 0.6, 1) - magenta. A real ISP applies WB in the raw
 *     domain and clips there. We do the same with the LSC (lens shading) block,
 *     which is a per-Bayer-channel gain applied before demosaic whose output
 *     saturates: flat LSC tables carry the WB gains, the ISP's own WB stays at
 *     1.0. A clipped pixel then reaches the CCM as (255, 255, 255), which the
 *     CCM (rows summing to >= 1) keeps white.
 *
 *  2. Lifted, tinted blacks and a wrong AWB. The IMX708 outputs a fixed
 *     pedestal of 64/1023 (~16/255 in the ISP's 8-bit domain, 15.0 effective
 *     after the ISP's internal truncations; register 0x0008 is read-only). Multiplied by WB gains and the CCM it becomes a large
 *     purple offset in the shadows and lifts black to ~40/255 after gamma; it
 *     also biases the AWB statistics towards grey. The offset is a constant
 *     vector after the CCM, so it is removed exactly in the per-channel gamma
 *     LUT (the last linear-domain point we control): each channel's LUT
 *     subtracts its offset o_c and stretches by k_c = 255 / (255 - o_c), so a
 *     clipped channel still maps to 255. The k_c differ per channel, so the
 *     CCM sent to the ISP is pre-scaled by c / k_c (c = max k_c, keeping rows
 *     >= 1 so saturated pixels stay white): the net transform for unclipped
 *     pixels is exactly c * CCM_tuning * WB * (raw - pedestal).
 *
 * The IPA (esp_ipa) keeps running unchanged, in raw units: before it sees the
 * statistics, the AE luminance and the AWB white-patch sums have the pedestal
 * removed and the LSC gains divided out; the AWB white-patch window it asks
 * for is mapped into the post-LSC domain the ISP measures in. Its WB gains,
 * CCM and gamma curve are intercepted and re-expressed as above.
 *
 * Only the IMX708 uses this; other sensors (OV5647) pass straight through.
 */
#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ipa.h"
#include "isp_ipa_hook.h"

static const char *TAG = "isp_hook";

/* The real esp_ipa_pipeline_init()/process(), reached through -Wl,--wrap. */
esp_err_t __real_esp_ipa_pipeline_init(esp_ipa_pipeline_handle_t handle, const esp_ipa_sensor_t *sensor,
                                       esp_ipa_metadata_t *metadata);
esp_err_t __real_esp_ipa_pipeline_process(esp_ipa_pipeline_handle_t handle, const esp_ipa_stats_t *stats,
        const esp_ipa_sensor_t *sensor, esp_ipa_metadata_t *metadata);

#ifndef CONFIG_EXAMPLE_IMX708_COLOUR_PIPELINE
#define CONFIG_EXAMPLE_IMX708_COLOUR_PIPELINE 0
#endif

/*
 * IMX708 black pedestal in the ISP's 8-bit RGB domain. The sensor's pedestal
 * is 64/1023 (register 0x0008, read-only; measured 63.6 with the RAW path at
 * minimum exposure) and the RGB path works on raw10 / 4 - but the ISP
 * truncates at every stage (10 -> 8 bit, LSC, CCM), which lowers the
 * effective black by a fraction of an LSB. That fraction matters: through the
 * tone curve's steep toe one LSB is several output levels, so the default is
 * the value that kept a grey card neutral down an exposure ladder (see
 * docs/camera-formats.md). Adjustable at run time through the debug API.
 */
#define IMX708_PEDESTAL_X100    1500
#define IMX708_PEDESTAL_DEFAULT ((float)IMX708_PEDESTAL_X100 / 100.0f)

/*
 * Weights of the P4 ISP's AE block luminance, measured with the sensor's solid
 * test pattern and per-channel LSC gains (doubling R, G, B moved a 64-level
 * patch by 18, 35, 6). The AE statistics are taken after LSC, before the CCM.
 */
#define AE_W_R  0.28f
#define AE_W_G  0.55f
#define AE_W_B  0.094f

/* LSC gain: 2 integer + 8 fractional bits. */
#define LSC_GAIN_MAX            3.99f
#define LSC_GAIN_MIN            0.25f
/*
 * Entries per LSC table. esp_video sizes the grid from the current resolution
 * without checking the table it was given, so the tables are sized for the
 * largest grid any mode can need (2304x1296 -> 37 x 22 = 814 cells).
 */
#define LSC_TABLE_ENTRIES       1024

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static char s_sensor[16];

static isp_hook_snapshot_t s_snap;
static isp_hook_overrides_t s_ov;
static uint32_t s_dirty;            /* override groups to push on the next frame */
static uint32_t s_restore_flags;    /* IPA groups to re-push on the next frame */
static esp_ipa_stats_t s_stats_copy;

/*
 * Statistics averaging over the last N frames (isp_hook_set_stats_average):
 * used by the IMX708 full-view tile scan, where every frame shows a
 * different quarter of the scene. AE/AWB then see the whole scene in every
 * frame instead of hunting between the tiles.
 */
#define STATS_AVG_MAX   4
static int s_avg_n = 1;
static int s_avg_pos, s_avg_fill;
static esp_ipa_stats_ae_t s_avg_ae[STATS_AVG_MAX][ISP_AE_REGIONS];
static esp_ipa_stats_awb_t s_avg_awb[STATS_AVG_MAX];
static uint32_t s_avg_hist[STATS_AVG_MAX][ISP_HIST_SEGMENT_NUMS];

/* The sensor state esp_video hands to the IPA (its own struct; see isp_hook_set_sensor_max_exposure). */
static esp_ipa_sensor_t *volatile s_ipa_sensor;

/* Debug LSC tables (flat gains). Kept alive while programmed: the ISP driver keeps the pointers. */
static isp_lsc_gain_t *s_dbg_lsc[4];
static uint32_t s_dbg_lsc_size;

#define DIRTY_AWB   (1u << 0)
#define DIRTY_CCM   (1u << 1)
#define DIRTY_GAMMA (1u << 2)
#define DIRTY_LSC   (1u << 3)
#define DIRTY_ALL   (DIRTY_AWB | DIRTY_CCM | DIRTY_GAMMA | DIRTY_LSC)

/* IMX708 pipeline state */
static struct {
    bool enabled;                   /* wanted (Kconfig / debug toggle) */
    bool running;                   /* what was applied last frame */
    uint32_t dirty;                 /* DIRTY_* groups to recompute and push */
    float wb[3];                    /* WB gains in raw units (R, G, B) */
    float lsc[4];                   /* R, Gr, Gb, B programmed */
    float ccm_t[3][3];              /* tuning CCM (from the IPA or a debug override) */
    float ccm_isp[3][3];
    float offset[3];
    float stretch[3];
    float ccm_scale;
    float pedestal;                 /* sensor black in the ISP's 8-bit domain */
    esp_ipa_gamma_t gamma_t;        /* tuning gamma curve */
    bool have_gamma;
    esp_ipa_awb_range_t awb_t;      /* tuning AWB white-patch window (raw units) */
    bool have_awb;
    isp_lsc_gain_t *tables[4];
} s_px = {
    .enabled = CONFIG_EXAMPLE_IMX708_COLOUR_PIPELINE,
    .wb = { 1.0f, 1.0f, 1.0f },
    .lsc = { 1.0f, 1.0f, 1.0f, 1.0f },
    .ccm_t = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} },
    .ccm_scale = 1.0f,
    .pedestal = IMX708_PEDESTAL_DEFAULT,
};

static void lock_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        isp_hook_default_overrides(&s_ov);
    }
}

static void lock(void)
{
    lock_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

void isp_hook_default_overrides(isp_hook_overrides_t *ov)
{
    memset(ov, 0, sizeof(*ov));
    ov->red_gain = 1.0f;
    ov->blue_gain = 1.0f;
    for (int i = 0; i < 3; i++) {
        ov->ccm[i][i] = 1.0f;
    }
    ov->gamma = 1.0f;
    for (int i = 0; i < 4; i++) {
        ov->lsc_gain[i] = 1.0f;
    }
    ov->ae_luma_scale = 1.0f;
}

void isp_hook_set_sensor(const char *sensor_name)
{
    lock();
    strlcpy(s_sensor, sensor_name ? sensor_name : "", sizeof(s_sensor));
    unlock();
}

const char *isp_hook_sensor(void)
{
    return s_sensor;
}

/* Copy the metadata groups named in `flags` from src to dst. */
static void copy_groups(esp_ipa_metadata_t *dst, const esp_ipa_metadata_t *src, uint32_t flags)
{
    if (flags & IPA_METADATA_FLAGS_RG) {
        dst->red_gain = src->red_gain;
    }
    if (flags & IPA_METADATA_FLAGS_BG) {
        dst->blue_gain = src->blue_gain;
    }
    if (flags & IPA_METADATA_FLAGS_ET) {
        dst->exposure = src->exposure;
    }
    if (flags & IPA_METADATA_FLAGS_GN) {
        dst->gain = src->gain;
    }
    if (flags & IPA_METADATA_FLAGS_BF) {
        dst->bf = src->bf;
    }
    if (flags & IPA_METADATA_FLAGS_SH) {
        dst->sharpen = src->sharpen;
    }
    if (flags & IPA_METADATA_FLAGS_GAMMA) {
        dst->gamma = src->gamma;
    }
    if (flags & IPA_METADATA_FLAGS_CCM) {
        dst->ccm = src->ccm;
    }
    if (flags & IPA_METADATA_FLAGS_BR) {
        dst->brightness = src->brightness;
    }
    if (flags & IPA_METADATA_FLAGS_CN) {
        dst->contrast = src->contrast;
    }
    if (flags & IPA_METADATA_FLAGS_ST) {
        dst->saturation = src->saturation;
    }
    if (flags & IPA_METADATA_FLAGS_HUE) {
        dst->hue = src->hue;
    }
    if (flags & IPA_METADATA_FLAGS_DM) {
        dst->demosaic = src->demosaic;
    }
    if (flags & IPA_METADATA_FLAGS_LSC) {
        dst->lsc = src->lsc;
    }
    if (flags & IPA_METADATA_FLAGS_AETL) {
        dst->ae_target_level = src->ae_target_level;
    }
    if (flags & IPA_METADATA_FLAGS_SR) {
        dst->stats_region = src->stats_region;
    }
    if (flags & IPA_METADATA_FLAGS_AF) {
        dst->af = src->af;
    }
    if (flags & IPA_METADATA_FLAGS_FP) {
        dst->focus_pos = src->focus_pos;
    }
    if (flags & IPA_METADATA_FLAGS_BLC) {
        dst->blc = src->blc;
    }
    if (flags & IPA_METADATA_FLAGS_AWB) {
        dst->awb = src->awb;
    }
}

/* Gamma curve on the ISP's 16-point grid (x = 16, 32, ..., 240, 255). */
static void build_gamma(esp_ipa_gamma_t *g, float gamma, const uint8_t black[3])
{
    esp_ipa_gamma_channel_t *ch[3] = { &g->red, &g->green, &g->blue };
    float bmax = MAX(black[0], MAX(black[1], black[2]));
    for (int c = 0; c < 3; c++) {
        float b = black[c];
        for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
            int x = MIN((i + 1) * (256 / ISP_GAMMA_CURVE_POINTS_NUM), 255);
            /* common stretch: keeps the white balance, saturated channels still reach 255 */
            float v = ((float)x - b) / (255.0f - bmax);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            v = powf(v, gamma);
            int y = (int)lroundf(v * 255.0f);
            ch[c]->x[i] = (uint8_t)x;
            ch[c]->y[i] = (uint8_t)MIN(y, 255);
        }
    }
    g->flags = IPA_GAMMA_FLAGS_RED | IPA_GAMMA_FLAGS_GREEN | IPA_GAMMA_FLAGS_BLUE;
}

/* Tone curves on a log-spaced grid (power-of-two steps; fine near black). */
static const uint8_t s_log_x[ISP_GAMMA_CURVE_POINTS_NUM] = { 1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 160, 192, 255 };

static float tone_eval(int curve, float v)
{
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    switch (curve) {
    case 1: /* sRGB */
        return v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    case 2: /* BT.709 (toe slope 4.5) */
        return v < 0.018f ? 4.5f * v : 1.099f * powf(v, 0.45f) - 0.099f;
    default:
        return v;
    }
}

static void build_tone(esp_ipa_gamma_t *g, int curve)
{
    esp_ipa_gamma_channel_t *ch[3] = { &g->red, &g->green, &g->blue };
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
            ch[c]->x[i] = s_log_x[i];
            ch[c]->y[i] = (uint8_t)MIN(lroundf(255.0f * tone_eval(curve, s_log_x[i] / 255.0f)), 255);
        }
    }
    g->flags = IPA_GAMMA_FLAGS_RED | IPA_GAMMA_FLAGS_GREEN | IPA_GAMMA_FLAGS_BLUE;
}

/* Evaluate a 16-point gamma channel (implicit (0,0) start) at u in [0, 255]. */
static float gamma_eval(const esp_ipa_gamma_channel_t *ch, float u)
{
    float x0 = 0.0f, y0 = 0.0f;
    for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
        float x1 = ch->x[i], y1 = ch->y[i];
        if (u <= x1) {
            if (x1 <= x0) {
                return y1;
            }
            return y0 + (y1 - y0) * (u - x0) / (x1 - x0);
        }
        x0 = x1;
        y0 = y1;
    }
    return y0;
}

/* LSC grid count per axis, as esp_video sizes it (ISP_LSC_GET_GRIDS). */
static uint32_t lsc_grids(uint32_t res)
{
    return ((res - 1) / 2 / 32) + 2;
}

static isp_lsc_gain_t lsc_gain_code(float g)
{
    isp_lsc_gain_t v = { .val = 0 };
    g = g < 0.0f ? 0.0f : (g > 3.996f ? 3.996f : g);
    uint32_t q = (uint32_t)lroundf(g * 256.0f);
    v.integer = q >> 8;
    v.decimal = q & 0xff;
    return v;
}

static void fill_tables(isp_lsc_gain_t *tables[4], uint32_t n, const float gain[4])
{
    for (int c = 0; c < 4; c++) {
        isp_lsc_gain_t v = lsc_gain_code(gain[c]);
        for (uint32_t i = 0; i < n; i++) {
            tables[c][i] = v;
        }
    }
}

static bool alloc_tables(isp_lsc_gain_t *tables[4], uint32_t n)
{
    for (int c = 0; c < 4; c++) {
        if (!tables[c]) {
            tables[c] = heap_caps_calloc(n, sizeof(isp_lsc_gain_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
            if (!tables[c]) {
                return false;
            }
        }
    }
    return true;
}

/* Fill flat debug LSC tables sized for width x height. */
static bool fill_flat_lsc(isp_lsc_gain_t *tables[4], uint32_t *size, uint32_t width, uint32_t height,
                          const float gain[4])
{
    uint32_t n = lsc_grids(width) * lsc_grids(height);
    if (n > LSC_TABLE_ENTRIES || !alloc_tables(tables, LSC_TABLE_ENTRIES)) {
        return false;
    }
    fill_tables(tables, LSC_TABLE_ENTRIES, gain);
    *size = n;
    return true;
}

static void set_lsc_metadata(esp_ipa_metadata_t *md, isp_lsc_gain_t *tables[4], uint32_t n)
{
    md->lsc.enable = true;
    md->lsc.gain_r = tables[0];
    md->lsc.gain_gr = tables[1];
    md->lsc.gain_gb = tables[2];
    md->lsc.gain_b = tables[3];
    md->lsc.lsc_gain_array_size = n;
    md->flags |= IPA_METADATA_FLAGS_LSC;
}

static bool px_wanted(void)
{
    return s_px.enabled && strcmp(s_sensor, "IMX708") == 0 && !s_ov.lsc_manual;
}

/* Raw-domain statistics for the IPA: remove the pedestal and divide out the LSC (WB) gains. */
static const esp_ipa_stats_t *px_stats(const esp_ipa_stats_t *stats)
{
    const float p = s_px.pedestal;
    if (stats != &s_stats_copy) {
        s_stats_copy = *stats;
    }
    if (stats->flags & IPA_STATS_FLAGS_AE) {
        float yp = p * (AE_W_R * s_px.lsc[0] + AE_W_G * 0.5f * (s_px.lsc[1] + s_px.lsc[2]) + AE_W_B * s_px.lsc[3]);
        for (int i = 0; i < ISP_AE_REGIONS; i++) {
            float l = (float)s_stats_copy.ae_stats[i].luminance - yp;
            s_stats_copy.ae_stats[i].luminance = l > 0.0f ? (uint32_t)lroundf(l) : 0;
        }
    }
    if ((stats->flags & IPA_STATS_FLAGS_AWB) && s_stats_copy.awb_stats[0].counted) {
        esp_ipa_stats_awb_t *a = &s_stats_copy.awb_stats[0];
        double n = a->counted;
        double r = a->sum_r / (double)s_px.lsc[0] - p * n;
        double g = a->sum_g / (double)(0.5f * (s_px.lsc[1] + s_px.lsc[2])) - p * n;
        double b = a->sum_b / (double)s_px.lsc[3] - p * n;
        a->sum_r = r > 0 ? (uint32_t)r : 0;
        a->sum_g = g > 0 ? (uint32_t)g : 0;
        a->sum_b = b > 0 ? (uint32_t)b : 0;
    }
    return &s_stats_copy;
}

/* Black offsets / stretch / ISP CCM for the current WB and tuning CCM (see header comment). */
static void px_solve(void)
{
    const float p = s_px.pedestal;
    const float g[3] = { s_px.lsc[0], 0.5f * (s_px.lsc[1] + s_px.lsc[2]), s_px.lsc[3] };
    float b[3], rowsum[3];
    for (int c = 0; c < 3; c++) {
        b[c] = 0.0f;
        rowsum[c] = 0.0f;
        for (int j = 0; j < 3; j++) {
            b[c] += s_px.ccm_t[c][j] * g[j] * p;
            rowsum[c] += s_px.ccm_t[c][j];
        }
        if (rowsum[c] < 0.5f) {
            rowsum[c] = 0.5f;
        }
    }
    float cs = 1.0f;
    for (int it = 0; it < 8; it++) {
        float next = 1.0f;
        for (int c = 0; c < 3; c++) {
            float o = cs * b[c] / (1.0f + cs * b[c] / 255.0f);
            o = MIN(o, 200.0f);
            s_px.offset[c] = o;
            s_px.stretch[c] = 255.0f / (255.0f - o);
            next = MAX(next, s_px.stretch[c] / rowsum[c]);
        }
        cs = next;
    }
    s_px.ccm_scale = cs;
    for (int c = 0; c < 3; c++) {
        for (int j = 0; j < 3; j++) {
            s_px.ccm_isp[c][j] = cs / s_px.stretch[c] * s_px.ccm_t[c][j];
        }
    }
}

/*
 * x grid for one channel. The ISP only requires power-of-two steps between
 * points (from 0; the last point is 255 == 256). Each channel gets:
 *  - a point exactly at its black offset x0 (reached by x0's binary
 *    decomposition; the curve is 0 up to there). With a fixed 16-wide grid
 *    the segment from (0,0) to the first point leaked a black of up to
 *    y(16) * x0 / 16 - measured at 16/255 on green;
 *  - then log-spaced steps (1, 1, 2, 4, 8, ...) so the steep, curved toe of
 *    the tone curve is sampled finely in every channel. With uniform steps the
 *    linear interpolation sagged differently per channel (each channel's grid
 *    sits at a different offset), which tinted dark greys by up to ~9 levels;
 *    log spacing keeps it to ~2 (simulated).
 */
static void px_gamma_grid(int x0, uint16_t xs[ISP_GAMMA_CURVE_POINTS_NUM])
{
    int steps[ISP_GAMMA_CURVE_POINTS_NUM + 9];
    int n = 0;
    x0 = MIN(MAX(x0, 0), 192);
    for (int b = 7; b >= 0; b--) {
        if (x0 & (1 << b)) {
            steps[n++] = 1 << b;
        }
    }
    const int room = ISP_GAMMA_CURVE_POINTS_NUM - n;
    const int rem = 256 - x0;
    int st[ISP_GAMMA_CURVE_POINTS_NUM + 9];
    int m = 0;
    for (int len = 9; len >= 0; len--) {
        /* log run 1, 1, 2, 4, ... of `len` steps (sum 2^(len-1)), then the remainder in binary */
        int sum = len ? (1 << (len - 1)) : 0;
        if (sum > rem) {
            continue;
        }
        m = 0;
        for (int i = 0; i < len; i++) {
            st[m++] = i == 0 ? 1 : 1 << (i - 1);
        }
        int r = rem - sum;
        for (int b = 8; b >= 0; b--) {
            if (r & (1 << b)) {
                st[m++] = 1 << b;
            }
        }
        if (m <= room) {
            break;
        }
    }
    while (m < room) {          /* use every point: split the largest step */
        int big = 0;
        for (int i = 1; i < m; i++) {
            if (st[i] > st[big]) {
                big = i;
            }
        }
        if (st[big] <= 1) {
            break;
        }
        st[big] /= 2;
        st[m++] = st[big];
    }
    for (int i = 0; i < m; i++) {   /* finest steps nearest black */
        for (int j = i + 1; j < m; j++) {
            if (st[j] < st[i]) {
                int t = st[i];
                st[i] = st[j];
                st[j] = t;
            }
        }
    }
    for (int i = 0; i < m && n < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
        steps[n++] = st[i];
    }
    int x = 0;
    for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
        x += i < n ? steps[i] : 0;
        xs[i] = (uint16_t)MIN(x, 256);
    }
}

/*
 * The P4 ISP's RGB565 output truncates: R and B lose 0..7 levels, G 0..3, so
 * every tone comes out ~2 levels greener than it is - most visible in the
 * shadows. Adding half a quantisation step makes it round instead (0 stays 0:
 * 4 >> 3 == 0).
 */
static const uint8_t s_rgb565_round[3] = { 4, 2, 4 };

static void px_gamma(esp_ipa_gamma_t *out)
{
    const esp_ipa_gamma_channel_t *src[3] = { &s_px.gamma_t.green, &s_px.gamma_t.green, &s_px.gamma_t.green };
    if (s_px.gamma_t.flags & IPA_GAMMA_FLAGS_RED) {
        src[0] = &s_px.gamma_t.red;
    }
    if (s_px.gamma_t.flags & IPA_GAMMA_FLAGS_BLUE) {
        src[2] = &s_px.gamma_t.blue;
    }
    esp_ipa_gamma_channel_t *dst[3] = { &out->red, &out->green, &out->blue };
    for (int c = 0; c < 3; c++) {
        uint16_t xs[ISP_GAMMA_CURVE_POINTS_NUM];
        px_gamma_grid((int)lroundf(s_px.offset[c]), xs);
        for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
            int x = MIN(xs[i], 255);
            float u = ((float)xs[i] - s_px.offset[c]) * s_px.stretch[c];
            u = u < 0.0f ? 0.0f : (u > 255.0f ? 255.0f : u);
            long y = lroundf(gamma_eval(src[c], u));
            if (y > 0 || u > 0.0f) {
                y += s_rgb565_round[c];
            }
            dst[c]->x[i] = (uint8_t)x;
            dst[c]->y[i] = (uint8_t)MIN(y, 255);
        }
    }
    out->flags = IPA_GAMMA_FLAGS_RED | IPA_GAMMA_FLAGS_GREEN | IPA_GAMMA_FLAGS_BLUE;
}

/* The IPA's AWB window (raw, pedestal-free units) mapped to what the ISP measures (post-LSC, with pedestal). */
static void px_awb_range(esp_ipa_awb_range_t *out)
{
    const float p = s_px.pedestal;
    const esp_ipa_awb_range_t *t = &s_px.awb_t;
    float gm = 0.5f * ((float)t->green_min + (float)t->green_max);
    if (gm < 2.0f * p) {
        gm = 2.0f * p;
    }
    float gg = 0.5f * (s_px.lsc[1] + s_px.lsc[2]);
    *out = *t;
    out->rg_min = s_px.lsc[0] / gg * (t->rg_min * (gm - p) + p) / gm;
    out->rg_max = s_px.lsc[0] / gg * (t->rg_max * (gm - p) + p) / gm;
    out->bg_min = s_px.lsc[3] / gg * (t->bg_min * (gm - p) + p) / gm;
    out->bg_max = s_px.lsc[3] / gg * (t->bg_max * (gm - p) + p) / gm;
    /*
     * Hardware limits (esp_driver_isp / isp_ll): ratios are 2.8 fixed point
     * (< 4), and esp_video programs the luminance window as
     * green * (1 + rg + bg), which must stay <= 1023. A rejected window stops
     * the AWB statistics and with them the whole ISP statistics stream, so the
     * window is clamped rather than sent out of range.
     */
    out->rg_min = MIN(out->rg_min, 3.9f);
    out->rg_max = MIN(out->rg_max, 3.98f);
    out->bg_min = MIN(out->bg_min, 3.9f);
    out->bg_max = MIN(out->bg_max, 3.98f);
    float lum_scale = 1.0f + out->rg_max + out->bg_max;
    uint32_t gmax = (uint32_t)floorf(1023.0f / lum_scale);
    if (out->green_max > gmax) {
        out->green_max = (uint8_t)gmax;
    }
    if (out->green_min >= out->green_max) {
        out->green_min = out->green_max / 4;
    }
}

/*
 * Rewrite the IPA's output for the IMX708 pipeline. Called with the lock held,
 * after debug overrides have been merged into `md`.
 */
static void px_process(esp_ipa_metadata_t *md)
{
    if (!s_px.running) {
        /* (Re)activation: take over every group from the IPA's latest values. */
        s_px.running = true;
        if (s_snap.seen_flags & IPA_METADATA_FLAGS_RG) {
            s_px.wb[0] = s_snap.last.red_gain;
        }
        if (s_snap.seen_flags & IPA_METADATA_FLAGS_BG) {
            s_px.wb[2] = s_snap.last.blue_gain;
        }
        if (s_snap.seen_flags & IPA_METADATA_FLAGS_CCM) {
            memcpy(s_px.ccm_t, s_snap.last.ccm.matrix, sizeof(s_px.ccm_t));
        }
        if (s_snap.seen_flags & IPA_METADATA_FLAGS_GAMMA) {
            s_px.gamma_t = s_snap.last.gamma;
            s_px.have_gamma = true;
        }
        if (s_snap.seen_flags & IPA_METADATA_FLAGS_AWB) {
            s_px.awb_t = s_snap.last.awb;
            s_px.have_awb = true;
        }
        s_px.dirty = DIRTY_ALL;
        ESP_LOGI(TAG, "IMX708 colour pipeline active (WB in LSC, black level %.2f/255 removed in gamma)",
                 s_px.pedestal);
    }
    if (md->flags & (IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG)) {
        if (md->flags & IPA_METADATA_FLAGS_RG) {
            s_px.wb[0] = md->red_gain;
        }
        if (md->flags & IPA_METADATA_FLAGS_BG) {
            s_px.wb[2] = md->blue_gain;
        }
        md->flags &= ~(IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG);
        s_px.dirty |= DIRTY_LSC;
    }
    if (s_px.dirty == DIRTY_ALL) {
        /* Activation: the ISP's own WB (folded into the CCM by esp_video) goes to unity. */
        md->red_gain = 1.0f;
        md->blue_gain = 1.0f;
        md->flags |= IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG;
    }
    if (md->flags & IPA_METADATA_FLAGS_CCM) {
        memcpy(s_px.ccm_t, md->ccm.matrix, sizeof(s_px.ccm_t));
        md->flags &= ~IPA_METADATA_FLAGS_CCM;
        s_px.dirty |= DIRTY_CCM;
    }
    if (md->flags & IPA_METADATA_FLAGS_GAMMA) {
        s_px.gamma_t = md->gamma;
        s_px.have_gamma = true;
        md->flags &= ~IPA_METADATA_FLAGS_GAMMA;
        s_px.dirty |= DIRTY_GAMMA;
    }
    if (md->flags & IPA_METADATA_FLAGS_AWB) {
        s_px.awb_t = md->awb;
        s_px.have_awb = true;
        md->flags &= ~IPA_METADATA_FLAGS_AWB;
        s_px.dirty |= DIRTY_AWB;
    }
    md->flags &= ~IPA_METADATA_FLAGS_LSC;

    if (!s_px.dirty) {
        return;
    }
    if (s_px.dirty & DIRTY_LSC) {
        float r = MIN(MAX(s_px.wb[0], LSC_GAIN_MIN), LSC_GAIN_MAX);
        float b = MIN(MAX(s_px.wb[2], LSC_GAIN_MIN), LSC_GAIN_MAX);
        const float gains[4] = { r, 1.0f, 1.0f, b };
        if (alloc_tables(s_px.tables, LSC_TABLE_ENTRIES)) {
            memcpy(s_px.lsc, gains, sizeof(gains));
            fill_tables(s_px.tables, LSC_TABLE_ENTRIES, gains);
            uint32_t n = s_snap.sensor.width ? lsc_grids(s_snap.sensor.width) * lsc_grids(s_snap.sensor.height)
                         : LSC_TABLE_ENTRIES;
            set_lsc_metadata(md, s_px.tables, MIN(n, LSC_TABLE_ENTRIES));
        } else {
            ESP_LOGE(TAG, "no memory for LSC tables");
        }
        s_px.dirty |= DIRTY_CCM | DIRTY_GAMMA | DIRTY_AWB;
    }
    if (s_px.dirty & (DIRTY_CCM | DIRTY_GAMMA)) {
        px_solve();
        memcpy(md->ccm.matrix, s_px.ccm_isp, sizeof(s_px.ccm_isp));
        md->flags |= IPA_METADATA_FLAGS_CCM;
        if (s_px.have_gamma) {
            px_gamma(&md->gamma);
            md->flags |= IPA_METADATA_FLAGS_GAMMA;
        }
    }
    if ((s_px.dirty & DIRTY_AWB) && s_px.have_awb) {
        px_awb_range(&md->awb);
        md->flags |= IPA_METADATA_FLAGS_AWB;
    }
    s_px.dirty = 0;
}

/* Hand everything back to the plain esp_video path (pipeline switched off). */
static void px_release(esp_ipa_metadata_t *md)
{
    s_px.running = false;
    md->red_gain = s_px.wb[0];
    md->blue_gain = s_px.wb[2];
    md->flags |= IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG;
    memcpy(md->ccm.matrix, s_px.ccm_t, sizeof(s_px.ccm_t));
    md->flags |= IPA_METADATA_FLAGS_CCM;
    if (s_px.have_gamma) {
        md->gamma = s_px.gamma_t;
        md->flags |= IPA_METADATA_FLAGS_GAMMA;
    }
    if (s_px.have_awb) {
        md->awb = s_px.awb_t;
        md->flags |= IPA_METADATA_FLAGS_AWB;
    }
    /* esp_video can only enable LSC from metadata: flat 1.0 tables are a no-op. */
    const float one[4] = { 1, 1, 1, 1 };
    if (alloc_tables(s_px.tables, LSC_TABLE_ENTRIES)) {
        memcpy(s_px.lsc, one, sizeof(one));
        fill_tables(s_px.tables, LSC_TABLE_ENTRIES, one);
        uint32_t n = s_snap.sensor.width ? lsc_grids(s_snap.sensor.width) * lsc_grids(s_snap.sensor.height)
                     : LSC_TABLE_ENTRIES;
        set_lsc_metadata(md, s_px.tables, MIN(n, LSC_TABLE_ENTRIES));
    }
    ESP_LOGI(TAG, "IMX708 colour pipeline off");
}

/* The IPA's initial values (CCM, gamma, AWB range, ...) come from init, not process. */
esp_err_t __wrap_esp_ipa_pipeline_init(esp_ipa_pipeline_handle_t handle, const esp_ipa_sensor_t *sensor,
                                       esp_ipa_metadata_t *metadata)
{
    esp_err_t ret = __real_esp_ipa_pipeline_init(handle, sensor, metadata);
    if (ret == ESP_OK && metadata) {
        lock();
        copy_groups(&s_snap.last, metadata, metadata->flags);
        s_snap.seen_flags |= metadata->flags;
        s_snap.init_flags = metadata->flags;
        s_px.running = false;   /* re-take every group on the next frame */
        unlock();
    }
    return ret;
}

/* Replace the AE/AWB/histogram statistics by their mean over the last s_avg_n frames. Caller holds the lock. */
static const esp_ipa_stats_t *avg_stats(const esp_ipa_stats_t *stats)
{
    if (stats != &s_stats_copy) {
        s_stats_copy = *stats;
    }
    esp_ipa_stats_t *st = &s_stats_copy;
    int k = s_avg_pos;
    memcpy(s_avg_ae[k], st->ae_stats, sizeof(s_avg_ae[k]));
    s_avg_awb[k] = st->awb_stats[0];
    for (int i = 0; i < ISP_HIST_SEGMENT_NUMS; i++) {
        s_avg_hist[k][i] = st->hist_stats[i].value;
    }
    s_avg_pos = (s_avg_pos + 1) % s_avg_n;
    if (s_avg_fill < s_avg_n) {
        s_avg_fill++;
    }
    int n = s_avg_fill;
    if (st->flags & IPA_STATS_FLAGS_AE) {
        for (int i = 0; i < ISP_AE_REGIONS; i++) {
            uint32_t sum = 0;
            for (int j = 0; j < n; j++) {
                sum += s_avg_ae[j][i].luminance;
            }
            st->ae_stats[i].luminance = (sum + n / 2) / n;
        }
    }
    if (st->flags & IPA_STATS_FLAGS_AWB) {
        uint64_t c = 0, r = 0, g = 0, b = 0;
        for (int j = 0; j < n; j++) {
            c += s_avg_awb[j].counted;
            r += s_avg_awb[j].sum_r;
            g += s_avg_awb[j].sum_g;
            b += s_avg_awb[j].sum_b;
        }
        st->awb_stats[0].counted = (uint32_t)(c / n);
        st->awb_stats[0].sum_r = (uint32_t)(r / n);
        st->awb_stats[0].sum_g = (uint32_t)(g / n);
        st->awb_stats[0].sum_b = (uint32_t)(b / n);
    }
    if (st->flags & IPA_STATS_FLAGS_HIST) {
        for (int i = 0; i < ISP_HIST_SEGMENT_NUMS; i++) {
            uint32_t sum = 0;
            for (int j = 0; j < n; j++) {
                sum += s_avg_hist[j][i];
            }
            st->hist_stats[i].value = sum / n;
        }
    }
    return st;
}

void isp_hook_set_stats_average(int frames)
{
    if (frames < 1) {
        frames = 1;
    }
    if (frames > STATS_AVG_MAX) {
        frames = STATS_AVG_MAX;
    }
    lock();
    s_avg_n = frames;
    s_avg_pos = 0;
    s_avg_fill = 0;
    unlock();
}

bool isp_hook_set_sensor_max_exposure(uint32_t max_us)
{
    esp_ipa_sensor_t *s = s_ipa_sensor;
    if (!s || !max_us) {
        return false;
    }
    /* esp_video reads the sensor's exposure range once, when the ISP pipeline
     * is created, and checks AE ceilings against it. The struct is esp_video's
     * own (non-const) isp->sensor, handed to the IPA on every frame. */
    s->max_exposure = max_us;
    return true;
}

uint32_t isp_hook_sensor_max_exposure(void)
{
    esp_ipa_sensor_t *s = s_ipa_sensor;
    return s ? s->max_exposure : 0;
}

esp_err_t __wrap_esp_ipa_pipeline_process(esp_ipa_pipeline_handle_t handle, const esp_ipa_stats_t *stats,
        const esp_ipa_sensor_t *sensor, esp_ipa_metadata_t *metadata)
{
    if (sensor) {
        s_ipa_sensor = (esp_ipa_sensor_t *)sensor;
    }
    lock();
    const esp_ipa_stats_t *ipa_stats = stats;
    if (stats) {
        s_snap.awb_raw = stats->awb_stats[0];
        if (px_wanted() && s_px.running) {
            ipa_stats = px_stats(stats);
        }
        if (s_ov.ae_luma_scale != 1.0f && (stats->flags & IPA_STATS_FLAGS_AE)) {
            if (ipa_stats != &s_stats_copy) {
                s_stats_copy = *stats;
            }
            for (int i = 0; i < ISP_AE_REGIONS; i++) {
                float l = s_stats_copy.ae_stats[i].luminance * s_ov.ae_luma_scale;
                s_stats_copy.ae_stats[i].luminance = l > 255.0f ? 255 : (uint32_t)l;
            }
            ipa_stats = &s_stats_copy;
        }
        if (s_avg_n > 1) {
            ipa_stats = avg_stats(ipa_stats);
        }
    }
    unlock();

    esp_err_t ret = __real_esp_ipa_pipeline_process(handle, ipa_stats, sensor, metadata);
    if (ret != ESP_OK || !metadata) {
        return ret;
    }

    lock();
    s_snap.frames++;
    if (ipa_stats) {
        s_snap.seq = ipa_stats->seq;
        s_snap.stats_flags = ipa_stats->flags;
        if (ipa_stats->flags & IPA_STATS_FLAGS_AE) {
            for (int i = 0; i < ISP_AE_REGIONS; i++) {
                s_snap.ae_luma[i] = ipa_stats->ae_stats[i].luminance;
            }
        }
        if (ipa_stats->flags & IPA_STATS_FLAGS_HIST) {
            for (int i = 0; i < ISP_HIST_SEGMENT_NUMS; i++) {
                s_snap.hist[i] = ipa_stats->hist_stats[i].value;
            }
        }
        if (ipa_stats->flags & IPA_STATS_FLAGS_AWB) {
            s_snap.awb = ipa_stats->awb_stats[0];
        }
        if (ipa_stats->flags & IPA_STATS_FLAGS_SHARPEN) {
            s_snap.sharpen_max = ipa_stats->sharpen_stats.value;
        }
    }
    if (sensor) {
        s_snap.sensor = *sensor;
        s_snap.sensor.focus_info = NULL;
    }

    uint32_t flags = metadata->flags;
    copy_groups(&s_snap.last, metadata, flags);
    s_snap.seen_flags |= flags;
    s_snap.frame_flags = flags;
    for (int i = 0; i < ISP_HOOK_METADATA_GROUPS; i++) {
        if (flags & (1u << i)) {
            s_snap.updates[i]++;
        }
    }

    /* Re-push the IPA's own values after a debug reset. */
    if (s_restore_flags) {
        uint32_t r = s_restore_flags & s_snap.seen_flags;
        copy_groups(metadata, &s_snap.last, r);
        metadata->flags |= r;
        s_restore_flags = 0;
    }

    /* Debug overrides that replace an IPA output (they then flow through the IMX708 pipeline too). */
    if (s_ov.awb_manual) {
        metadata->flags &= ~(IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG);
        if (s_dirty & DIRTY_AWB) {
            metadata->red_gain = s_ov.red_gain;
            metadata->blue_gain = s_ov.blue_gain;
            metadata->flags |= IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG;
        }
    }
    if (s_ov.ccm_manual) {
        metadata->flags &= ~IPA_METADATA_FLAGS_CCM;
        if (s_dirty & DIRTY_CCM) {
            memcpy(metadata->ccm.matrix, s_ov.ccm, sizeof(s_ov.ccm));
            metadata->flags |= IPA_METADATA_FLAGS_CCM;
        }
    }
    bool want = px_wanted();
    if (s_ov.gamma_manual) {
        metadata->flags &= ~IPA_METADATA_FLAGS_GAMMA;
        if (s_dirty & DIRTY_GAMMA) {
            static const uint8_t zero[3] = { 0, 0, 0 };
            /* With the IMX708 pipeline the black offsets are its job: the override is the tone curve only. */
            if (s_ov.tone_curve) {
                build_tone(&metadata->gamma, s_ov.tone_curve);
            } else {
                build_gamma(&metadata->gamma, s_ov.gamma, want ? zero : s_ov.black);
            }
            metadata->flags |= IPA_METADATA_FLAGS_GAMMA;
        }
    }

    if (want) {
        px_process(metadata);
    } else if (s_px.running) {
        px_release(metadata);
    }

    if (s_ov.lsc_manual) {
        metadata->flags &= ~IPA_METADATA_FLAGS_LSC;
        if ((s_dirty & DIRTY_LSC) && sensor && sensor->width && sensor->height) {
            if (fill_flat_lsc(s_dbg_lsc, &s_dbg_lsc_size, sensor->width, sensor->height, s_ov.lsc_gain)) {
                set_lsc_metadata(metadata, s_dbg_lsc, s_dbg_lsc_size);
            } else {
                ESP_LOGE(TAG, "no memory for LSC tables");
            }
        }
    }
    metadata->flags &= ~s_ov.hold_flags;
    s_dirty = 0;
    s_snap.applied_flags = metadata->flags;
    unlock();
    return ret;
}

/* IPA process calls since boot: one per ISP statistics frame, i.e. the rate
 * at which frames really leave the sensor (counted even when no V4L2 buffer
 * was free to receive them). Word-sized read, no lock needed. */
uint32_t isp_hook_frame_count(void)
{
    return s_snap.frames;
}

void isp_hook_get_snapshot(isp_hook_snapshot_t *out)
{
    lock();
    *out = s_snap;
    unlock();
}

void isp_hook_get_overrides(isp_hook_overrides_t *out)
{
    lock();
    *out = s_ov;
    unlock();
}

void isp_hook_set_overrides(const isp_hook_overrides_t *ov)
{
    lock();
    s_ov = *ov;
    s_dirty = DIRTY_ALL;
    unlock();
}

void isp_hook_reset(void)
{
    lock();
    isp_hook_default_overrides(&s_ov);
    s_dirty = 0;
    s_restore_flags = 0xffffffffu & ~(IPA_METADATA_FLAGS_ET | IPA_METADATA_FLAGS_GN | IPA_METADATA_FLAGS_FP |
                                      IPA_METADATA_FLAGS_LSC);
    s_px.running = false;   /* the pipeline re-takes the re-pushed groups */
    unlock();
}

void isp_hook_restore_groups(uint32_t flags)
{
    lock();
    s_restore_flags |= flags;
    if (flags & (IPA_METADATA_FLAGS_CCM | IPA_METADATA_FLAGS_GAMMA)) {
        s_px.dirty |= DIRTY_CCM | DIRTY_GAMMA;
    }
    unlock();
}

void isp_hook_set_imx708_pedestal(float pedestal_8bit)
{
    lock();
    if (pedestal_8bit >= 0.0f && pedestal_8bit <= 64.0f) {
        s_px.pedestal = pedestal_8bit;
        s_px.dirty |= DIRTY_ALL & ~DIRTY_LSC;
        s_px.dirty |= DIRTY_CCM | DIRTY_GAMMA | DIRTY_AWB;
    }
    unlock();
}

void isp_hook_set_imx708_pipeline(bool enable)
{
    lock();
    s_px.enabled = enable;
    unlock();
}

void isp_hook_get_imx708_state(isp_hook_imx708_state_t *out)
{
    lock();
    memset(out, 0, sizeof(*out));
    out->enabled = s_px.enabled;
    out->active = s_px.running;
    memcpy(out->wb_gain, s_px.wb, sizeof(out->wb_gain));
    memcpy(out->lsc_gain, s_px.lsc, sizeof(out->lsc_gain));
    out->pedestal = s_px.pedestal;
    memcpy(out->offset, s_px.offset, sizeof(out->offset));
    memcpy(out->stretch, s_px.stretch, sizeof(out->stretch));
    out->ccm_scale = s_px.ccm_scale;
    memcpy(out->ccm_isp, s_px.ccm_isp, sizeof(out->ccm_isp));
    unlock();
}

void isp_hook_prepare_ipa_configs(void)
{
    /*
     * The project's IMX708 tuning (main/tuning/imx708_b0310.json) is generated
     * "writable", so its AGC block lives in RAM and the mains frequency can
     * come from Kconfig instead of being baked into the JSON. Must run before
     * esp_video creates the IPA pipeline.
     */
    for (int i = 0; ; i++) {
        const esp_ipa_config_t *cfg = esp_ipa_pipeline_enum_configs("IMX708", i);
        if (!cfg) {
            break;
        }
        if (!cfg->agc || !cfg->description || strcmp(cfg->description, "b0310") != 0) {
            continue;
        }
        esp_ipa_agc_config_t *agc = (esp_ipa_agc_config_t *)cfg->agc;
#if CONFIG_EXAMPLE_IMX708_ANTI_FLICKER_60HZ
        agc->anti_flicker_mode = ESP_IPA_AGC_ANTI_FLICKER_FULL;
        agc->ac_freq = 60;
#elif CONFIG_EXAMPLE_IMX708_ANTI_FLICKER_OFF
        agc->anti_flicker_mode = ESP_IPA_AGC_ANTI_FLICKER_NONE;
#else
        agc->anti_flicker_mode = ESP_IPA_AGC_ANTI_FLICKER_FULL;
        agc->ac_freq = 50;
#endif
        ESP_LOGI(TAG, "IMX708 tuning '%s': anti-flicker %s", cfg->description,
                 agc->anti_flicker_mode == ESP_IPA_AGC_ANTI_FLICKER_NONE ? "off" :
                 (agc->ac_freq == 60 ? "60 Hz" : "50 Hz"));
    }
}
