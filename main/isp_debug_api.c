/*
 * See isp_debug_api.h.
 */
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "esp_video_ioctl.h"
#include "esp_video_isp_ioctl.h"
#include "esp_video_isp_pipeline.h"
#include "esp_cam_sensor.h"
#include "isp_ipa_hook.h"
#include "isp_debug_api.h"
#include "stream_stats.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#if CONFIG_CAMERA_IMX708
#include "imx708.h"
#endif

#if CONFIG_EXAMPLE_ENABLE_ISP_DEBUG_API

static const char *TAG = "isp_dbg";
static isp_debug_api_ops_t s_ops;
static bool s_lsc_forced;   /* LSC enabled/disabled by a debug request (not by the IPA) */

#ifndef ISP_TUNING_JSON_NAME
#define ISP_TUNING_JSON_NAME ""
#endif

static const char *const s_group_names[ISP_HOOK_METADATA_GROUPS] = {
    "awb_range", "red_gain", "blue_gain", "exposure", "gain", "bf", "sharpen", "gamma", "ccm",
    "brightness", "contrast", "saturation", "hue", "demosaic", "lsc", "ae_target_level",
    "stats_region", "af", "focus_pos", "blc",
};

/* ------------------------------------------------------------------ */
/* ISP device access                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t isp_ctrl(int fd, bool set, uint32_t id, void *p, int32_t *value)
{
    if (fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    struct v4l2_ext_control control = { .id = id };
    if (p) {
        control.p_u8 = p;
    } else if (value && set) {
        control.value = *value;
    }
    struct v4l2_ext_controls controls = { .ctrl_class = V4L2_CID_USER_CLASS, .count = 1, .controls = &control };
    int r = ioctl(fd, set ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS, &controls);
    if (r == 0 && !set && value && !p) {
        *value = control.value;
    }
    return r == 0 ? ESP_OK : ESP_FAIL;
}

static cJSON *matrix_json(const float m[3][3])
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            cJSON_AddItemToArray(a, cJSON_CreateNumber(roundf(m[i][j] * 10000.0f) / 10000.0f));
        }
    }
    return a;
}

static cJSON *u32_array(const uint32_t *v, int n)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
    }
    return a;
}

static cJSON *gamma_json(const esp_ipa_gamma_channel_t *ch)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *x = cJSON_AddArrayToObject(o, "x");
    cJSON *y = cJSON_AddArrayToObject(o, "y");
    for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
        cJSON_AddItemToArray(x, cJSON_CreateNumber(ch->x[i]));
        cJSON_AddItemToArray(y, cJSON_CreateNumber(ch->y[i]));
    }
    return o;
}

static cJSON *gamma_points_json(const esp_video_isp_gamma_point_t *pts)
{
    cJSON *y = cJSON_CreateArray();
    for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
        cJSON_AddItemToArray(y, cJSON_CreateNumber(pts[i].y));
    }
    return y;
}

static float round4(float v)
{
    return roundf(v * 10000.0f) / 10000.0f;
}

/* ------------------------------------------------------------------ */
/* Sensor registers                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint16_t addr;
    uint8_t bytes;
} reg_desc_t;

static const reg_desc_t s_imx708_regs[] = {
    {"orientation_0101", 0x0101, 1},
    {"exposure_lines_0202", 0x0202, 2},
    {"analog_gain_code_0204", 0x0204, 2},
    {"digital_gain_020e", 0x020e, 2},
    {"dgain_r_0210", 0x0210, 2},
    {"dgain_b_0212", 0x0212, 2},
    {"dgain_gb_0214", 0x0214, 2},
    {"frame_length_0340", 0x0340, 2},
    {"long_exp_shift_3100", 0x3100, 1},
    {"pedestal_0008", 0x0008, 2},
    {"data_format_0112", 0x0112, 2},
};

static const reg_desc_t s_ov5647_regs[] = {
    {"exposure_3500", 0x3500, 3},
    {"gain_350a", 0x350a, 2},
    {"aec_agc_manual_3503", 0x3503, 1},
    {"isp_ctrl_5000", 0x5000, 1},
    {"isp_ctrl_5001", 0x5001, 1},
    {"blc_target_4009", 0x4009, 1},
    {"awb_manual_3406", 0x3406, 1},
    {"awb_r_gain_3400", 0x3400, 2},
    {"awb_g_gain_3402", 0x3402, 2},
    {"awb_b_gain_3404", 0x3404, 2},
};

static esp_err_t sensor_reg_read(esp_cam_sensor_device_t *s, uint32_t addr, uint32_t *val)
{
    esp_cam_sensor_reg_val_t r = { .regaddr = addr };
    esp_err_t ret = esp_cam_sensor_ioctl(s, ESP_CAM_SENSOR_IOC_G_REG, &r);
    *val = r.value;
    return ret;
}

static cJSON *sensor_regs_json(esp_cam_sensor_device_t *s)
{
    const reg_desc_t *regs = NULL;
    size_t n = 0;
    if (!s || !s->name) {
        return cJSON_CreateNull();
    }
    if (strcmp(s->name, "IMX708") == 0) {
        regs = s_imx708_regs;
        n = sizeof(s_imx708_regs) / sizeof(s_imx708_regs[0]);
    } else if (strcmp(s->name, "OV5647") == 0) {
        regs = s_ov5647_regs;
        n = sizeof(s_ov5647_regs) / sizeof(s_ov5647_regs[0]);
    }
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; i < n; i++) {
        uint32_t v = 0;
        bool ok = true;
        for (int b = 0; b < regs[i].bytes && ok; b++) {
            uint32_t byte;
            ok = sensor_reg_read(s, regs[i].addr + b, &byte) == ESP_OK;
            v = (v << 8) | (byte & 0xff);
        }
        if (ok) {
            cJSON_AddNumberToObject(o, regs[i].name, v);
        } else {
            cJSON_AddNullToObject(o, regs[i].name);
        }
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* GET                                                                */
/* ------------------------------------------------------------------ */

