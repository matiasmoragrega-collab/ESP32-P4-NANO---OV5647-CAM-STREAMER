/*
 * Per-stage timing of the MJPEG pipeline, accumulated over 5 s windows.
 *
 *   capture task:  DQBUF wait -> JPEG encode -> publish
 *   stream client: wait for a new frame -> send (part header + JPEG)
 *
 * Each completed window is logged once ("stream: ...") and kept for
 * GET /api/debug/stream_stats (isp_debug_api.c). The sensor rate comes from
 * the ISP statistics interrupt count (isp_hook_frame_count()), so frames the
 * sensor produced but the pipeline dropped are visible as the difference
 * between sensor_fps and capture_fps.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t quality;
    float sensor_fps;           /*!< actual sensor frame rate (currentFrameRate) */
    float image_fps;            /*!< images per second before the stream cap (sensor / tiles) */
    float sensor_cap;           /*!< sensor frame-rate cap, 0 = none */
    float stream_cap;           /*!< MJPEG stream frame-rate cap, 0 = none */
    uint32_t tiles;             /*!< sensor frames per image (4: IMX708 full-view tile scan) */
    uint32_t frame_length;      /*!< sensor VTS in lines, 0 if unknown */
    uint32_t v4l2_buffers;
    uint32_t jpeg_slots;
} stream_stats_info_t;

void stream_stats_init(void);
/* Start a fresh window (mode change, stream (re)start). */
void stream_stats_reset(void);
void stream_stats_set_info(const stream_stats_info_t *info);

/* One frame taken from V4L2: time blocked in DQBUF, encode time (0 if not
 * encoded), JPEG bytes produced. */
void stream_stats_capture(uint32_t dq_wait_us, uint32_t encode_us, uint32_t jpeg_bytes, bool encoded);
/* A frame was dropped because every JPEG slot was in use. */
void stream_stats_drop_no_slot(void);
/* A frame was not encoded because no consumer was due for it (stream frame-rate cap). */
void stream_stats_not_wanted(void);
/* One image encoded outside the capture task (IMX708 tile scan: the composed canvas). */
void stream_stats_encoded(uint32_t encode_us, uint32_t jpeg_bytes);
/* A stream client passed over a frame because of its frame-rate cap. */
void stream_stats_cap_skip(void);
/* One part sent to a stream client: time waiting for a new frame, time in
 * send(), bytes on the wire, frames published but never sent to this client. */
void stream_stats_sent(uint32_t wait_us, uint32_t send_us, uint32_t bytes, uint32_t skipped);

/* Last completed window (and the one in progress) as JSON. Caller deletes. */
cJSON *stream_stats_json(void);

#ifdef __cplusplus
}
#endif
