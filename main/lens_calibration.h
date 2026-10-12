/*
 * Lens-centred sensor readout windows and persistent, per-sensor lens calibration.
 *
 * Supported sensors
 * -----------------
 * The window layer is sensor-aware.  lens_sensor_select() picks a profile by
 * the esp_cam_sensor name of the detected sensor; everything else works on
 * the selected profile:
 *
 *   OV5647  window registers 0x3800-0x3813 rewritten (see below).
 *   IMX708  digital-crop offset registers 0x0408-0x040B rewritten inside the
 *           2x2-binned 2304x1296 field of the mushbraindave/esp_cam_sensor_imx
 *           modes (see "IMX708" below).
 *
 * Calibration is stored in NVS per sensor (namespace "lens_cal", key
 * "calib_<sensor>", e.g. "calib_ov5647", "calib_imx708"), so swapping cameras
 * never applies the wrong lens centre.  The pre-IMX708 key "calib" is migrated
 * to "calib_ov5647" by lens_calib_init().
 *
 * Sensor coordinate system
 * ------------------------
 * "Sensor coordinates" are the sensor's own full-resolution pixel addresses.
 * Coordinates are continuous with the *edge* convention: column i covers
 * [i, i + 1), so its centre is at i + 0.5.  The same convention is used for
 * output-image pixels.
 *
 * OV5647: the address space of the array-window registers 0x3800-0x3807.
 *   whole array : x 0..2623, y 0..1955  (2624 x 1956, incl. dummy/dark border)
 *   active area : x 16..2607, y 6..1949 (2592 x 1944 imaging pixels)
 *   default lens centre (1312.0, 978.0)
 *
 * IMX708: the address space of the analog-crop registers 0x0344-0x034B, which
 * starts at the first effective pixel (the optical-black border is not
 * addressable there).
 *   whole array = active area : x 0..4607, y 0..2591 (4608 x 2592)
 *   default lens centre (2304.0, 1296.0)
 *
 * Window model (see docs/lens-calibration.md)
 * --------------------------------------------
 * For every mode the firmware reads out an output region of
 * (out_w * scale) x (out_h * scale) sensor pixels whose top-left corner
 * (window.x, window.y) is placed so that the region's centre lands on the
 * lens centre (rounded to keep the Bayer phase, then clamped to what the mode
 * can reach).
 *
 * Output pixel edge coordinates (u, v) map to sensor coordinates as
 *   X = mirror ? window.x + window.width  - u * scale : window.x + u * scale
 *   Y = flip   ? window.y + window.height - v * scale : window.y + v * scale
 *
 * OV5647 specifics: the array window programmed into 0x3800-0x3807 is the
 * output region grown by a symmetric ISP margin, and 0x3810-0x3813 skip
 * exactly that margin.
 *
 * IMX708 specifics: every mode reads the whole array, reduces it to a field
 * by binning (0x0900/0x0901: 2x2 -> 2304x1296, 4x4 -> 1152x648) and/or
 * subsampling (0x0383/0x0387) and takes a digital crop of crop_w x crop_h at
 * (crop_x, crop_y) field pixels (registers 0x0408-0x040F); the output size
 * equals the crop (the driver's modes and the extra modes of imx708_modes.c;
 * a scaler setting in the table, 0x0401/0x0405, is honoured in the maths).
 * Moving the crop moves the window by bin*skip sensor px per field px, in
 * steps of LENS_IMX708_CROP_STEP field px.  The crop offsets count in readout
 * order, i.e. after the sensor's mirror/flip (0x0101); the window math
 * accounts for that.  A mode whose crop is the whole field has no slack: its
 * centre range collapses to the field centre.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_cam_sensor.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OV5647 geometry (kept under the historic names). */
#define LENS_SENSOR_ARRAY_WIDTH    2624
#define LENS_SENSOR_ARRAY_HEIGHT   1956
#define LENS_SENSOR_ACTIVE_X       16
#define LENS_SENSOR_ACTIVE_Y       6
#define LENS_SENSOR_ACTIVE_WIDTH   2592
#define LENS_SENSOR_ACTIVE_HEIGHT  1944

/* IMX708 geometry (effective pixel array = analog-crop address space). */
#define LENS_IMX708_ARRAY_WIDTH    4608
#define LENS_IMX708_ARRAY_HEIGHT   2592