static cJSON *build_state(void)
{
    static isp_hook_snapshot_t snap;    /* ~1.5 KB, keep it off the httpd stack */
    isp_hook_overrides_t ov;
    isp_hook_imx708_state_t imx;
    isp_hook_get_snapshot(&snap);
    isp_hook_get_overrides(&ov);
    isp_hook_get_imx708_state(&imx);

    int fd = s_ops.isp_fd ? s_ops.isp_fd(s_ops.ctx) : -1;
    esp_cam_sensor_device_t *sensor = s_ops.sensor ? s_ops.sensor(s_ops.ctx) : NULL;
    const char *sensor_name = sensor && sensor->name ? sensor->name : "";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "sensor", sensor_name);
    cJSON_AddStringToObject(root, "chip_isp", "ESP32-P4 rev<3: no BLC/WBG hardware; WB folded into CCM");

    /* Tuning */
    cJSON *tuning = cJSON_AddObjectToObject(root, "tuning");
    const esp_ipa_config_t *cfg = sensor_name[0] ? esp_video_isp_pipeline_enum_ipa_configs(sensor_name, 0) : NULL;
    if (strcmp(sensor_name, "IMX708") == 0) {
        cJSON_AddStringToObject(tuning, "json", ISP_TUNING_JSON_NAME);
    }
    cJSON_AddBoolToObject(tuning, "ipa_config_found", cfg != NULL);
    if (cfg) {
        cJSON_AddStringToObject(tuning, "description", cfg->description ? cfg->description : "");
        cJSON *algos = cJSON_AddArrayToObject(tuning, "algorithms");
        for (int i = 0; i < cfg->nums; i++) {
            cJSON_AddItemToArray(algos, cJSON_CreateString(cfg->names[i]));
        }
        if (cfg->agc) {
            const esp_ipa_agc_config_t *a = cfg->agc;
            cJSON *agc = cJSON_AddObjectToObject(tuning, "agc");
            cJSON_AddNumberToObject(agc, "luma_target", a->luma_target);
            cJSON_AddNumberToObject(agc, "luma_low", a->luma_low);
            cJSON_AddNumberToObject(agc, "luma_high", a->luma_high);
            cJSON_AddNumberToObject(agc, "meter_mode", a->meter_mode);
            cJSON_AddNumberToObject(agc, "anti_flicker_mode", a->anti_flicker_mode);
            cJSON_AddNumberToObject(agc, "ac_freq", a->ac_freq);
            cJSON_AddNumberToObject(agc, "max_gain", a->max_gain);
            cJSON_AddItemToObject(agc, "weights", cJSON_CreateArray());
            cJSON *w = cJSON_GetObjectItem(agc, "weights");
            for (int i = 0; i < ISP_AE_REGIONS; i++) {
                cJSON_AddItemToArray(w, cJSON_CreateNumber(a->luma_weight_table[i]));
            }
        }
    }

    cJSON_AddNumberToObject(root, "frames", snap.frames);
    cJSON_AddNumberToObject(root, "seq", (double)snap.seq);

    /* AE */
    cJSON *ae = cJSON_AddObjectToObject(root, "ae");
    esp_video_isp_pipeline_agc_status_t agc_status;
    if (esp_video_isp_pipeline_get_agc_status(&agc_status) == ESP_OK) {
        cJSON_AddBoolToObject(ae, "agc_enabled", agc_status == ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE);
    }
    cJSON_AddNumberToObject(ae, "exposure_us", snap.sensor.cur_exposure);
    cJSON_AddNumberToObject(ae, "exposure_min_us", snap.sensor.min_exposure);
    /* The AE ceiling in force (esp_ipa AGC), not the boot-time sensor range:
     * the firmware moves it with the frame length (frame-rate cap). */
    uint32_t agc_max = 0;
    if (esp_video_isp_pipeline_get_agc_max_exposure(&agc_max) == ESP_OK) {
        cJSON_AddNumberToObject(ae, "exposure_max_us", agc_max);
    } else {
        cJSON_AddNullToObject(ae, "exposure_max_us");
    }
    /* Longest exposure the ISP pipeline believes the sensor takes (kept in step with the frame length). */
    cJSON_AddNumberToObject(ae, "sensor_exposure_max_us", snap.sensor.max_exposure);
    if (sensor) {
        esp_cam_sensor_param_desc_t qd = { .id = ESP_CAM_SENSOR_EXPOSURE_VAL };
        esp_cam_sensor_format_t cur;
        if (esp_cam_sensor_query_para_desc(sensor, &qd) == ESP_OK && qd.type == ESP_CAM_SENSOR_PARAM_TYPE_NUMBER &&
                esp_cam_sensor_get_format(sensor, &cur) == ESP_OK && cur.isp_info) {
            cJSON_AddNumberToObject(ae, "frame_exposure_max_us",
                                    (double)qd.number.maximum * cur.isp_info->isp_v1_info.tline_ns / 1000.0);
        }
    }
    cJSON_AddNumberToObject(ae, "gain", round4(snap.sensor.cur_gain));
    cJSON_AddNumberToObject(ae, "gain_min", round4(snap.sensor.min_gain));
    cJSON_AddNumberToObject(ae, "gain_max", round4(snap.sensor.max_gain));
    cJSON_AddNumberToObject(ae, "ipa_exposure_us", snap.last.exposure);
    cJSON_AddNumberToObject(ae, "ipa_gain", round4(snap.last.gain));
    float f;
    if (esp_video_isp_pipeline_get_env_float("ae.luma.avg", &f) == ESP_OK) {
        cJSON_AddNumberToObject(ae, "env_ae_luma_avg", round4(f));
    }
    uint32_t sum = 0;
    for (int i = 0; i < ISP_AE_REGIONS; i++) {
        sum += snap.ae_luma[i];
    }
    cJSON_AddNumberToObject(ae, "block_luma_mean", round4((float)sum / ISP_AE_REGIONS));
    cJSON_AddItemToObject(ae, "block_luma", u32_array(snap.ae_luma, ISP_AE_REGIONS));
    cJSON_AddItemToObject(ae, "histogram", u32_array(snap.hist, ISP_HIST_SEGMENT_NUMS));

    /* AWB */
    cJSON *awb = cJSON_AddObjectToObject(root, "awb");
    cJSON_AddNumberToObject(awb, "ipa_red_gain", round4(snap.last.red_gain));
    cJSON_AddNumberToObject(awb, "ipa_blue_gain", round4(snap.last.blue_gain));
    esp_video_isp_wb_t wb = {0};
    if (isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_WB, &wb, NULL) == ESP_OK) {
        cJSON *w = cJSON_AddObjectToObject(awb, "isp");
        cJSON_AddBoolToObject(w, "enable", wb.enable);
        cJSON_AddNumberToObject(w, "red_gain", round4(wb.red_gain));
        cJSON_AddNumberToObject(w, "blue_gain", round4(wb.blue_gain));
    }
    const esp_ipa_stats_awb_t *as[2] = { &snap.awb, &snap.awb_raw };
    const char *as_name[2] = { "stats_ipa", "stats_isp" };
    for (int k = 0; k < 2; k++) {
        cJSON *s = cJSON_AddObjectToObject(awb, as_name[k]);
        cJSON_AddNumberToObject(s, "counted", as[k]->counted);
        cJSON_AddNumberToObject(s, "sum_r", as[k]->sum_r);
        cJSON_AddNumberToObject(s, "sum_g", as[k]->sum_g);
        cJSON_AddNumberToObject(s, "sum_b", as[k]->sum_b);
        if (as[k]->sum_g) {
            cJSON_AddNumberToObject(s, "r_over_g", round4((float)as[k]->sum_r / as[k]->sum_g));
            cJSON_AddNumberToObject(s, "b_over_g", round4((float)as[k]->sum_b / as[k]->sum_g));
        }
    }
    cJSON *rng = cJSON_AddObjectToObject(awb, "white_patch_range");
    cJSON_AddNumberToObject(rng, "green_min", snap.last.awb.green_min);
    cJSON_AddNumberToObject(rng, "green_max", snap.last.awb.green_max);
    cJSON_AddNumberToObject(rng, "rg_min", round4(snap.last.awb.rg_min));
    cJSON_AddNumberToObject(rng, "rg_max", round4(snap.last.awb.rg_max));
    cJSON_AddNumberToObject(rng, "bg_min", round4(snap.last.awb.bg_min));
    cJSON_AddNumberToObject(rng, "bg_max", round4(snap.last.awb.bg_max));
    if (esp_video_isp_pipeline_get_env_float("ct", &f) == ESP_OK) {
        cJSON_AddNumberToObject(awb, "env_ct", round4(f));
    } else {
        int32_t ct;
        if (esp_video_isp_pipeline_get_env_int32("ct", &ct) == ESP_OK) {
            cJSON_AddNumberToObject(awb, "env_ct", ct);
        }
    }

    /* CCM */
    cJSON *ccm = cJSON_AddObjectToObject(root, "ccm");
    cJSON_AddItemToObject(ccm, "ipa", matrix_json(snap.last.ccm.matrix));
    esp_video_isp_ccm_t iccm = {0};
    if (isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_CCM, &iccm, NULL) == ESP_OK) {
        cJSON_AddBoolToObject(ccm, "isp_enable", iccm.enable);
        cJSON_AddItemToObject(ccm, "isp", matrix_json(iccm.matrix));
    }
    cJSON_AddStringToObject(ccm, "note", "the ISP multiplies CCM columns 0/2 by the WB red/blue gains (no WBG block)");

    /* Gamma */
    cJSON *gamma = cJSON_AddObjectToObject(root, "gamma");
    cJSON_AddItemToObject(gamma, "ipa_green", gamma_json(&snap.last.gamma.green));
    static esp_video_isp_gamma_ext_t g;
    memset(&g, 0, sizeof(g));
    if (isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_GAMMA_EXT, &g, NULL) == ESP_OK) {
        cJSON_AddBoolToObject(gamma, "isp_enable", g.enable);
        cJSON_AddItemToObject(gamma, "isp_red_y", gamma_points_json(g.red_points));
        cJSON_AddItemToObject(gamma, "isp_green_y", gamma_points_json(g.green_points));
        cJSON_AddItemToObject(gamma, "isp_blue_y", gamma_points_json(g.blue_points));
        cJSON *gx = cJSON_AddArrayToObject(gamma, "isp_x_rgb");
        const esp_video_isp_gamma_point_t *gp[3] = { g.red_points, g.green_points, g.blue_points };
        for (int c = 0; c < 3; c++) {
            cJSON *xa = cJSON_CreateArray();
            for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
                cJSON_AddItemToArray(xa, cJSON_CreateNumber(gp[c][i].x));
            }
            cJSON_AddItemToArray(gx, xa);
        }
    }

    /* Colour */
    cJSON *color = cJSON_AddObjectToObject(root, "color");
    const struct {
        const char *n;
        uint32_t id;
    } cc[] = { {"brightness", V4L2_CID_BRIGHTNESS}, {"contrast", V4L2_CID_CONTRAST},
        {"saturation", V4L2_CID_SATURATION}, {"hue", V4L2_CID_HUE}
    };
    for (size_t i = 0; i < sizeof(cc) / sizeof(cc[0]); i++) {
        int32_t v = 0;
        if (isp_ctrl(fd, false, cc[i].id, NULL, &v) == ESP_OK) {
            cJSON_AddNumberToObject(color, cc[i].n, v);
        }
    }

    /* Modules */
    cJSON *mod = cJSON_AddObjectToObject(root, "modules");
    esp_video_isp_bf_t bf = {0};
    if (isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_BF, &bf, NULL) == ESP_OK) {
        cJSON *m = cJSON_AddObjectToObject(mod, "bf");
        cJSON_AddBoolToObject(m, "enable", bf.enable);
        cJSON_AddNumberToObject(m, "level", bf.level);
    }
    esp_video_isp_sharpen_t sh = {0};
    if (isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_SHARPEN, &sh, NULL) == ESP_OK) {
        cJSON *m = cJSON_AddObjectToObject(mod, "sharpen");
        cJSON_AddBoolToObject(m, "enable", sh.enable);
        cJSON_AddNumberToObject(m, "h_thresh", sh.h_thresh);
        cJSON_AddNumberToObject(m, "l_thresh", sh.l_thresh);
        cJSON_AddNumberToObject(m, "h_coeff", round4(sh.h_coeff));
        cJSON_AddNumberToObject(m, "m_coeff", round4(sh.m_coeff));
    }
    esp_video_isp_demosaic_t dm = {0};
    if (isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_DEMOSAIC, &dm, NULL) == ESP_OK) {
        cJSON *m = cJSON_AddObjectToObject(mod, "demosaic");
        cJSON_AddBoolToObject(m, "enable", dm.enable);
        cJSON_AddNumberToObject(m, "gradient_ratio", round4(dm.gradient_ratio));
    }
    cJSON_AddBoolToObject(mod, "gamma", g.enable);
    cJSON_AddBoolToObject(mod, "ccm", iccm.enable);
    esp_video_isp_lsc_t lsc = {0};
    if (isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_LSC, &lsc, NULL) == ESP_OK) {
        cJSON *m = cJSON_AddObjectToObject(mod, "lsc");
        cJSON_AddBoolToObject(m, "enable", lsc.enable);
        cJSON_AddNumberToObject(m, "grid_cells", lsc.lsc_gain_size);
        if (lsc.enable && lsc.gain_r && lsc.lsc_gain_size) {
            /* Report the centre cell of each channel. */
            uint32_t c = lsc.lsc_gain_size / 2;
            const isp_lsc_gain_t *t[4] = { lsc.gain_r, lsc.gain_gr, lsc.gain_gb, lsc.gain_b };
            cJSON *cg = cJSON_AddArrayToObject(m, "centre_gain_r_gr_gb_b");
            for (int k = 0; k < 4; k++) {
                cJSON_AddItemToArray(cg, cJSON_CreateNumber(round4(t[k][c].integer + t[k][c].decimal / 256.0f)));
            }
        }
    } else {
        cJSON_AddStringToObject(mod, "lsc", "unsupported");
    }
    cJSON_AddStringToObject(mod, "blc", "unsupported on this chip revision (P4 < v3.0)");

    /* IPA metadata bookkeeping */
    cJSON *md = cJSON_AddObjectToObject(root, "ipa_metadata");
    cJSON_AddNumberToObject(md, "seen_flags", snap.seen_flags);
    cJSON_AddNumberToObject(md, "init_flags", snap.init_flags);
    cJSON_AddNumberToObject(md, "last_frame_flags", snap.frame_flags);
    cJSON_AddNumberToObject(md, "applied_flags", snap.applied_flags);
    cJSON *upd = cJSON_AddObjectToObject(md, "updates");
    for (int i = 0; i < ISP_HOOK_METADATA_GROUPS; i++) {
        if (snap.updates[i]) {
            cJSON_AddNumberToObject(upd, s_group_names[i], snap.updates[i]);
        }
    }
    cJSON_AddNumberToObject(md, "contrast", snap.last.contrast);
    cJSON_AddNumberToObject(md, "saturation", snap.last.saturation);
    cJSON_AddNumberToObject(md, "bf_level", snap.last.bf.level);
    cJSON_AddNumberToObject(md, "demosaic_gradient_ratio", round4(snap.last.demosaic.gradient_ratio));

    /* IMX708 colour pipeline */
    cJSON *ix = cJSON_AddObjectToObject(root, "imx708_pipeline");
    cJSON_AddBoolToObject(ix, "enabled", imx.enabled);
    cJSON_AddBoolToObject(ix, "active", imx.active);
    if (imx.active) {
        cJSON *a3 = cJSON_AddArrayToObject(ix, "wb_gain_rgb");
        cJSON *o3 = cJSON_AddArrayToObject(ix, "black_offset_rgb");
        cJSON *k3 = cJSON_AddArrayToObject(ix, "stretch_rgb");
        for (int k = 0; k < 3; k++) {
            cJSON_AddItemToArray(a3, cJSON_CreateNumber(round4(imx.wb_gain[k])));
            cJSON_AddItemToArray(o3, cJSON_CreateNumber(round4(imx.offset[k])));
            cJSON_AddItemToArray(k3, cJSON_CreateNumber(round4(imx.stretch[k])));
        }
        cJSON *lg = cJSON_AddArrayToObject(ix, "lsc_gain_r_gr_gb_b");
        for (int k = 0; k < 4; k++) {
            cJSON_AddItemToArray(lg, cJSON_CreateNumber(round4(imx.lsc_gain[k])));
        }
        cJSON_AddNumberToObject(ix, "pedestal_8bit", imx.pedestal);
        cJSON_AddNumberToObject(ix, "ccm_scale", round4(imx.ccm_scale));
        cJSON_AddItemToObject(ix, "ccm_isp", matrix_json(imx.ccm_isp));
    }

    /* Sensor registers */
    cJSON_AddItemToObject(root, "sensor_regs", sensor_regs_json(sensor));

    /* Overrides */
    cJSON *o = cJSON_AddObjectToObject(root, "overrides");
    if (ov.awb_manual) {
        cJSON *a = cJSON_AddObjectToObject(o, "awb");
        cJSON_AddNumberToObject(a, "red", ov.red_gain);
        cJSON_AddNumberToObject(a, "blue", ov.blue_gain);
    }
    if (ov.ccm_manual) {
        cJSON_AddItemToObject(o, "ccm", matrix_json(ov.ccm));
    }
    if (ov.gamma_manual) {
        cJSON *a = cJSON_AddObjectToObject(o, "gamma");
        cJSON_AddNumberToObject(a, "gamma", ov.gamma);
        cJSON *b = cJSON_AddArrayToObject(a, "black");
        for (int k = 0; k < 3; k++) {
            cJSON_AddItemToArray(b, cJSON_CreateNumber(ov.black[k]));
        }
    }
    if (ov.lsc_manual) {
        cJSON *a = cJSON_AddArrayToObject(o, "lsc");
        for (int k = 0; k < 4; k++) {
            cJSON_AddItemToArray(a, cJSON_CreateNumber(ov.lsc_gain[k]));
        }
    }
    if (ov.ae_luma_scale != 1.0f) {
        cJSON_AddNumberToObject(o, "ae_luma_scale", ov.ae_luma_scale);
    }
    if (ov.hold_flags) {
        cJSON_AddNumberToObject(o, "hold_flags", ov.hold_flags);
    }
    return root;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(out, ESP_ERR_NO_MEM, TAG, "no memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr(req, out);
    free(out);
    return ret;
}

