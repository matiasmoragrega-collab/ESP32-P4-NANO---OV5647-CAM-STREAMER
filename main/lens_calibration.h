/*
 * Lens-centred OV5647 readout windows and persistent lens calibration.
 *
 * Sensor coordinate system
 * ------------------------
 * "Sensor coordinates" are OV5647 pixel-array addresses in full-resolution
 * pixels -- the address space used by the array-window registers
 * 0x3800-0x3807.  Coordinates are continuous with the *edge* convention:
 * array column i covers [i, i + 1), so its centre is at i + 0.5.  The same
 * convention is used for output-image pixels.
 *
 *   whole array : x 0..2623, y 0..1955  (2624 x 1956, incl. dummy/dark border)
 *   active area : x 16..2607, y 6..1949 (2592 x 1944 imaging pixels)
 *
 * The default lens centre is the centre of the active area: (1312.0, 978.0).
 *
 * Window model (see docs/lens-calibration.md)
 * --------------------------------------------
 * For every mode the firmware reads out an output region of
 * (out_w * scale) x (out_h * scale) sensor pixels whose top-left corner
 * (window.x, window.y) is placed so that the region's centre lands on the
 * lens centre (rounded to an even address to keep the Bayer phase, then
 * clamped to the active area).  The array window programmed into
 * 0x3800-0x3807 is that region grown by a symmetric ISP margin of
 * LENS_ISP_MARGIN_X/Y output pixels per side, and 0x3810-0x3813 skip exactly
 * that margin, so the mapping does not depend on which side the sensor
 * counts the ISP offset from when mirroring.
 *
 * Output pixel edge coordinates (u, v) map to sensor coordinates as
 *   X = mirror ? window.x + window.width  - u * scale : window.x + u * scale
 *   Y = flip   ? window.y + window.height - v * scale : window.y + v * scale
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_cam_sensor.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LENS_SENSOR_ARRAY_WIDTH    2624
#define LENS_SENSOR_ARRAY_HEIGHT   1956
#define LENS_SENSOR_ACTIVE_X       16
#define LENS_SENSOR_ACTIVE_Y       6
#define LENS_SENSOR_ACTIVE_WIDTH   2592
#define LENS_SENSOR_ACTIVE_HEIGHT  1944

typedef struct {
    double fx, fy, k1, k2, p1, p2, k3;
} lens_distortion_t;

typedef struct {
    double cx, cy;              /*!< lens centre, sensor coordinates */
    bool calibrated;            /*!< false: defaults (source "default") */
    bool has_distortion;
    lens_distortion_t distortion;
} lens_calibration_t;

typedef struct {
    bool valid;
    int x, y;                   /*!< top-left of the output region (sensor coords) */
    int width, height;          /*!< output region size in sensor pixels */
    int scale_x, scale_y;       /*!< sensor pixels per output pixel */
    bool mirror, flip;
    double err_x, err_y;        /*!< achieved centre - requested centre */
    double cx_min, cx_max, cy_min, cy_max; /*!< achievable output-centre range */
    /* register values derived from the window */
    int array_x_start, array_x_end, array_y_start, array_y_end;
    int isp_off_x, isp_off_y;
} lens_window_t;

/** Load calibration from NVS (defaults when unset). NVS must be initialised. */
esp_err_t lens_calib_init(void);

/** Current calibration (RAM copy). */
const lens_calibration_t *lens_calib_get(void);

/** Default calibration (active-area centre, no distortion). */
void lens_calib_get_default(lens_calibration_t *cal);

/** Replace the RAM copy (no NVS write). */
void lens_calib_set(const lens_calibration_t *cal);

/** Persist the RAM copy to NVS (defaults erase the stored record). */
esp_err_t lens_calib_save(void);

/** True if the sensor driving the camera is an OV5647 (window rewriting applies). */
bool lens_sensor_supported(const char *sensor_name);

/** Compute the lens-centred window of a mode without applying it. */
bool lens_window_compute(const esp_cam_sensor_format_t *fmt, const lens_calibration_t *cal, lens_window_t *win);

/**
 * Build a RAM copy of @p base with its readout window rewritten for @p cal.
 * The returned format (and its register table) lives in static storage that
 * stays valid while the driver uses it (two slots, alternating, so the
 * previously applied format remains valid for the driver's rollback; the
 * slot whose register table is @p in_use_regs is never overwritten).
 * Returns NULL if the mode cannot be handled; @p win receives the window.
 */
const esp_cam_sensor_format_t *lens_window_build_format(const esp_cam_sensor_format_t *base,
                                                        const lens_calibration_t *cal,
                                                        const void *in_use_regs, lens_window_t *win);

/** True if @p regs points at one of the rewritten register tables. */
bool lens_window_is_built_regs(const void *regs);

/** JSON helpers for the camera-info API. */
cJSON *lens_sensor_json(void);
cJSON *lens_calibration_json(const lens_calibration_t *cal);
cJSON *lens_window_json(const lens_window_t *win);

#ifdef __cplusplus
}
#endif
