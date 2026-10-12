/*
 * Extra IMX708 sensor modes, built in RAM from the mushbraindave/esp_cam_sensor_imx
 * driver's 1920x1080 table (the driver is never edited).
 *
 * Every driver mode is a centred digital crop of the 2x2-binned 2304x1296
 * field, and the ESP32-P4 CSI/ISP path cannot take a line wider than 1920 px,
 * so none of them shows the whole sensor. The sensor cannot shrink the field
 * (scaler, subsampling and binning factors other than 2x2 were tested on
 * hardware and do nothing - docs/camera-formats.md), so the one built-in mode
 * here is a 2x2 tile scan of the whole field (IMX708_TILE_OVERLAP below). The
 * spec fields for binning / subsampling / scaler remain for the debug
 * experimental mode (POST /api/debug/imx708_mode).
 *
 * The modes are enumerated after the driver's own (format index = driver count
 * + i). Their register tables live in static storage for the life of the
 * firmware (the driver keeps the pointer of the applied format). The lens
 * window layer (lens_calibration.c) reads binning, subsampling, scaler and
 * digital crop from the table, so the window maths needs no per-mode code.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_cam_sensor.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMX708_MODE_EXTRA_REGS  8

typedef struct {
    uint16_t out_w, out_h;      /*!< output size (0x034C/0x034E) */
    uint16_t crop_w, crop_h;    /*!< digital crop (0x040C/0x040E), in field px (after binning/subsampling) */
    uint8_t binning;            /*!< 0x0901: 0x11 none, 0x22 2x2, 0x44 4x4 ([7:4] h, [3:0] v) */
    uint8_t x_odd_inc;          /*!< 0x0383: 1 = every pixel, 3 = every 2nd Bayer pair, 7 = every 4th */
    uint8_t y_odd_inc;          /*!< 0x0387 */
    uint8_t scale_mode;         /*!< 0x0401: 0 none, 1 horizontal, 2 both */
    uint8_t scale_m;            /*!< 0x0405: output = input * 16 / M */
    uint8_t n_extra;            /*!< extra raw register writes, appended last */
    uint16_t extra_reg[IMX708_MODE_EXTRA_REGS];
    uint8_t extra_val[IMX708_MODE_EXTRA_REGS];
    char method[12];            /*!< how the field is covered: "tiled", or a test label */
    uint8_t tiles;              /*!< 2: 2x2 tile scan of the whole field (see below); 0: plain mode */
} imx708_mode_spec_t;

/*
 * Tile scan ("tiled" mode): the sensor runs a 2x2-binned digital crop a bit
 * larger than a quarter of the 2304x1296 field, and the capture task moves
 * the crop to the next quarter after every frame (live register writes, see
 * simple_video_server_example.c). Four consecutive frames are pasted into a
 * 2304x1296 canvas, i.e. the whole sensor at the binned resolution. Tiles
 * overlap by IMX708_TILE_OVERLAP field px so the ISP's border pixels are
 * never used.
 */
#define IMX708_TILE_OVERLAP     16

typedef struct {
    int count;                  /*!< number of tiles (4) */
    int tile_w, tile_h;         /*!< sensor output (= V4L2 frame) size */
    int canvas_w, canvas_h;     /*!< composed image size (the whole field) */
    int crop_x[4], crop_y[4];   /*!< digital-crop offsets (field px, readout order) */
    int src_x[4], src_y[4];     /*!< part of the tile frame that is used ... */
    int dst_x[4], dst_y[4];     /*!< ... and where it goes in the canvas */
    int part_w, part_h;         /*!< size of that part (a quarter of the canvas) */
} imx708_tiling_t;

/** Tile geometry of a tiled extra mode (matched by name); false for any other format. */
bool imx708_modes_tiling(const esp_cam_sensor_format_t *fmt, imx708_tiling_t *t);

/** Build the built-in extra modes from the driver's 1920x1080 table. Idempotent. */
void imx708_modes_init(void);

/** Number of extra modes (built-in + the debug experimental slot, if defined). */
size_t imx708_modes_count(void);

/** Extra mode @p i (0-based, i.e. format index - driver count), or NULL. */
const esp_cam_sensor_format_t *imx708_modes_get(size_t i);

/**
 * Reduction method of a format ("crop" for the driver's own modes, the spec's
 * method for an extra mode), matched by name so it also works for the format
 * the driver reports back as current.
 */
const char *imx708_modes_method(const esp_cam_sensor_format_t *fmt);

/**
 * Debug: define (or replace) the experimental extra mode, appended after the
 * built-in ones. Returns its index in the extra list through @p index.
 */
esp_err_t imx708_modes_set_experimental(const imx708_mode_spec_t *spec, size_t *index);

#ifdef __cplusplus
}
#endif