static esp_err_t debug_isp_get_handler(httpd_req_t *req)
{
    return send_json(req, build_state());
}

/* ------------------------------------------------------------------ */
/* POST                                                               */
/* ------------------------------------------------------------------ */

static bool is_auto(const cJSON *j)
{
    return cJSON_IsString(j) && strcmp(j->valuestring, "auto") == 0;
}

static uint32_t module_flags(const char *name)
{
    if (strcmp(name, "bf") == 0) {
        return IPA_METADATA_FLAGS_BF;
    } else if (strcmp(name, "sharpen") == 0) {
        return IPA_METADATA_FLAGS_SH;
    } else if (strcmp(name, "demosaic") == 0) {
        return IPA_METADATA_FLAGS_DM;
    } else if (strcmp(name, "gamma") == 0) {
        return IPA_METADATA_FLAGS_GAMMA;
    } else if (strcmp(name, "ccm") == 0) {
        return IPA_METADATA_FLAGS_CCM;
    } else if (strcmp(name, "lsc") == 0) {
        return IPA_METADATA_FLAGS_LSC;
    } else if (strcmp(name, "color") == 0) {
        return IPA_METADATA_FLAGS_BR | IPA_METADATA_FLAGS_CN | IPA_METADATA_FLAGS_ST | IPA_METADATA_FLAGS_HUE;
    }
    return 0;
}