typedef enum {
    LENS_SENSOR_NONE = 0,
    LENS_SENSOR_OV5647,
    LENS_SENSOR_IMX708,
} lens_sensor_kind_t;

/** Geometry of the selected sensor, in sensor coordinates. */
typedef struct {
    lens_sensor_kind_t kind;
    const char *name;           /*!< esp_cam_sensor name, e.g. "OV5647" */
    int array_width, array_height;
    int active_x, active_y, active_width, active_height;
} lens_sensor_geometry_t;

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
    int scale_x, scale_y;       /*!< sensor pixels per field pixel (binning x subsampling) */
    double scale, scale_v;      /*!< sensor pixels per output pixel (x, y); non-integer for scaled modes */
    bool mirror, flip;
    double err_x, err_y;        /*!< achieved centre - requested centre */
    double cx_min, cx_max, cy_min, cy_max; /*!< achievable output-centre range */
    /* OV5647 register values derived from the window */
    int array_x_start, array_x_end, array_y_start, array_y_end;
    int isp_off_x, isp_off_y;
    /* IMX708 digital-crop offsets (binned px, readout order) */
    int crop_x, crop_y;
} lens_window_t;

/**
 * NVS housekeeping at boot (migrates the legacy "calib" key to
 * "calib_ov5647"). NVS must be initialised. No calibration is active until a
 * sensor is selected.
 */
esp_err_t lens_calib_init(void);

/**
 * Select the sensor profile by esp_cam_sensor name and load its calibration
 * from NVS (defaults when unset). Idempotent for the already-selected sensor.
 * Returns false (and deselects) for sensors without lens-window support.
 */
bool lens_sensor_select(const char *sensor_name);

/** Geometry of the selected sensor, or NULL if none is selected. */
const lens_sensor_geometry_t *lens_sensor_geometry(void);

/**
 * Sensor mirror/flip the firmware programs for the selected sensor before a
 * mode is applied (IMX708: through the driver's HMIRROR/VFLIP parameters).
 * Returns false if the orientation is handled inside the mode's register
 * table instead (OV5647) or no sensor is selected.
 */
bool lens_sensor_orientation(int *hmirror, int *vflip);

/** Current calibration (RAM copy) of the selected sensor. */
const lens_calibration_t *lens_calib_get(void);

/** Default calibration (active-area centre, no distortion) of the selected sensor. */
void lens_calib_get_default(lens_calibration_t *cal);

/** Replace the RAM copy (no NVS write). */
void lens_calib_set(const lens_calibration_t *cal);

/** Persist the RAM copy to the selected sensor's NVS key (defaults erase the stored record). */
esp_err_t lens_calib_save(void);

/** True if lens-centred windows are implemented for this sensor. */
bool lens_sensor_supported(const char *sensor_name);

/** Compute the lens-centred window of a mode of the selected sensor without applying it. */
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
                                                        const void *in_use_regs, lens_window_t *win,
                                                        double fps_cap);

/**
 * OV5647: the frame rate @p base runs at with a sensor frame-rate cap of
 * @p fps_cap (0 = none): VTS is stretched by lens_window_build_format() to
 * ceil(vts * fps / cap) lines when the cap is below the mode's rate. Returns
 * the resulting rate and, through @p vts_out (may be NULL), the VTS.
 */
double lens_ov5647_capped_fps(const esp_cam_sensor_format_t *base, double fps_cap, int *vts_out);

/**
 * Window of a mode that shows the whole sensor (IMX708 tile scan): the
 * active area at @p scale sensor px per output px. Its centre is fixed, so
 * centerRange is a single point and centerError is the sensor centre's offset
 * from the lens centre.
 */
bool lens_window_full_sensor(const lens_calibration_t *cal, double scale, lens_window_t *win);

/** True if @p regs points at one of the rewritten register tables. */
bool lens_window_is_built_regs(const void *regs);

/** JSON helpers for the camera-info API. */
cJSON *lens_sensor_json(void);
cJSON *lens_calibration_json(const lens_calibration_t *cal);
cJSON *lens_window_json(const lens_window_t *win);

#ifdef __cplusplus
}
#endif
