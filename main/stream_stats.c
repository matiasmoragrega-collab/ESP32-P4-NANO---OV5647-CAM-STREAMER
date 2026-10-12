/*
 * Per-stage MJPEG pipeline timing; see stream_stats.h.
 */
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "isp_ipa_hook.h"
#include "stream_stats.h"

#define STREAM_STATS_WINDOW_US  (5 * 1000 * 1000)

typedef struct {
    uint64_t sum;
    uint32_t max;
    uint32_t n;
} acc_t;

typedef struct {
    int64_t t_start_us;
    int64_t t_end_us;
    uint32_t isp_frames_start;
    uint32_t isp_frames;        /*!< frames the sensor/ISP produced in the window */
    uint32_t captured;          /*!< frames dequeued from V4L2 */
    uint32_t encoded;
    uint32_t drop_no_slot;
    uint32_t not_wanted;
    uint32_t cap_skipped;
    uint32_t sent;
    uint32_t client_skipped;
    acc_t dq_wait;
    acc_t encode;
    acc_t jpeg_bytes;
    acc_t wait;
    acc_t send;
    acc_t sent_bytes;
} window_t;

static const char *TAG = "stream";
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static window_t s_cur;
static window_t s_last;
static bool s_have_last;
static stream_stats_info_t s_info;

static inline void acc(acc_t *a, uint32_t v)
{
    a->sum += v;
    a->n++;
    if (v > a->max) {
        a->max = v;
    }
}

static inline double avg(const acc_t *a)
{
    return a->n ? (double)a->sum / a->n : 0;
}

void stream_stats_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        stream_stats_reset();
    }
}

static void start_window(window_t *w, int64_t now)
{
    memset(w, 0, sizeof(*w));
    w->t_start_us = now;
    w->isp_frames_start = isp_hook_frame_count();
}

void stream_stats_reset(void)
{
    stream_stats_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    start_window(&s_cur, esp_timer_get_time());
    s_have_last = false;
    xSemaphoreGive(s_lock);
}

void stream_stats_set_info(const stream_stats_info_t *info)
{
    stream_stats_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_info = *info;
    xSemaphoreGive(s_lock);
}

static void log_window(const window_t *w)
{
    double secs = (w->t_end_us - w->t_start_us) / 1e6;
    if (secs <= 0 || (w->encoded == 0 && w->sent == 0)) {   /* quiet while nobody watches */
        return;
    }
    ESP_LOGI(TAG, "%" PRIu32 "x%" PRIu32 " q%" PRIu32 ": sensor %.1f fps, captured %.1f, encoded %.1f, sent %.1f fps | "
             "dq %.1f/%.1f ms, enc %.1f/%.1f ms, send %.1f/%.1f ms (avg/max) | %.0f KB, %.2f MB/s | "
             "drop(no slot) %" PRIu32 ", client skipped %" PRIu32 ", cap: not encoded %" PRIu32 ", skipped %" PRIu32,
             s_info.width, s_info.height, s_info.quality,
             w->isp_frames / secs, w->captured / secs, w->encoded / secs, w->sent / secs,
             avg(&w->dq_wait) / 1000, w->dq_wait.max / 1000.0, avg(&w->encode) / 1000, w->encode.max / 1000.0,
             avg(&w->send) / 1000, w->send.max / 1000.0,
             avg(&w->sent_bytes) / 1024, w->sent_bytes.sum / secs / 1e6,
             w->drop_no_slot, w->client_skipped, w->not_wanted, w->cap_skipped);
}

/* Caller holds the lock. Rolls the window over when it is old enough. */
static void maybe_roll(int64_t now)
{
    if (now - s_cur.t_start_us < STREAM_STATS_WINDOW_US) {
        return;
    }
    s_cur.t_end_us = now;
    s_cur.isp_frames = isp_hook_frame_count() - s_cur.isp_frames_start;
    s_last = s_cur;
    s_have_last = true;
    start_window(&s_cur, now);
    log_window(&s_last);
}

void stream_stats_capture(uint32_t dq_wait_us, uint32_t encode_us, uint32_t jpeg_bytes, bool encoded)
{
    if (!s_lock) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.captured++;
    acc(&s_cur.dq_wait, dq_wait_us);
    if (encoded) {
        s_cur.encoded++;
        acc(&s_cur.encode, encode_us);
        acc(&s_cur.jpeg_bytes, jpeg_bytes);
    }
    maybe_roll(now);
    xSemaphoreGive(s_lock);
}

void stream_stats_not_wanted(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.not_wanted++;
    xSemaphoreGive(s_lock);
}

void stream_stats_cap_skip(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.cap_skipped++;
    xSemaphoreGive(s_lock);
}