static esp_err_t set_module_enable(int fd, const char *name, bool enable, uint32_t *hold)
{
    /* Only disabling is done directly; enabling is handing the module back to the IPA. */
    if (enable) {
        return ESP_OK;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (strcmp(name, "bf") == 0) {
        esp_video_isp_bf_t v = {0};
        isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_BF, &v, NULL);
        v.enable = false;
        ret = isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_BF, &v, NULL);
        *hold |= IPA_METADATA_FLAGS_BF;
    } else if (strcmp(name, "sharpen") == 0) {
        esp_video_isp_sharpen_t v = {0};
        isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_SHARPEN, &v, NULL);
        v.enable = false;
        ret = isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_SHARPEN, &v, NULL);
        *hold |= IPA_METADATA_FLAGS_SH;
    } else if (strcmp(name, "demosaic") == 0) {
        esp_video_isp_demosaic_t v = {0};
        isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_DEMOSAIC, &v, NULL);
        v.enable = false;
        ret = isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_DEMOSAIC, &v, NULL);
        *hold |= IPA_METADATA_FLAGS_DM;
    } else if (strcmp(name, "gamma") == 0) {
        static esp_video_isp_gamma_ext_t v;
        memset(&v, 0, sizeof(v));
        isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_GAMMA_EXT, &v, NULL);
        v.enable = false;
        ret = isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_GAMMA_EXT, &v, NULL);
        *hold |= IPA_METADATA_FLAGS_GAMMA;
    } else if (strcmp(name, "ccm") == 0) {
        esp_video_isp_ccm_t v = {0};
        isp_ctrl(fd, false, V4L2_CID_USER_ESP_ISP_CCM, &v, NULL);
        v.enable = false;
        ret = isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_CCM, &v, NULL);
        *hold |= IPA_METADATA_FLAGS_CCM;
    } else if (strcmp(name, "lsc") == 0) {
        esp_video_isp_lsc_t v = { .enable = false };
        ret = isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_LSC, &v, NULL);
        *hold |= IPA_METADATA_FLAGS_LSC;
        s_lsc_forced = true;
    } else if (strcmp(name, "color") == 0) {
        *hold |= IPA_METADATA_FLAGS_BR | IPA_METADATA_FLAGS_CN | IPA_METADATA_FLAGS_ST | IPA_METADATA_FLAGS_HUE;
        int32_t v = 0;
        isp_ctrl(fd, true, V4L2_CID_BRIGHTNESS, NULL, &v);
        v = 128;
        isp_ctrl(fd, true, V4L2_CID_CONTRAST, NULL, &v);
        isp_ctrl(fd, true, V4L2_CID_SATURATION, NULL, &v);
        v = 0;
        ret = isp_ctrl(fd, true, V4L2_CID_HUE, NULL, &v);
    }
    return ret;
}

