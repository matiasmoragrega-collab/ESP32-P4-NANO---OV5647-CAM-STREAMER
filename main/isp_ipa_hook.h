/*
 * Hook between esp_video's ISP pipeline controller and the esp_ipa image
 * process algorithms (AE/AWB/CCM/gamma/...).
 *
 * esp_video calls esp_ipa_pipeline_process() once per ISP statistics frame and
 * applies the metadata it returns to the ISP and the sensor. The project links
 * with -Wl,--wrap=esp_ipa_pipeline_process (main/CMakeLists.txt), so every
 * call passes through here first. That gives us:
 *
 *  - a snapshot of what the IPA saw and decided (statistics, sensor state, the
 *    latest value of every metadata group), for the debug API;
 *  - debug overrides (manual WB, CCM, gamma, AE luma scale, held modules);
 *  - the IMX708 colour pipeline corrections (isp_ipa_hook.c, "IMX708 colour
 *    pipeline"), which need to sit exactly between the IPA and the ISP.
 *
 * Nothing here runs for a sensor until isp_hook_set_sensor() names it, and the
 * IMX708 corrections are only active for the IMX708.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ipa_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ISP_HOOK_METADATA_GROUPS    20  /* IPA_METADATA_FLAGS_* bits 0..19 */

typedef struct {
    uint32_t frames;                    /*!< IPA process calls since boot */
    uint64_t seq;                       /*!< ISP statistics sequence of the last call */
    uint32_t stats_flags;               /*!< IPA_STATS_FLAGS_* of the last call */
    uint32_t ae_luma[ISP_AE_REGIONS];   /*!< 5x5 AE block luminance (0..255), as the IPA saw it */
    uint32_t hist[ISP_HIST_SEGMENT_NUMS];
    esp_ipa_stats_awb_t awb;            /*!< AWB white-patch sums, as the IPA saw them */
    esp_ipa_stats_awb_t awb_raw;        /*!< AWB white-patch sums as the ISP measured them */
    uint8_t sharpen_max;
    esp_ipa_sensor_t sensor;            /*!< sensor state handed to the IPA (focus_info cleared) */
    esp_ipa_metadata_t last;            /*!< latest value of every metadata group the IPA produced */
    uint32_t seen_flags;                /*!< groups the IPA produced at least once */
    uint32_t frame_flags;               /*!< groups the IPA produced on the last call */
    uint32_t init_flags;                /*!< groups the IPA produced at pipeline init */
    uint32_t updates[ISP_HOOK_METADATA_GROUPS]; /*!< per-group update counters */
    uint32_t applied_flags;             /*!< groups actually passed on to the ISP on the last call */
} isp_hook_snapshot_t;

typedef struct {
    bool awb_manual;                    /*!< replace the IPA's WB gains */
    float red_gain;
    float blue_gain;

    bool ccm_manual;                    /*!< replace the IPA's CCM */
    float ccm[3][3];

    bool gamma_manual;                  /*!< replace the IPA's gamma curve */
    float gamma;                        /*!< y = ((x - black) / (255 - black)) ^ gamma; 1.0 = linear */
    uint8_t black[3];                   /*!< per-channel (R, G, B) offset removed by the curve, 8-bit units */
    int tone_curve;                     /*!< 0: power curve above; 1: sRGB; 2: BT.709 (log-spaced points) */

    bool lsc_manual;                    /*!< program flat LSC gains (per Bayer channel) */
    float lsc_gain[4];                  /*!< R, Gr, Gb, B */

    float ae_luma_scale;                /*!< AE statistics are multiplied by this before the IPA sees them (1.0 = off) */

    uint32_t hold_flags;                /*!< IPA_METADATA_FLAGS_* groups whose IPA updates are dropped */
} isp_hook_overrides_t;

/* Apply Kconfig settings to the generated IPA tuning (anti-flicker). Call before esp_video init. */
void isp_hook_prepare_ipa_configs(void);

/* Tell the hook which sensor the pipeline belongs to (e.g. "IMX708"). */
void isp_hook_set_sensor(const char *sensor_name);
const char *isp_hook_sensor(void);

void isp_hook_get_snapshot(isp_hook_snapshot_t *out);
uint32_t isp_hook_frame_count(void);
void isp_hook_get_overrides(isp_hook_overrides_t *out);

/* Install new overrides; they are applied on the next ISP frame. */
void isp_hook_set_overrides(const isp_hook_overrides_t *ov);

/*
 * Drop every override and push the IPA's latest value of every group back to
 * the ISP on the next frame (so a module held or replaced by a debug override
 * returns to what the tuning wants without waiting for the IPA to change its
 * mind).
 */
void isp_hook_reset(void);

/* Push the IPA's latest value of these IPA_METADATA_FLAGS_* groups on the next frame. */
void isp_hook_restore_groups(uint32_t flags);

/* Default (no-op) overrides. */
void isp_hook_default_overrides(isp_hook_overrides_t *ov);

/*
 * Feed the IPA the mean of the last @p frames frames' AE/AWB/histogram
 * statistics (1 = off, max 4). Used by the IMX708 full-view tile scan, whose
 * consecutive frames show different quarters of the scene.
 */
void isp_hook_set_stats_average(int frames);

/*
 * Update the longest exposure (us) esp_video's ISP pipeline believes the
 * sensor supports. esp_video reads it once at pipeline creation and rejects
 * AE ceilings above it, so a longer frame (lower frame-rate cap) needs this
 * before esp_video_isp_pipeline_set_agc_max_exposure(). Returns false until
 * the pipeline has processed its first frame.
 */
bool isp_hook_set_sensor_max_exposure(uint32_t max_us);
uint32_t isp_hook_sensor_max_exposure(void);

/* IMX708 colour pipeline state, for the debug API. */
typedef struct {
    bool enabled;                       /*!< pipeline switched on (Kconfig default, debug toggle) */
    bool active;                        /*!< ... and running: IMX708 detected, no manual LSC override */
    float wb_gain[3];                   /*!< WB gains (R, G, B) requested by the IPA (or a debug override) */
    float lsc_gain[4];                  /*!< R, Gr, Gb, B gains programmed in the LSC (= WB, pre-demosaic, clipped) */
    float pedestal;                     /*!< sensor black level in the ISP's 8-bit domain */
    float offset[3];                    /*!< per-channel black offset after the CCM, removed in the gamma LUT */
    float stretch[3];                   /*!< per-channel gain that maps each channel's clip point back to 255 */
    float ccm_scale;                    /*!< common gain folded into the CCM so saturated pixels stay white */
    float ccm_isp[3][3];                /*!< CCM actually programmed into the ISP */
} isp_hook_imx708_state_t;

/* Sensor black level (8-bit ISP units) the IMX708 pipeline removes. */
void isp_hook_set_imx708_pedestal(float pedestal_8bit);

/* Switch the IMX708 colour pipeline on/off at run time (A/B testing). */
void isp_hook_set_imx708_pipeline(bool enable);

void isp_hook_get_imx708_state(isp_hook_imx708_state_t *out);

#ifdef __cplusplus
}
#endif