void stream_stats_encoded(uint32_t encode_us, uint32_t jpeg_bytes)
{
    if (!s_lock) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.encoded++;
    acc(&s_cur.encode, encode_us);
    acc(&s_cur.jpeg_bytes, jpeg_bytes);
    maybe_roll(now);
    xSemaphoreGive(s_lock);
}

void stream_stats_drop_no_slot(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.drop_no_slot++;
    xSemaphoreGive(s_lock);
}

void stream_stats_sent(uint32_t wait_us, uint32_t send_us, uint32_t bytes, uint32_t skipped)
{
    if (!s_lock) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.sent++;
    s_cur.client_skipped += skipped;
    acc(&s_cur.wait, wait_us);
    acc(&s_cur.send, send_us);
    acc(&s_cur.sent_bytes, bytes);
    maybe_roll(now);
    xSemaphoreGive(s_lock);
}

static void add_acc(cJSON *obj, const char *name, const acc_t *a, double scale)
{
    cJSON *o = cJSON_AddObjectToObject(obj, name);
    cJSON_AddNumberToObject(o, "avg", avg(a) * scale);
    cJSON_AddNumberToObject(o, "max", a->max * scale);
    cJSON_AddNumberToObject(o, "n", a->n);
}

static cJSON *window_json(const window_t *w, int64_t end_us, uint32_t isp_frames)
{
    cJSON *o = cJSON_CreateObject();
    double secs = (end_us - w->t_start_us) / 1e6;
    cJSON_AddNumberToObject(o, "seconds", secs);
    if (secs > 0) {
        cJSON_AddNumberToObject(o, "sensor_fps", isp_frames / secs);
        cJSON_AddNumberToObject(o, "captured_fps", w->captured / secs);
        cJSON_AddNumberToObject(o, "encoded_fps", w->encoded / secs);
        cJSON_AddNumberToObject(o, "sent_fps", w->sent / secs);
        cJSON_AddNumberToObject(o, "sent_MBps", w->sent_bytes.sum / secs / 1e6);
    }
    cJSON_AddNumberToObject(o, "drop_no_slot", w->drop_no_slot);
    cJSON_AddNumberToObject(o, "not_encoded_cap", w->not_wanted);
    cJSON_AddNumberToObject(o, "client_cap_skipped", w->cap_skipped);
    cJSON_AddNumberToObject(o, "client_skipped", w->client_skipped);
    add_acc(o, "dqbuf_wait_ms", &w->dq_wait, 1e-3);
    add_acc(o, "encode_ms", &w->encode, 1e-3);
    add_acc(o, "jpeg_kb", &w->jpeg_bytes, 1.0 / 1024);
    add_acc(o, "client_wait_ms", &w->wait, 1e-3);
    add_acc(o, "send_ms", &w->send, 1e-3);
    add_acc(o, "sent_kb", &w->sent_bytes, 1.0 / 1024);
    return o;
}

cJSON *stream_stats_json(void)
{
    stream_stats_init();
    cJSON *root = cJSON_CreateObject();
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *info = cJSON_AddObjectToObject(root, "info");
    cJSON_AddNumberToObject(info, "width", s_info.width);
    cJSON_AddNumberToObject(info, "height", s_info.height);
    cJSON_AddNumberToObject(info, "quality", s_info.quality);
    cJSON_AddNumberToObject(info, "sensor_fps", round(s_info.sensor_fps * 1000) / 1000);
    cJSON_AddNumberToObject(info, "image_fps", round(s_info.image_fps * 1000) / 1000);
    cJSON_AddNumberToObject(info, "tiles", s_info.tiles);
    if (s_info.sensor_cap > 0) {
        cJSON_AddNumberToObject(info, "sensor_cap", s_info.sensor_cap);
    } else {
        cJSON_AddNullToObject(info, "sensor_cap");
    }
    if (s_info.stream_cap > 0) {
        cJSON_AddNumberToObject(info, "stream_cap", s_info.stream_cap);
    } else {
        cJSON_AddNullToObject(info, "stream_cap");
    }
    cJSON_AddNumberToObject(info, "frame_length", s_info.frame_length);
    cJSON_AddNumberToObject(info, "v4l2_buffers", s_info.v4l2_buffers);
    cJSON_AddNumberToObject(info, "jpeg_slots", s_info.jpeg_slots);
    if (s_have_last) {
        cJSON_AddItemToObject(root, "last", window_json(&s_last, s_last.t_end_us, s_last.isp_frames));
    }
    cJSON_AddItemToObject(root, "current", window_json(&s_cur, now, isp_hook_frame_count() - s_cur.isp_frames_start));
    xSemaphoreGive(s_lock);
    return root;
}