static esp_err_t debug_isp_post_handler(httpd_req_t *req)
{
    char *content = NULL;
    cJSON *root = NULL;
    const char *error = NULL;
    static char errbuf[96];

    if (req->content_len <= 0 || req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body must be 1..4096 bytes of JSON");
        return ESP_FAIL;
    }
    content = calloc(1, req->content_len + 1);
    ESP_RETURN_ON_FALSE(content, ESP_ERR_NO_MEM, TAG, "no memory");
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, content + got, req->content_len - got);
        if (r <= 0) {
            free(content);
            return ESP_FAIL;
        }
        got += r;
    }
    root = cJSON_Parse(content);
    free(content);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    int fd = s_ops.isp_fd ? s_ops.isp_fd(s_ops.ctx) : -1;
    esp_cam_sensor_device_t *sensor = s_ops.sensor ? s_ops.sensor(s_ops.ctx) : NULL;
    isp_hook_overrides_t ov;
    cJSON *j;

    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "reset"))) {
        if (s_lsc_forced) {
            esp_video_isp_lsc_t v = { .enable = false };
            isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_LSC, &v, NULL);
            s_lsc_forced = false;
        }
        isp_hook_reset();
        esp_video_isp_pipeline_set_agc_status(ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE);
    }
    isp_hook_get_overrides(&ov);

    j = cJSON_GetObjectItem(root, "awb");
    if (j) {
        if (is_auto(j)) {
            ov.awb_manual = false;
            isp_hook_restore_groups(IPA_METADATA_FLAGS_RG | IPA_METADATA_FLAGS_BG);
        } else if (cJSON_IsObject(j)) {
            cJSON *r = cJSON_GetObjectItem(j, "red"), *b = cJSON_GetObjectItem(j, "blue");
            if (!cJSON_IsNumber(r) || !cJSON_IsNumber(b) || r->valuedouble <= 0 || b->valuedouble <= 0 ||
                    r->valuedouble > 8 || b->valuedouble > 8) {
                error = "awb needs {\"red\":0..8,\"blue\":0..8} or \"auto\"";
                goto done;
            }
            ov.awb_manual = true;
            ov.red_gain = r->valuedouble;
            ov.blue_gain = b->valuedouble;
        }
    }

    j = cJSON_GetObjectItem(root, "ccm");
    if (j) {
        if (is_auto(j)) {
            ov.ccm_manual = false;
            isp_hook_restore_groups(IPA_METADATA_FLAGS_CCM);
        } else if (cJSON_IsString(j) && strcmp(j->valuestring, "identity") == 0) {
            ov.ccm_manual = true;
            memset(ov.ccm, 0, sizeof(ov.ccm));
            for (int i = 0; i < 3; i++) {
                ov.ccm[i][i] = 1.0f;
            }
        } else if (cJSON_IsArray(j) && cJSON_GetArraySize(j) == 9) {
            for (int i = 0; i < 9; i++) {
                cJSON *e = cJSON_GetArrayItem(j, i);
                if (!cJSON_IsNumber(e) || fabs(e->valuedouble) > 4) {
                    error = "ccm entries must be numbers in -4..4";
                    goto done;
                }
                ov.ccm[i / 3][i % 3] = e->valuedouble;
            }
            ov.ccm_manual = true;
        } else {
            error = "ccm must be \"auto\", \"identity\" or 9 numbers (row-major)";
            goto done;
        }
    }

    j = cJSON_GetObjectItem(root, "gamma");
    if (j) {
        if (is_auto(j)) {
            ov.gamma_manual = false;
            isp_hook_restore_groups(IPA_METADATA_FLAGS_GAMMA);
        } else if (cJSON_IsObject(j)) {
            cJSON *gv = cJSON_GetObjectItem(j, "gamma");
            cJSON *cv = cJSON_GetObjectItem(j, "curve");
            ov.tone_curve = 0;
            if (cJSON_IsString(cv)) {
                ov.tone_curve = strcmp(cv->valuestring, "srgb") == 0 ? 1 : (strcmp(cv->valuestring, "bt709") == 0 ? 2 : 0);
            }
            cJSON *bl = cJSON_GetObjectItem(j, "black");
            ov.gamma = cJSON_IsNumber(gv) ? gv->valuedouble : 1.0;
            if (ov.gamma < 0.2f || ov.gamma > 3.0f) {
                error = "gamma must be 0.2..3";
                goto done;
            }
            memset(ov.black, 0, sizeof(ov.black));
            if (cJSON_IsNumber(bl)) {
                ov.black[0] = ov.black[1] = ov.black[2] = (uint8_t)MIN(MAX(bl->valueint, 0), 128);
            } else if (cJSON_IsArray(bl) && cJSON_GetArraySize(bl) == 3) {
                for (int k = 0; k < 3; k++) {
                    ov.black[k] = (uint8_t)MIN(MAX(cJSON_GetArrayItem(bl, k)->valueint, 0), 128);
                }
            }
            ov.gamma_manual = true;
        }
    }

    j = cJSON_GetObjectItem(root, "lsc");
    if (j) {
        if (is_auto(j)) {
            ov.lsc_manual = false;
            if (s_lsc_forced) {
                esp_video_isp_lsc_t v = { .enable = false };
                isp_ctrl(fd, true, V4L2_CID_USER_ESP_ISP_LSC, &v, NULL);
                s_lsc_forced = false;
            }
        } else if (cJSON_IsObject(j)) {
            const char *k[4] = { "r", "gr", "gb", "b" };
            for (int i = 0; i < 4; i++) {
                cJSON *e = cJSON_GetObjectItem(j, k[i]);
                ov.lsc_gain[i] = cJSON_IsNumber(e) ? e->valuedouble : 1.0;
                if (ov.lsc_gain[i] < 0 || ov.lsc_gain[i] > 3.99f) {
                    error = "lsc gains must be 0..3.99";
                    goto done;
                }
            }
            ov.lsc_manual = true;
            s_lsc_forced = true;
        }
    }

    j = cJSON_GetObjectItem(root, "imx708_pedestal");
    if (cJSON_IsNumber(j)) {
        isp_hook_set_imx708_pedestal((float)j->valuedouble);
    }

    j = cJSON_GetObjectItem(root, "imx708_pipeline");
    if (cJSON_IsBool(j)) {
        isp_hook_set_imx708_pipeline(cJSON_IsTrue(j));
    }

    j = cJSON_GetObjectItem(root, "ae_luma_scale");
    if (cJSON_IsNumber(j)) {
        if (j->valuedouble < 0.1 || j->valuedouble > 10) {
            error = "ae_luma_scale must be 0.1..10";
            goto done;
        }
        ov.ae_luma_scale = j->valuedouble;
    }

    j = cJSON_GetObjectItem(root, "modules");
    if (cJSON_IsObject(j)) {
        cJSON *m;
        cJSON_ArrayForEach(m, j) {
            if (!cJSON_IsBool(m)) {
                continue;
            }
            uint32_t mask = 0;
            if (cJSON_IsTrue(m)) {
                /* Release: stop dropping the IPA's updates and re-push its latest values. */
                mask = module_flags(m->string);
                ov.hold_flags &= ~mask;
                isp_hook_restore_groups(mask);
                if (strcmp(m->string, "lsc") == 0) {
                    s_lsc_forced = false;
                }
                continue;
            }
            if (set_module_enable(fd, m->string, false, &mask) != ESP_OK) {
                snprintf(errbuf, sizeof(errbuf), "failed to disable module %s", m->string);
                error = errbuf;
                goto done;
            }
            ov.hold_flags |= mask;
        }
    }

    /* AE: manual exposure (sensor lines) and gain (menu index), or auto. */
    j = cJSON_GetObjectItem(root, "ae");
    if (j) {
        if (is_auto(j)) {
            esp_video_isp_pipeline_set_agc_status(ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE);
        } else if (cJSON_IsObject(j) && sensor) {
            esp_video_isp_pipeline_set_agc_status(ESP_VIDEO_ISP_PIPELINE_AGC_DISABLE);
            cJSON *e = cJSON_GetObjectItem(j, "exposure_lines");
            cJSON *g = cJSON_GetObjectItem(j, "gain_index");
            if (cJSON_IsNumber(e)) {
                uint32_t v = e->valueint;
                if (esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_EXPOSURE_VAL, &v, sizeof(v)) != ESP_OK) {
                    error = "failed to set exposure";
                    goto done;
                }
            }
            if (cJSON_IsNumber(g)) {
                uint32_t v = g->valueint;
                if (esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_GAIN, &v, sizeof(v)) != ESP_OK) {
                    error = "failed to set gain";
                    goto done;
                }
            }
        }
    }

#if CONFIG_CAMERA_IMX708
    /* IMX708 test pattern: 0 off, 1 colour bars, 2 solid (levels in test_colour, 12-bit R, Gr, B, Gb). */
    j = cJSON_GetObjectItem(root, "test_colour");
    if (cJSON_IsArray(j) && cJSON_GetArraySize(j) == 4 && sensor && sensor->name && strcmp(sensor->name, "IMX708") == 0) {
        static const uint32_t ids[4] = { IMX708_CID_TEST_PATTERN_RED, IMX708_CID_TEST_PATTERN_GREENR,
                                         IMX708_CID_TEST_PATTERN_BLUE, IMX708_CID_TEST_PATTERN_GREENB
                                       };
        for (int k = 0; k < 4; k++) {
            uint32_t v = (uint32_t)cJSON_GetArrayItem(j, k)->valueint;
            if (esp_cam_sensor_set_para_value(sensor, ids[k], &v, sizeof(v)) != ESP_OK) {
                error = "failed to set test colour";
                goto done;
            }
        }
    }
    j = cJSON_GetObjectItem(root, "test_pattern");
    if (cJSON_IsNumber(j) && sensor) {
        int v = j->valueint;
        if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_TEST_PATTERN, &v) != ESP_OK) {
            error = "failed to set test pattern";
            goto done;
        }
    }
#endif

    /* Raw sensor register writes: [{"addr":n,"value":n}, ...] (8-bit registers). */
    j = cJSON_GetObjectItem(root, "sensor_regs");
    if (cJSON_IsArray(j) && sensor) {
        cJSON *w;
        cJSON_ArrayForEach(w, j) {
            cJSON *a = cJSON_GetObjectItem(w, "addr"), *v = cJSON_GetObjectItem(w, "value");
            if (!cJSON_IsNumber(a) || !cJSON_IsNumber(v)) {
                error = "sensor_regs entries need numeric addr and value";
                goto done;
            }
            esp_cam_sensor_reg_val_t r = { .regaddr = (uint32_t)a->valueint, .value = (uint32_t)v->valueint & 0xff };
            if (esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_REG, &r) != ESP_OK) {
                error = "sensor register write failed";
                goto done;
            }
            ESP_LOGI(TAG, "sensor reg 0x%04" PRIx32 " <- 0x%02" PRIx32, r.regaddr, r.value);
        }
    }

    isp_hook_set_overrides(&ov);

done:
    if (error) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        return ESP_FAIL;
    }

    /* Optional register reads: "read_regs": [addr, ...] */
    cJSON *state = build_state();
    j = cJSON_GetObjectItem(root, "read_regs");
    if (cJSON_IsArray(j) && sensor) {
        cJSON *rr = cJSON_AddObjectToObject(state, "read_regs");
        cJSON *a;
        cJSON_ArrayForEach(a, j) {
            if (!cJSON_IsNumber(a)) {
                continue;
            }
            uint32_t v;
            char key[12];
            snprintf(key, sizeof(key), "0x%04x", a->valueint);
            if (sensor_reg_read(sensor, a->valueint, &v) == ESP_OK) {
                cJSON_AddNumberToObject(rr, key, v & 0xff);
            } else {
                cJSON_AddNullToObject(rr, key);
            }
        }
    }
    cJSON_Delete(root);
    return send_json(req, state);
}

/* ------------------------------------------------------------------ */
/* RAW Bayer                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t debug_raw_handler(httpd_req_t *req)
{
    char q[48] = {0};
    int skip = 4;
    int bits = 0;   /* 0 = RAW10 (ISP bypassed), falling back to RAW8 */
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(q, "skip", v, sizeof(v)) == ESP_OK) {
            skip = atoi(v);
            skip = skip < 0 ? 0 : (skip > 30 ? 30 : skip);
        }
        if (httpd_query_key_value(q, "bits", v, sizeof(v)) == ESP_OK) {
            bits = atoi(v);
        }
    }
    if (!s_ops.raw_capture) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "RAW capture not available");
        return ESP_FAIL;
    }
    uint8_t *data = NULL;
    size_t len = 0;
    uint32_t w = 0, h = 0, fmt = bits == 8 ? V4L2_PIX_FMT_SBGGR8 : V4L2_PIX_FMT_SBGGR10;
    esp_err_t ret = s_ops.raw_capture(s_ops.ctx, skip, &data, &len, &w, &h, &fmt);
    if (ret == ESP_ERR_TIMEOUT && bits == 0) {
        /*
         * RAW10 with the ISP bypassed delivers no frames on this chip/IDF
         * combination; RAW8 keeps the ISP running but only its RAW-domain
         * blocks (LSC, Bayer denoise) touch the data - no demosaic, WB, CCM
         * or gamma - and it is the sensor's top 8 bits.
         */
        fmt = V4L2_PIX_FMT_SBGGR8;
        ret = s_ops.raw_capture(s_ops.ctx, skip, &data, &len, &w, &h, &fmt);
    }
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "RAW capture failed (see log)");
        return ret;
    }
    char hdr[16];
    httpd_resp_set_type(req, "application/octet-stream");
    snprintf(hdr, sizeof(hdr), "%" PRIu32, w);
    httpd_resp_set_hdr(req, "X-Width", hdr);
    char hdr2[16];
    snprintf(hdr2, sizeof(hdr2), "%" PRIu32, h);
    httpd_resp_set_hdr(req, "X-Height", hdr2);
    char hdr3[8];
    memcpy(hdr3, &fmt, 4);
    hdr3[4] = 0;
    httpd_resp_set_hdr(req, "X-Pixel-Format", hdr3);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)data, len);
    heap_caps_free(data);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Stream pipeline timing and network ceiling                         */
/* ------------------------------------------------------------------ */

static esp_err_t debug_stream_stats_handler(httpd_req_t *req)
{
    cJSON *root = stream_stats_json();
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr(req, out);
    free(out);
    return ret;
}

/*
 * GET /api/debug/tcp_throughput?mb=20&chunk=32768&mem=psram|internal&nodelay=1
 *
 * Raw TCP ceiling: a Content-Length response of `mb` MB of a constant pattern,
 * written with plain send() calls of `chunk` bytes straight onto the socket -
 * the same path the MJPEG stream uses, minus the camera. Read it with any
 * HTTP client and divide by the time; the device's own figure is logged.
 */
static esp_err_t debug_tcp_throughput_handler(httpd_req_t *req)
{
    char q[96] = {0};
    char v[16];
    uint32_t mb = 20, chunk = 32768;
    bool psram = true, nodelay = true;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "mb", v, sizeof(v)) == ESP_OK) {
            mb = (uint32_t)atoi(v);
        }
        if (httpd_query_key_value(q, "chunk", v, sizeof(v)) == ESP_OK) {
            chunk = (uint32_t)atoi(v);
        }
        if (httpd_query_key_value(q, "mem", v, sizeof(v)) == ESP_OK) {
            psram = strcmp(v, "internal") != 0;
        }
        if (httpd_query_key_value(q, "nodelay", v, sizeof(v)) == ESP_OK) {
            nodelay = atoi(v) != 0;
        }
    }
    mb = mb < 1 ? 1 : (mb > 200 ? 200 : mb);
    chunk = chunk < 1024 ? 1024 : (chunk > 262144 ? 262144 : chunk);
    if (!psram && chunk > 32768) {
        chunk = 32768;
    }
    uint8_t *buf = heap_caps_malloc(chunk, psram ? MALLOC_CAP_SPIRAM : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    memset(buf, 0x5a, chunk);
    int fd = httpd_req_to_sockfd(req);
    int one = nodelay ? 1 : 0;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    uint64_t total = (uint64_t)mb * 1000000ULL;
    char hdr[160];
    int hlen = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                        "Content-Length: %llu\r\nConnection: close\r\n\r\n", (unsigned long long)total);
    esp_err_t ret = ESP_OK;
    int64_t t0 = esp_timer_get_time();
    if (send(fd, hdr, hlen, 0) != hlen) {
        ret = ESP_FAIL;
    }
    uint64_t left = total;
    while (ret == ESP_OK && left) {
        size_t n = left < chunk ? (size_t)left : chunk;
        size_t off = 0;
        while (off < n) {
            int w = send(fd, buf + off, n - off, 0);
            if (w <= 0) {
                ret = ESP_FAIL;
                break;
            }
            off += w;
        }
        left -= n;
    }
    int64_t us = esp_timer_get_time() - t0;
    heap_caps_free(buf);
    ESP_LOGI(TAG, "tcp_throughput: %llu bytes from %s in %" PRIi64 " ms (chunk %" PRIu32 ", nodelay %d): %.2f MB/s%s",
             (unsigned long long)(total - left), psram ? "PSRAM" : "internal RAM", us / 1000, chunk, nodelay,
             us > 0 ? (total - left) / (double)us : 0, ret == ESP_OK ? "" : " (client went away)");
    /* The response was written raw; make httpd drop the connection. */
    httpd_sess_trigger_close(req->handle, fd);
    return ESP_OK;
}

esp_err_t isp_debug_api_register(httpd_handle_t server, const isp_debug_api_ops_t *ops)
{
    s_ops = *ops;
    const httpd_uri_t uris[ISP_DEBUG_API_URI_COUNT] = {
        { .uri = "/api/debug/isp", .method = HTTP_GET, .handler = debug_isp_get_handler },
        { .uri = "/api/debug/isp", .method = HTTP_POST, .handler = debug_isp_post_handler },
        { .uri = "/api/debug/raw_bayer", .method = HTTP_GET, .handler = debug_raw_handler },
        { .uri = "/api/debug/stream_stats", .method = HTTP_GET, .handler = debug_stream_stats_handler },
        { .uri = "/api/debug/tcp_throughput", .method = HTTP_GET, .handler = debug_tcp_throughput_handler },
    };
    for (int i = 0; i < ISP_DEBUG_API_URI_COUNT; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "failed to register %s", uris[i].uri);
    }
    ESP_LOGI(TAG, "ISP debug API enabled: /api/debug/isp, /api/debug/raw_bayer, /api/debug/stream_stats, "
             "/api/debug/tcp_throughput");
    return ESP_OK;
}

#else

esp_err_t isp_debug_api_register(httpd_handle_t server, const isp_debug_api_ops_t *ops)
{
    (void)server;
    (void)ops;
    return ESP_OK;
}

#endif /* CONFIG_EXAMPLE_ENABLE_ISP_DEBUG_API */
