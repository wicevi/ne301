/**
 * @file video_dual_model.c
 * @brief Generic two-stage inference engine implementation
 * @details Model A (active model, PIPE2 frames) detects; the top detection
 *          boxes are cropped from the raw PIPE2 RGB888 frame, letterboxed to
 *          Model B's input size and run through a second NN instance loaded
 *          from the non-active AI slot. See video_dual_model.h for the
 *          threading contract.
 *
 *          Data flow per AI cycle (all on the AI node thread, inside the
 *          frame hook the video_ai_node runs before the PIPE2 buffer is
 *          recycled):
 *
 *            Model A result ──filter/conf──> ranked boxes (top-K by area)
 *                  │
 *                  └─ for each box: expand ─> clamp ─> crop PIPE2
 *                         ─> letterbox bilinear warp ─> Model B inference
 *                         ─> consumer callback (embedding / class / ...)
 */

#include "video_dual_model.h"
#include "camera.h"
#include "buffer_mgr.h"
#include "debug.h"
#include "mem.h"
#include "mem_map.h"
#include "json_config_mgr.h"
#include "cmsis_os2.h"
#include <string.h>
#include <ctype.h>
#include <stddef.h>

/* ==================== Internal Definitions ==================== */

/* Consecutive Model B inference errors before the fail-safe latch engages */
#define DM_ERROR_LATCH_THRESHOLD 3u

/* Job slot lifecycle: FREE -> PENDING (crop staged) -> RUNNING (worker) -> FREE */
typedef enum {
    DM_JOB_FREE = 0,
    DM_JOB_PENDING,
    DM_JOB_RUNNING,
} dm_job_state_t;

typedef struct {
    volatile dm_job_state_t state;
    intptr_t tag;                  // attached by the target consumer
    uint32_t frame_id;
    float x, y, w, h;              // expanded crop box (A-input space)
    float det_conf;
    uint32_t b_infer_ms;           // filled by the worker
    nn_result_t b_result;          // worker's inference output (valid during the result cb)
    uint8_t *crop;                 // input_size bytes, allocated at model load
} dm_job_t;

typedef struct {
    aicam_bool_t initialized;

    osMutexId_t mutex;

    dual_model_config_t config;
    dual_model_status_t status;

    /* Model B instance + geometry (guarded by mutex) */
    nn_handle_t model_b;
    uint32_t input_w;              // Model B input width (px)
    uint32_t input_h;              // Model B input height (px)
    uint32_t input_size;           // input_w * input_h * 3

    /* PIPE2 (Model A input) geometry cache; refreshed from the primary
     * model info when the incoming buffer size disagrees */
    uint32_t pipe2_w;
    uint32_t pipe2_h;

    /* Consumers (single slots; the application layer may wrap its own
     * dispatcher when it needs more) */
    dual_model_target_cb_t target_cb;   // fast path, AI node thread, pre-B
    void *target_cb_user_data;
    dual_model_result_cb_t consumer;    // Model B results, worker thread
    void *consumer_user_data;

    /* Async Model B jobs: slots are guarded by the engine mutex, the
     * semaphore only wakes the worker (count == number of PENDING slots) */
    dm_job_t jobs[DM_JOB_SLOTS];
    osSemaphoreId_t job_sem;
    osThreadId_t worker;
    aicam_bool_t worker_run;
    uint32_t consecutive_errors;
    uint32_t last_pass_tick;
} video_dual_model_ctx_t;

static video_dual_model_ctx_t g_dm = {0};

/* Worker stack: the Model B inference path is the same depth the AI node
 * runs (VIDEO_THREAD_STACK_SIZE), so match it. */
#define DM_WORKER_STACK_SIZE 8192
static uint32_t dm_worker_stack[DM_WORKER_STACK_SIZE] __attribute__((aligned(8)));

/* ==================== Internal Function Declarations ==================== */

static void dm_target_from_od(const od_detect_t *d, float *x, float *y,
                              float *w, float *h, float *conf);
static uint32_t dm_collect_targets(const nn_result_t *result,
                                   const dual_model_config_t *cfg,
                                   uint32_t pipe2_w, uint32_t pipe2_h,
                                   float bx[], float by[], float bw[], float bh[],
                                   float bc[], uint32_t bi[],
                                   float bk[][DUAL_MODEL_MAX_KPTS][2],
                                   uint32_t bkn[]);
static void dm_sort_by_area(uint32_t n, float bw[], float bh[],
                            float bx[], float by[], float bc[], uint32_t bi[]);
static void dm_expand_box(float *x, float *y, float *w, float *h,
                          uint32_t expand_pm);
static void dm_letterbox_rgb888(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint32_t crop_x, uint32_t crop_y,
                                uint32_t crop_w, uint32_t crop_h,
                                uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                                uint32_t pad_value);
static aicam_bool_t dm_class_matches(const char *filter, const char *class_name);
static void dm_release_model_locked(video_dual_model_ctx_t *ctx);

/* ==================== Detection Helpers ==================== */

/* Extract a normalized box from an OD-flavor detection */
static void dm_target_from_od(const od_detect_t *d, float *x, float *y,
                              float *w, float *h, float *conf)
{
    *x = d->x;
    *y = d->y;
    *w = d->width;
    *h = d->height;
    *conf = d->conf;
}

/**
 * @brief Collect candidate boxes from Model A's result (OD or MPE flavor)
 * @return Number of candidates kept (<= DM_MAX_DETECTIONS)
 * @note  Boxes are normalized in Model A input space; the arrays are filled
 *        in detection order (sorting happens separately).
 */
static uint32_t dm_collect_targets(const nn_result_t *result,
                                   const dual_model_config_t *cfg,
                                   uint32_t pipe2_w, uint32_t pipe2_h,
                                   float bx[], float by[], float bw[], float bh[],
                                   float bc[], uint32_t bi[],
                                   float bk[][DUAL_MODEL_MAX_KPTS][2],
                                   uint32_t bkn[])
{
    if (!result || !result->is_valid) {
        return 0;
    }
    if (result->type != PP_TYPE_OD && result->type != PP_TYPE_MPE) {
        return 0;
    }

    uint32_t nb = (result->type == PP_TYPE_OD) ? result->od.nb_detect
                                               : result->mpe.nb_detect;
    if (nb > DM_MAX_DETECTIONS) {
        nb = DM_MAX_DETECTIONS;
    }

    const float min_conf = (float)cfg->target_conf_threshold / 100.0f;
    const float min_edge = (float)cfg->crop_min_size;

    uint32_t count = 0;
    for (uint32_t i = 0; i < nb; i++) {
        float x, y, w, h, conf;
        const char *class_name;
        uint32_t nk = 0;
        if (result->type == PP_TYPE_OD) {
            const od_detect_t *d = &result->od.detects[i];
            class_name = d->class_name;
            dm_target_from_od(d, &x, &y, &w, &h, &conf);
        } else {
            const mpe_detect_t *d = &result->mpe.detects[i];
            class_name = d->class_name;
            x = d->x; y = d->y; w = d->width; h = d->height; conf = d->conf;
            /* Carry the detection keypoints through (pose gating etc.) */
            nk = d->nb_keypoints;
            if (nk > DUAL_MODEL_MAX_KPTS) {
                nk = DUAL_MODEL_MAX_KPTS;
            }
        }

        if (w <= 0.0f || h <= 0.0f || conf <= 0.0f) {
            continue;
        }
        if (conf < min_conf) {
            continue;
        }
        if (!dm_class_matches(cfg->target_class, class_name)) {
            continue;
        }

        /* Clamp into the frame, then apply the minimum crop edge in pixels */
        if (x < 0.0f) x = 0.0f;
        if (y < 0.0f) y = 0.0f;
        if (x + w > 1.0f) w = 1.0f - x;
        if (y + h > 1.0f) h = 1.0f - y;
        if (w <= 0.0f || h <= 0.0f) {
            continue;
        }
        if (w * (float)pipe2_w < min_edge || h * (float)pipe2_h < min_edge) {
            continue;
        }

        bx[count] = x;
        by[count] = y;
        bw[count] = w;
        bh[count] = h;
        bc[count] = conf;
        bi[count] = i;
        bkn[count] = 0;
        if (nk > 0 && result->type == PP_TYPE_MPE) {
            const mpe_detect_t *d = &result->mpe.detects[i];
            for (uint32_t k = 0; k < nk; k++) {
                bk[count][k][0] = d->keypoints[k].x;
                bk[count][k][1] = d->keypoints[k].y;
            }
            bkn[count] = nk;
        }
        count++;
    }
    return count;
}

/* Case-insensitive substring match; empty filter accepts every class */
static aicam_bool_t dm_class_matches(const char *filter, const char *class_name)
{
    if (!filter || filter[0] == '\0') {
        return AICAM_TRUE;
    }
    if (!class_name) {
        return AICAM_FALSE;
    }
    /* Substring so "face" also matches "face 1"-style labels */
    size_t flen = strlen(filter);
    size_t clen = strlen(class_name);
    if (flen > clen) {
        return AICAM_FALSE;
    }
    for (size_t start = 0; start + flen <= clen; start++) {
        size_t k = 0;
        while (k < flen &&
               tolower((unsigned char)filter[k]) == tolower((unsigned char)class_name[start + k])) {
            k++;
        }
        if (k == flen) {
            return AICAM_TRUE;
        }
    }
    return AICAM_FALSE;
}

/* Insertion sort, largest area first (n <= DM_MAX_DETECTIONS) */
static void dm_sort_by_area(uint32_t n, float bw[], float bh[],
                            float bx[], float by[], float bc[], uint32_t bi[])
{
    for (uint32_t i = 1; i < n; i++) {
        float key_area = bw[i] * bh[i];
        float kw = bw[i], kh = bh[i], kx = bx[i], ky = by[i], kc = bc[i];
        uint32_t ki = bi[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && bw[j] * bh[j] < key_area) {
            bw[j + 1] = bw[j]; bh[j + 1] = bh[j];
            bx[j + 1] = bx[j]; by[j + 1] = by[j];
            bc[j + 1] = bc[j]; bi[j + 1] = bi[j];
            j--;
        }
        bw[j + 1] = kw; bh[j + 1] = kh;
        bx[j + 1] = kx; by[j + 1] = ky;
        bc[j + 1] = kc; bi[j + 1] = ki;
    }
}

/* Expand a normalized box by expand_pm/1000 of its size on every side */
static void dm_expand_box(float *x, float *y, float *w, float *h,
                          uint32_t expand_pm)
{
    float dx = *w * ((float)expand_pm / 1000.0f);
    float dy = *h * ((float)expand_pm / 1000.0f);
    *x -= dx;
    *y -= dy;
    *w += 2.0f * dx;
    *h += 2.0f * dy;
    /* Clamp back into the frame */
    if (*x < 0.0f) *x = 0.0f;
    if (*y < 0.0f) *y = 0.0f;
    if (*x + *w > 1.0f) *w = 1.0f - *x;
    if (*y + *h > 1.0f) *h = 1.0f - *y;
    if (*w <= 0.0f) *w = 0.0f;
    if (*h <= 0.0f) *h = 0.0f;
}

/* ==================== Letterbox Warp ==================== */

/**
 * @brief Crop a sub-rect from a packed RGB888 frame and letterbox-resize it
 *        (aspect-preserving bilinear scale + pad, never stretched)
 * @param src     Packed RGB888 source frame (src_w * src_h * 3 bytes)
 * @param crop_x  Crop rect in source pixels (pre-clamped by the caller)
 * @param dst     Destination buffer, dst_w * dst_h * 3 bytes
 * @param pad_value Fill value for the letterbox bars (0..255)
 */
static void dm_letterbox_rgb888(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint32_t crop_x, uint32_t crop_y,
                                uint32_t crop_w, uint32_t crop_h,
                                uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                                uint32_t pad_value)
{
    if (!src || !dst || crop_w == 0 || crop_h == 0 || dst_w == 0 || dst_h == 0) {
        return;
    }
    const uint8_t pad = (uint8_t)(pad_value & 0xFFu);

    /* Scale to fit, centered */
    float scale = (float)dst_w / (float)crop_w;
    if ((float)dst_h / (float)crop_h < scale) {
        scale = (float)dst_h / (float)crop_h;
    }
    const uint32_t out_w = (uint32_t)((float)crop_w * scale + 0.5f);
    const uint32_t out_h = (uint32_t)((float)crop_h * scale + 0.5f);
    const uint32_t off_x = (dst_w - out_w) / 2u;
    const uint32_t off_y = (dst_h - out_h) / 2u;

    /* Pad everything first (also covers rounding gaps at the edges) */
    memset(dst, pad, (size_t)dst_w * dst_h * 3u);

    /* Inverse mapping: for each destination pixel inside the scaled rect,
     * sample the source with bilinear interpolation. Working in 24.8 fixed
     * point keeps the inner loop free of float math. */
    const uint32_t step_x = (uint32_t)(((float)crop_w / (float)out_w) * 256.0f + 0.5f);
    const uint32_t step_y = (uint32_t)(((float)crop_h / (float)out_h) * 256.0f + 0.5f);

    for (uint32_t dy = 0; dy < out_h; dy++) {
        const uint32_t src_y_fixed = dy * step_y;                 /* 24.8 */
        const uint32_t y0 = src_y_fixed >> 8;
        uint32_t y1 = y0 + 1u;
        const uint32_t fy = src_y_fixed & 0xFFu;
        if (y1 >= crop_h) {
            y1 = crop_h - 1u;
        }
        const uint8_t *row0 = src + (size_t)(crop_y + y0) * src_w * 3u + (size_t)crop_x * 3u;
        const uint8_t *row1 = src + (size_t)(crop_y + y1) * src_w * 3u + (size_t)crop_x * 3u;
        uint8_t *dst_row = dst + (size_t)(off_y + dy) * dst_w * 3u + (size_t)off_x * 3u;

        uint32_t src_x_fixed = 0;
        for (uint32_t dx = 0; dx < out_w; dx++, src_x_fixed += step_x) {
            const uint32_t x0 = src_x_fixed >> 8;
            uint32_t x1 = x0 + 1u;
            const uint32_t fx = src_x_fixed & 0xFFu;
            if (x1 >= crop_w) {
                x1 = crop_w - 1u;
            }
            const size_t p0 = (size_t)x0 * 3u;
            const size_t p1 = (size_t)x1 * 3u;

            /* Bilinear per channel: weights 0..256, sum 256 */
            const uint32_t w00 = (256u - fx) * (256u - fy);
            const uint32_t w01 = fx * (256u - fy);
            const uint32_t w10 = (256u - fx) * fy;
            const uint32_t w11 = fx * fy;

            for (uint32_t c = 0; c < 3u; c++) {
                uint32_t v = (uint32_t)row0[p0 + c] * w00 +
                             (uint32_t)row0[p1 + c] * w01 +
                             (uint32_t)row1[p0 + c] * w10 +
                             (uint32_t)row1[p1 + c] * w11;
                dst_row[dx * 3u + c] = (uint8_t)(v >> 16);
            }
        }
    }
    (void)src_h;
}


/* ==================== PIPE1 (encode-resolution) aligned warp ==================== */

static int dm_similarity_fit(const float kpts_src[5][2], uint32_t dst_w, uint32_t dst_h,
                             float *a_out, float *b_out, float *c_out, float *d_out);

/**
 * @brief Warp a 5-point aligned face crop from the PIPE1 encode frame.
 * @details PIPE1 is 1280x720 RGB565 - 4-8x more pixels per face than PIPE2
 * (the 320x180 model-A input), which is what the embedding network needs to
 * separate identities cleanly. The landmarks arrive in PIPE2-normalized
 * space; the view-rect affine maps them into PIPE1 space first.
 * Same bilinear sampling as dm_align_crop_rgb888 but reads RGB565 pixels.
 */
static void dm_align_crop_pipe1(const uint8_t *pipe1_buf,
                                uint32_t p1_w, uint32_t p1_h,
                                const float kpts_nn[5][2],
                                uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    /* Map landmarks from PIPE2/NN-normalized to PIPE1-normalized, then to
     * PIPE1 pixel coordinates. */
    float vx, vy, vw, vh;
    camera_get_pipe2_view_rect(&vx, &vy, &vw, &vh);
    float px[5][2];
    for (int i = 0; i < 5; i++) {
        /* Order by image-x (detector left/right semantics independent) */
        px[i][0] = 0.0f; px[i][1] = 0.0f; /* filled below */
    }
    {
        const int e0 = (kpts_nn[0][0] <= kpts_nn[1][0]) ? 0 : 1;
        const int e1 = 1 - e0;
        const int m0 = (kpts_nn[3][0] <= kpts_nn[4][0]) ? 3 : 4;
        const int m1 = (m0 == 3) ? 4 : 3;
        const int order[5] = { e0, e1, 2, m0, m1 };
        for (int i = 0; i < 5; i++) {
            const float *kp = kpts_nn[order[i]];
            const float p1x = (vx + kp[0] * vw) * (float)p1_w;
            const float p1y = (vy + kp[1] * vh) * (float)p1_h;
            px[i][0] = p1x;
            px[i][1] = p1y;
        }
    }

    float a, b, c, d;
    if (dm_similarity_fit(px, dst_w, dst_h, &a, &b, &c, &d) != 0) {
        memset(dst, 127, (size_t)dst_w * dst_h * 3u);
        return;
    }

    const float det = a * a + b * b;
    if (det < 1e-9f) {
        memset(dst, 127, (size_t)dst_w * dst_h * 3u);
        return;
    }
    const float ia = a / det, ib = b / det;
    const float ic = -(ia * c + ib * d);
    const float id = -(-ib * c + ia * d);

    memset(dst, 127, (size_t)dst_w * dst_h * 3u);
    const float max_x = (float)p1_w - 1.0f;
    const float max_y = (float)p1_h - 1.0f;

    for (uint32_t v = 0; v < dst_h; v++) {
        for (uint32_t u = 0; u < dst_w; u++) {
            const float x = ia * (float)u + ib * (float)v + ic;
            const float y = -ib * (float)u + ia * (float)v + id;
            if (x < 0.0f || y < 0.0f || x > max_x || y > max_y) {
                continue;
            }
            const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
            const uint32_t x1 = (x0 + 1u < p1_w) ? x0 + 1u : x0;
            const uint32_t y1 = (y0 + 1u < p1_h) ? y0 + 1u : y0;
            const float fx = x - (float)x0, fy = y - (float)y0;

            /* RGB565: [RRRRRGGG GGGBBBBB], 2 bytes per pixel */
            #define P1_R(p) ((float)(((p) >> 11) & 0x1F) * (255.0f / 31.0f))
            #define P1_G(p) ((float)(((p) >> 5) & 0x3F) * (255.0f / 63.0f))
            #define P1_B(p) ((float)((p) & 0x1F) * (255.0f / 31.0f))
            const uint16_t *base = (const uint16_t *)pipe1_buf;
            const uint16_t p00 = base[(size_t)y0 * p1_w + x0];
            const uint16_t p01 = base[(size_t)y0 * p1_w + x1];
            const uint16_t p10 = base[(size_t)y1 * p1_w + x0];
            const uint16_t p11 = base[(size_t)y1 * p1_w + x1];
            const float w00 = (1.0f - fx) * (1.0f - fy);
            const float w01 = fx * (1.0f - fy);
            const float w10 = (1.0f - fx) * fy;
            const float w11 = fx * fy;

            uint8_t *out = dst + ((size_t)v * dst_w + u) * 3u;
            out[0] = (uint8_t)(P1_R(p00) * w00 + P1_R(p01) * w01 + P1_R(p10) * w10 + P1_R(p11) * w11 + 0.5f);
            out[1] = (uint8_t)(P1_G(p00) * w00 + P1_G(p01) * w01 + P1_G(p10) * w10 + P1_G(p11) * w11 + 0.5f);
            out[2] = (uint8_t)(P1_B(p00) * w00 + P1_B(p01) * w01 + P1_B(p10) * w10 + P1_B(p11) * w11 + 0.5f);
        }
    }
}

/* Canonical ArcFace 112x112 eye/nose/mouth landmark template (insightface
 * buffalo models are trained on faces warped onto these points). Feeding a
 * mere letterboxed box drops same-person similarity by ~10 points (0.6x
 * instead of 0.7x) because scale/rotation/translation of the face varies
 * per frame - that variance is what alignment removes. */
static const float dm_arcface_ref112[5][2] = {
    { 38.2946f, 51.6963f },   /* right eye  */
    { 73.5318f, 51.5014f },   /* left eye   */
    { 56.0252f, 71.7366f },   /* nose tip   */
    { 41.5493f, 92.3655f },   /* right mouth corner */
    { 70.7299f, 92.2041f },   /* left mouth corner  */
};

/**
 * @brief Least-squares similarity transform (scale+rotation+translation)
 *        mapping src points onto dst points.
 * @param kpts_src 5 landmarks in source-frame PIXEL coordinates
 * @return 0 on success; fills a,b,c,d with  [u;v] = [a -b; b a][x;y] + [c;d]
 */
static int dm_similarity_fit(const float kpts_src[5][2], uint32_t dst_w, uint32_t dst_h,
                             float *a_out, float *b_out, float *c_out, float *d_out)
{
    /* Reference points scaled from the 112x112 template to the actual input */
    float ref[5][2];
    const float sx = (float)dst_w / 112.0f;
    const float sy = (float)dst_h / 112.0f;
    for (int i = 0; i < 5; i++) {
        ref[i][0] = dm_arcface_ref112[i][0] * sx;
        ref[i][1] = dm_arcface_ref112[i][1] * sy;
    }

    float mx = 0.0f, my = 0.0f, mu = 0.0f, mv = 0.0f;
    for (int i = 0; i < 5; i++) {
        mx += kpts_src[i][0]; my += kpts_src[i][1];
        mu += ref[i][0];      mv += ref[i][1];
    }
    mx /= 5.0f; my /= 5.0f; mu /= 5.0f; mv /= 5.0f;

    float num_a = 0.0f, num_b = 0.0f, den = 0.0f;
    for (int i = 0; i < 5; i++) {
        const float xs = kpts_src[i][0] - mx, ys = kpts_src[i][1] - my;
        const float ud = ref[i][0] - mu,      vd = ref[i][1] - mv;
        num_a += xs * ud + ys * vd;
        num_b += xs * vd - ys * ud;
        den   += xs * xs + ys * ys;
    }
    if (den < 1e-6f) {
        return -1; /* degenerate landmarks */
    }
    const float a = num_a / den;
    const float b = num_b / den;
    *a_out = a;
    *b_out = b;
    *c_out = mu - (a * mx - b * my);
    *d_out = mv - (b * mx + a * my);
    return 0;
}

/**
 * @brief Warp the source frame so the 5 landmarks land on the ArcFace
 *        template: aligned face straight into dst (dst_w x dst_h RGB888).
 * @param kpts 5 landmarks in source-frame NORMALIZED (0..1) coordinates,
 *             same space as dual_model_target_t.kpts
 */
static void dm_align_crop_rgb888(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                 const float kpts[5][2],
                                 uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    /* Landmark ORDER normalization: the ArcFace template is in IMAGE-space
     * order (ref[0] = the eye at image LEFT, ref[4] = mouth corner at image
     * RIGHT), while detectors label eyes/mouth by ANATOMY ("right_eye" of
     * the person = image left or right depending on the port). A swapped
     * pair mirrors the warp and tanks the similarity. Order by x coordinate
     * instead - detector-semantic independent. Expected YuNet layout:
     * kpts[0..1] = eyes, [2] = nose, [3..4] = mouth corners. */
    float px[5][2];
    {
        const int e0 = (kpts[0][0] <= kpts[1][0]) ? 0 : 1;
        const int e1 = 1 - e0;
        const int m0 = (kpts[3][0] <= kpts[4][0]) ? 3 : 4;
        const int m1 = (m0 == 3) ? 4 : 3;
        const int order[5] = { e0, e1, 2, m0, m1 };
        for (int i = 0; i < 5; i++) {
            px[i][0] = kpts[order[i]][0] * (float)src_w;
            px[i][1] = kpts[order[i]][1] * (float)src_h;
        }
    }

    float a, b, c, d;
    if (dm_similarity_fit(px, dst_w, dst_h, &a, &b, &c, &d) != 0) {
        memset(dst, 127, (size_t)dst_w * dst_h * 3u);
        return;
    }

    /* Inverse transform: [x;y] = inv(T) * ([u;v] - t), T = [[a,-b],[b,a]] */
    const float det = a * a + b * b;
    if (det < 1e-9f) {
        memset(dst, 127, (size_t)dst_w * dst_h * 3u);
        return;
    }
    const float ia = a / det, ib = b / det;   /* inv = [[ia,ib],[-ib,ia]] */
    const float ic = -(ia * c + ib * d);
    const float id = -(-ib * c + ia * d);

    memset(dst, 127, (size_t)dst_w * dst_h * 3u);
    const float max_x = (float)src_w - 1.0f;
    const float max_y = (float)src_h - 1.0f;

    for (uint32_t v = 0; v < dst_h; v++) {
        for (uint32_t u = 0; u < dst_w; u++) {
            const float x = ia * (float)u + ib * (float)v + ic;
            const float y = -ib * (float)u + ia * (float)v + id;
            if (x < 0.0f || y < 0.0f || x > max_x || y > max_y) {
                continue; /* stays pad gray (out-of-frame warp) */
            }
            const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
            const uint32_t x1 = (x0 + 1u < src_w) ? x0 + 1u : x0;
            const uint32_t y1 = (y0 + 1u < src_h) ? y0 + 1u : y0;
            const float fx = x - (float)x0, fy = y - (float)y0;
            const uint32_t w00 = (uint32_t)((1.0f - fx) * (1.0f - fy) * 256.0f);
            const uint32_t w01 = (uint32_t)(fx * (1.0f - fy) * 256.0f);
            const uint32_t w10 = (uint32_t)((1.0f - fx) * fy * 256.0f);
            const uint32_t w11 = (uint32_t)(fx * fy * 256.0f);
            const uint8_t *p00 = src + ((size_t)y0 * src_w + x0) * 3u;
            const uint8_t *p01 = p00 + (x1 - x0) * 3u;
            const uint8_t *p10 = src + ((size_t)y1 * src_w + x0) * 3u;
            const uint8_t *p11 = p10 + (x1 - x0) * 3u;
            uint8_t *out = dst + ((size_t)v * dst_w + u) * 3u;
            for (uint32_t ch = 0; ch < 3u; ch++) {
                uint32_t val = (uint32_t)p00[ch] * w00 + (uint32_t)p01[ch] * w01 +
                               (uint32_t)p10[ch] * w10 + (uint32_t)p11[ch] * w11;
                out[ch] = (uint8_t)(val >> 8);
            }
        }
    }
}

/* ==================== Model B Lifecycle ==================== */

/* Free Model B resources - caller holds the mutex */
static void dm_release_model_locked(video_dual_model_ctx_t *ctx)
{
    if (ctx->model_b) {
        nn_instance_unload_model(ctx->model_b);
        nn_instance_destroy(ctx->model_b);
        ctx->model_b = NULL;
    }
    for (uint32_t i = 0; i < DM_JOB_SLOTS; i++) {
        if (ctx->jobs[i].crop) {
            hal_mem_free(ctx->jobs[i].crop);
            ctx->jobs[i].crop = NULL;
        }
        ctx->jobs[i].state = DM_JOB_FREE;
    }
    ctx->input_w = 0;
    ctx->input_h = 0;
    ctx->input_size = 0;
    ctx->status.model_b_loaded = AICAM_FALSE;
    ctx->status.model_b_name[0] = '\0';
    ctx->status.model_b_postprocess[0] = '\0';
    ctx->status.model_b_input_w = 0;
    ctx->status.model_b_input_h = 0;
    ctx->consecutive_errors = 0;
    ctx->status.disabled_by_errors = AICAM_FALSE;
}

aicam_result_t video_dual_model_load_model(void)
{
    if (!g_dm.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    if (osMutexAcquire(g_dm.mutex, 500) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }

    if (g_dm.model_b) {
        /* Also clears the fail-safe latch */
        dm_release_model_locked(&g_dm);
    }

    /* Model A runs the ACTIVE slot; Model B takes the other one (mirrors
     * the complementary-address convention of video_ai_node's primary
     * loader). The +1024 skips the OTA package header. */
    uintptr_t model_addr = json_config_get_ai_1_active()
                               ? (AI_1_BASE + 1024u)
                               : (AI_2_BASE + 1024u);

    nn_handle_t handle = nn_instance_create();
    if (!handle) {
        LOG_CORE_ERROR("DualModel: failed to create Model B instance");
        osMutexRelease(g_dm.mutex);
        return AICAM_ERROR_NO_MEMORY;
    }

    if (nn_instance_load_model(handle, model_addr) != 0) {
        LOG_CORE_WARN("DualModel: no valid Model B package at 0x%08lX "
                      "(two-stage processing stays off)", (uint32_t)model_addr);
        nn_instance_destroy(handle);
        osMutexRelease(g_dm.mutex);
        g_dm.status.model_b_addr = 0;
        snprintf(g_dm.status.model_b_error, sizeof(g_dm.status.model_b_error),
                 "no valid model package in the Model B slot");
        return AICAM_ERROR_NOT_FOUND;
    }

    nn_model_info_t info;
    if (nn_instance_get_model_info(handle, &info) != 0) {
        LOG_CORE_ERROR("DualModel: failed to read Model B info");
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        osMutexRelease(g_dm.mutex);
        return AICAM_ERROR;
    }

    /* Validate the input contract: packed RGB888 uint8 (the whole PIPE2
     * path, and this warp stage, only speak that format). The compiled
     * network's real input buffer is authoritative - the JSON metadata can
     * disagree when a different variant is flashed. */
    uint8_t *model_input = NULL;
    uint32_t input_buffer_size = 0;
    if (nn_instance_get_model_input_buffer(handle, &model_input, &input_buffer_size) != 0 ||
        input_buffer_size == 0) {
        LOG_CORE_ERROR("DualModel: failed to query Model B input buffer");
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        osMutexRelease(g_dm.mutex);
        return AICAM_ERROR;
    }

    uint32_t expected = info.input_width * info.input_height * 3u;
    if (info.input_channels != 3u || input_buffer_size != expected) {
        LOG_CORE_ERROR("DualModel: Model B input mismatch (network %lu B vs "
                       "%lux%lux3=%lu B) - wrong model variant flashed?",
                       (unsigned long)input_buffer_size,
                       (unsigned long)info.input_width,
                       (unsigned long)info.input_height,
                       (unsigned long)expected);
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        osMutexRelease(g_dm.mutex);
        snprintf(g_dm.status.model_b_error, sizeof(g_dm.status.model_b_error),
                 "input contract mismatch (%lux%lux%lu network)",
                 (unsigned long)info.input_width,
                 (unsigned long)info.input_height,
                 (unsigned long)info.input_channels);
        return AICAM_ERROR_INVALID_DATA;
    }

    /* Load contract: reject packages whose postprocess type does not match
     * the configured application (face ships "pp_face_embed"). A detection
     * model flashed into the Model B slot would otherwise load fine (same
     * RGB888 input contract) and silently burn NPU cycles producing results
     * the application consumer drops. Empty expected_postprocess = off. */
    if (g_dm.config.expected_postprocess[0] != '\0' &&
        strncmp(info.postprocess_type, g_dm.config.expected_postprocess,
                sizeof(info.postprocess_type)) != 0) {
        LOG_CORE_ERROR("DualModel: Model B postprocess mismatch: package is '%s' "
                       "but the application expects '%s' - refusing to load",
                       info.postprocess_type, g_dm.config.expected_postprocess);
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        osMutexRelease(g_dm.mutex);
        snprintf(g_dm.status.model_b_error, sizeof(g_dm.status.model_b_error),
                 "postprocess mismatch: got '%.16s', want '%.16s'",
                 info.postprocess_type, g_dm.config.expected_postprocess);
        return AICAM_ERROR_INVALID_DATA;
    }

    /* One crop buffer per job slot (replaces the old single warp buffer) */
    uint32_t crops_ok = 0;
    for (uint32_t i = 0; i < DM_JOB_SLOTS; i++) {
        g_dm.jobs[i].crop = (uint8_t *)hal_mem_alloc_large(input_buffer_size);
        g_dm.jobs[i].state = DM_JOB_FREE;
        if (g_dm.jobs[i].crop) {
            crops_ok++;
        }
    }
    if (crops_ok == 0) {
        LOG_CORE_ERROR("DualModel: job crop alloc failed (%lu B x %u)",
                       (unsigned long)input_buffer_size, (unsigned)DM_JOB_SLOTS);
        for (uint32_t i = 0; i < DM_JOB_SLOTS; i++) {
            if (g_dm.jobs[i].crop) {
                hal_mem_free(g_dm.jobs[i].crop);
                g_dm.jobs[i].crop = NULL;
            }
        }
        nn_instance_unload_model(handle);
        nn_instance_destroy(handle);
        osMutexRelease(g_dm.mutex);
        return AICAM_ERROR_NO_MEMORY;
    }

    g_dm.model_b = handle;
    g_dm.input_w = info.input_width;
    g_dm.input_h = info.input_height;
    g_dm.input_size = input_buffer_size;
    g_dm.consecutive_errors = 0;
    g_dm.status.disabled_by_errors = AICAM_FALSE;

    g_dm.status.model_b_loaded = AICAM_TRUE;
    g_dm.status.model_b_error[0] = '\0';
    strncpy(g_dm.status.model_b_name, info.name, sizeof(g_dm.status.model_b_name) - 1u);
    g_dm.status.model_b_name[sizeof(g_dm.status.model_b_name) - 1u] = '\0';
    strncpy(g_dm.status.model_b_postprocess, info.postprocess_type,
            sizeof(g_dm.status.model_b_postprocess) - 1u);
    g_dm.status.model_b_postprocess[sizeof(g_dm.status.model_b_postprocess) - 1u] = '\0';
    g_dm.status.model_b_input_w = info.input_width;
    g_dm.status.model_b_input_h = info.input_height;
    g_dm.status.model_b_addr = (uint32_t)model_addr;

    LOG_CORE_INFO("DualModel: Model B loaded '%s' %lux%lu RGB888 from 0x%08lX "
                  "(pp=%s)",
                  g_dm.status.model_b_name,
                  (unsigned long)info.input_width, (unsigned long)info.input_height,
                  (unsigned long)model_addr, g_dm.status.model_b_postprocess);

    osMutexRelease(g_dm.mutex);
    return AICAM_OK;
}

aicam_result_t video_dual_model_reload_model(void)
{
    if (!g_dm.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    /* Quiesce: the frame hook is gated on model_b != NULL under the mutex,
     * but the AI node thread may be INSIDE a pass right now. Holding the
     * mutex through the release is enough - the pass holds it too, so we
     * simply wait for the in-flight one to finish. */
    if (osMutexAcquire(g_dm.mutex, 2000) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    dm_release_model_locked(&g_dm);
    osMutexRelease(g_dm.mutex);

    return video_dual_model_load_model();
}

/* ==================== Frame Hook ==================== */

/* Find a job slot for a submission: a PENDING slot with the same tag is
 * replaced (fresher crop for the same face wins), else a FREE slot is used.
 * Returns NULL when all slots are busy with other faces. */
static dm_job_t *dm_claim_job_slot(video_dual_model_ctx_t *ctx, intptr_t tag)
{
    dm_job_t *free_slot = NULL;
    for (uint32_t i = 0; i < DM_JOB_SLOTS; i++) {
        dm_job_t *j = &ctx->jobs[i];
        if (j->state == DM_JOB_PENDING && j->tag == tag) {
            return j; /* replace the stale crop in place */
        }
        if (j->state == DM_JOB_FREE && !free_slot) {
            free_slot = j;
        }
    }
    return free_slot;
}

void video_dual_model_on_frame(const nn_result_t *result,
                               const uint8_t *pipe2_buffer,
                               uint32_t pipe2_size,
                               uint32_t frame_id,
                               void *user_data)
{
    (void)user_data;
    if (!g_dm.initialized || !result || !pipe2_buffer) {
        return;
    }

    /* Cheap gate before any locking */
    if (!g_dm.config.enabled || !g_dm.model_b || g_dm.status.disabled_by_errors) {
        return;
    }

    uint32_t now = osKernelGetTickCount();
    if (g_dm.last_pass_tick != 0 &&
        (now - g_dm.last_pass_tick) < g_dm.config.interval_ms) {
        g_dm.status.skipped_throttle++;
        return;
    }

    if (osMutexAcquire(g_dm.mutex, 100) != osOK) {
        return;
    }

    /* Re-check under the lock (load/reload may have raced us) */
    if (!g_dm.config.enabled || !g_dm.model_b || g_dm.status.disabled_by_errors) {
        osMutexRelease(g_dm.mutex);
        return;
    }

    /* PIPE2 geometry: refresh when the size no longer matches the cache
     * (e.g. the primary model was re-flashed with another resolution). */
    if (g_dm.pipe2_w == 0 || g_dm.pipe2_w * g_dm.pipe2_h * 3u != pipe2_size) {
        nn_model_info_t primary;
        if (nn_get_model_info(&primary) != 0 ||
            primary.input_width == 0 ||
            primary.input_width * primary.input_height * 3u != pipe2_size) {
            /* Unknown geometry - cannot map boxes to pixels */
            osMutexRelease(g_dm.mutex);
            return;
        }
        g_dm.pipe2_w = primary.input_width;
        g_dm.pipe2_h = primary.input_height;
    }
    const uint32_t pw = g_dm.pipe2_w;
    const uint32_t ph = g_dm.pipe2_h;

    uint32_t pass_start = osKernelGetTickCount();
    g_dm.last_pass_tick = now;

    /* 1) Collect + rank Model A targets */
    float bx[DM_MAX_DETECTIONS], by[DM_MAX_DETECTIONS];
    float bw[DM_MAX_DETECTIONS], bh[DM_MAX_DETECTIONS];
    float bc[DM_MAX_DETECTIONS];
    uint32_t bi[DM_MAX_DETECTIONS];
    float bk[DM_MAX_DETECTIONS][DUAL_MODEL_MAX_KPTS][2];
    uint32_t bkn[DM_MAX_DETECTIONS];
    uint32_t count = dm_collect_targets(result, &g_dm.config, pw, ph,
                                        bx, by, bw, bh, bc, bi, bk, bkn);
    if (count == 0) {
        g_dm.status.last_targets = 0;
        osMutexRelease(g_dm.mutex);
        return;
    }
    dm_sort_by_area(count, bw, bh, bx, by, bc, bi);
    if (count > g_dm.config.max_targets) {
        count = g_dm.config.max_targets;
    }

    /* 2) Fast path per target: detection consumer (tracking/drawing state
     * updates immediately), then - only when it asks for recognition -
     * crop+letterbox straight into a job slot for the worker. The PIPE2
     * buffer is held for a few ms per pass, never for a Model B inference. */
    uint32_t submitted = 0;
    for (uint32_t i = 0; i < count; i++) {
        float ex = bx[i], ey = by[i], ew = bw[i], eh = bh[i];
        dm_expand_box(&ex, &ey, &ew, &eh, g_dm.config.crop_expand_pm);
        if (ew <= 0.0f || eh <= 0.0f) {
            continue;
        }

        dual_model_target_t desc;
        desc.frame_id = frame_id;
        desc.x = ex;
        desc.y = ey;
        desc.w = ew;
        desc.h = eh;
        desc.det_conf = bc[i];
        desc.b_result = NULL;
        desc.b_infer_ms = 0;
        desc.n_kpts = bkn[i];
        if (bkn[i] > 0) {
            memcpy(desc.kpts, bk[i], sizeof(float) * 2u * bkn[i]);
        }
        (void)bi[i];

        intptr_t tag = 0;
        aicam_bool_t submit = AICAM_TRUE;
        if (g_dm.target_cb) {
            /* Consumer contract: quick, non-blocking, must not call back
             * into this module (we hold the mutex). */
            submit = g_dm.target_cb(&desc, g_dm.target_cb_user_data, &tag);
        }
        if (!submit) {
            continue;
        }

        uint32_t cx = (uint32_t)(ex * (float)pw + 0.5f);
        uint32_t cy = (uint32_t)(ey * (float)ph + 0.5f);
        uint32_t cw = (uint32_t)(ew * (float)pw + 0.5f);
        uint32_t ch = (uint32_t)(eh * (float)ph + 0.5f);
        if (cx + cw > pw) cw = pw - cx;
        if (cy + ch > ph) ch = ph - cy;
        if (cw == 0 || ch == 0) {
            continue;
        }

        dm_job_t *job = dm_claim_job_slot(&g_dm, tag);
        if (!job) {
            /* All slots busy with other faces: report the drop so the
             * consumer can release any per-track pending state. */
            g_dm.status.jobs_dropped++;
            if (g_dm.consumer) {
                g_dm.consumer(&desc, tag, g_dm.consumer_user_data);
            }
            continue;
        }

        if (desc.n_kpts >= 5) {
            /* High-res warp from PIPE1 (1280x720 RGB565): the landmarks map
             * through the view-rect into PIPE1 space, giving the embedding
             * network 4-8x more source pixels per face than PIPE2. Fall back
             * to the PIPE2 warp when PIPE1 is momentarily unavailable. */
            device_t *cam = device_find_pattern(CAMERA_DEVICE_NAME, DEV_TYPE_VIDEO);
            camera_buffer_with_frame_id_t p1 = {0};
            /* Acquire PIPE1 with ownership (torn-read safe): the buffer
             * count is now 4 (encoder + draw + this warp + 1 ISR spare),
             * so the acquire/return model no longer starves the pipeline
             * the way it did with 3 buffers. */
            if (cam &&
                device_ioctl(cam, CAM_CMD_GET_PIPE1_BUFFER_WITH_FRAME_ID,
                             (uint8_t *)&p1, sizeof(p1)) == AICAM_OK &&
                p1.buffer != NULL) {
                const uint32_t p1_pixels = p1.size / 2u; /* RGB565 = 2 B/px */
                const uint32_t p1_w = 1280u; /* PIPE1 fixed at 720p */
                const uint32_t p1_h = p1_pixels / p1_w;
                dm_align_crop_pipe1(p1.buffer, p1_w, p1_h,
                                     desc.kpts, job->crop, g_dm.input_w, g_dm.input_h);
                device_ioctl(cam, CAM_CMD_RETURN_PIPE1_BUFFER, p1.buffer, 0);
            } else {
                dm_align_crop_rgb888(pipe2_buffer, pw, ph, desc.kpts,
                                     job->crop, g_dm.input_w, g_dm.input_h);
            }

        } else {
            dm_letterbox_rgb888(pipe2_buffer, pw, ph, cx, cy, cw, ch,
                                job->crop, g_dm.input_w, g_dm.input_h,
                                g_dm.config.pad_value);
        }
        job->tag = tag;
        job->frame_id = frame_id;
        job->x = ex;
        job->y = ey;
        job->w = ew;
        job->h = eh;
        job->det_conf = bc[i];
        job->state = DM_JOB_PENDING;
        submitted++;
        osSemaphoreRelease(g_dm.job_sem);
    }
    g_dm.status.passes++;
    g_dm.status.last_targets = submitted;
    g_dm.status.last_pass_ms = osKernelGetTickCount() - pass_start;
    g_dm.status.last_run_tick = now;

    osMutexRelease(g_dm.mutex);
}

/* ==================== Model B Worker ==================== */

/**
 * @brief Model B worker: runs one queued crop at a time
 * @details Only this thread touches the Model B instance for inference
 *          (under the engine mutex, so reload/stop stays safe). The result
 *          consumer is called here with the tag attached at submit time.
 */
static void dm_worker_task(void *arg)
{
    (void)arg;
    while (g_dm.worker_run) {
        if (osSemaphoreAcquire(g_dm.job_sem, 200) != osOK) {
            continue; /* periodic wakeup to re-check worker_run */
        }
        if (!g_dm.worker_run) {
            break;
        }

        dm_job_t *job = NULL;
        if (osMutexAcquire(g_dm.mutex, 500) != osOK) {
            /* Couldn't get the engine: re-post so the job is retried */
            osSemaphoreRelease(g_dm.job_sem);
            osDelay(10);
            continue;
        }

        for (uint32_t i = 0; i < DM_JOB_SLOTS; i++) {
            if (g_dm.jobs[i].state == DM_JOB_PENDING) {
                job = &g_dm.jobs[i];
                job->state = DM_JOB_RUNNING;
                break;
            }
        }
        if (!job || !g_dm.model_b) {
            if (!job) {
                /* Spurious wakeup (slot was re-claimed) - swallow one token */
            }
            osMutexRelease(g_dm.mutex);
            continue;
        }

        uint32_t infer_start = osKernelGetTickCount();
        int ret = nn_instance_inference_frame(g_dm.model_b, job->crop,
                                              g_dm.input_size, &job->b_result);
        job->b_infer_ms = osKernelGetTickCount() - infer_start;

        if (ret == 0) {
            g_dm.status.inferences++;
            g_dm.consecutive_errors = 0;
            if (g_dm.status.avg_infer_ms == 0) {
                g_dm.status.avg_infer_ms = job->b_infer_ms;
            } else {
                g_dm.status.avg_infer_ms =
                    (g_dm.status.avg_infer_ms * 31u + job->b_infer_ms) / 32u;
            }
        } else {
            g_dm.status.errors++;
            g_dm.consecutive_errors++;
        }

        if (g_dm.consumer) {
            dual_model_target_t desc;
            desc.frame_id = job->frame_id;
            desc.x = job->x;
            desc.y = job->y;
            desc.w = job->w;
            desc.h = job->h;
            desc.det_conf = job->det_conf;
            desc.b_result = (ret == 0) ? &job->b_result : NULL;
            desc.b_infer_ms = job->b_infer_ms;
            g_dm.consumer(&desc, job->tag, g_dm.consumer_user_data);
        }

        job->state = DM_JOB_FREE;

        /* Fail-safe: repeated Model B errors latch the engine off */
        if (ret != 0 && g_dm.consecutive_errors >= DM_ERROR_LATCH_THRESHOLD) {
            g_dm.status.disabled_by_errors = AICAM_TRUE;
            LOG_CORE_ERROR("DualModel: Model B failed %lu consecutive inferences, "
                           "latched off until reload",
                           (unsigned long)g_dm.consecutive_errors);
        }

        osMutexRelease(g_dm.mutex);
    }
}

/* ==================== Init / Deinit ==================== */

aicam_result_t video_dual_model_init(void)
{
    if (g_dm.initialized) {
        return AICAM_OK;
    }

    g_dm.mutex = osMutexNew(NULL);
    if (!g_dm.mutex) {
        LOG_CORE_ERROR("DualModel: mutex creation failed");
        return AICAM_ERROR;
    }

    video_dual_model_get_default_config(&g_dm.config);
    if (json_config_get_dual_model_config(&g_dm.config) == AICAM_OK) {
        LOG_CORE_INFO("DualModel: configuration loaded from NVS");
    }
    g_dm.status.enabled = g_dm.config.enabled;

    /* Model B worker: created once at engine init, sleeps on the job
     * semaphore until a model is loaded and crops are submitted. */
    g_dm.job_sem = osSemaphoreNew(DM_JOB_SLOTS, 0, NULL);
    if (!g_dm.job_sem) {
        LOG_CORE_ERROR("DualModel: job semaphore creation failed");
        return AICAM_ERROR;
    }
    g_dm.worker_run = AICAM_TRUE;
    osThreadAttr_t worker_attr = {
        .name = "DmBWorker",
        .stack_mem = dm_worker_stack,
        .stack_size = sizeof(dm_worker_stack),
        .priority = osPriorityNormal,
    };
    g_dm.worker = osThreadNew(dm_worker_task, NULL, &worker_attr);
    if (!g_dm.worker) {
        LOG_CORE_ERROR("DualModel: worker thread creation failed");
        osSemaphoreDelete(g_dm.job_sem);
        g_dm.job_sem = NULL;
        g_dm.worker_run = AICAM_FALSE;
        return AICAM_ERROR;
    }

    g_dm.initialized = AICAM_TRUE;
    LOG_CORE_INFO("DualModel: engine initialized (enabled=%d)",
                  g_dm.config.enabled ? 1 : 0);
    return AICAM_OK;
}

aicam_result_t video_dual_model_deinit(void)
{
    if (!g_dm.initialized) {
        return AICAM_OK;
    }

    /* Stop the worker first: flag off + wake + join window, so no result
     * consumer fires while we tear the model down. */
    g_dm.worker_run = AICAM_FALSE;
    if (g_dm.job_sem) {
        osSemaphoreRelease(g_dm.job_sem);
    }
    for (uint32_t waited_ms = 0; g_dm.worker && waited_ms < 1000; waited_ms += 50) {
        osDelay(50);
    }
    if (g_dm.worker) {
        osThreadTerminate(g_dm.worker);
        g_dm.worker = NULL;
    }

    if (osMutexAcquire(g_dm.mutex, 2000) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    dm_release_model_locked(&g_dm);
    osMutexRelease(g_dm.mutex);

    g_dm.initialized = AICAM_FALSE;
    if (g_dm.job_sem) {
        osSemaphoreDelete(g_dm.job_sem);
        g_dm.job_sem = NULL;
    }
    if (g_dm.mutex) {
        osMutexDelete(g_dm.mutex);
        g_dm.mutex = NULL;
    }
    return AICAM_OK;
}

/* ==================== Config / Status ==================== */

aicam_result_t video_dual_model_get_config(dual_model_config_t *config)
{
    if (!config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_dm.initialized) {
        video_dual_model_get_default_config(config);
        return AICAM_OK;
    }
    if (osMutexAcquire(g_dm.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    *config = g_dm.config;
    osMutexRelease(g_dm.mutex);
    return AICAM_OK;
}

aicam_result_t video_dual_model_set_config(const dual_model_config_t *config)
{
    if (!config || !g_dm.initialized) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    if (osMutexAcquire(g_dm.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    g_dm.config = *config;
    g_dm.status.enabled = config->enabled;
    osMutexRelease(g_dm.mutex);
    return AICAM_OK;
}

void video_dual_model_get_default_config(dual_model_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->enabled = AICAM_TRUE;
    config->target_conf_threshold = 60u;     /* x100 */
    config->target_class[0] = '\0';          /* any class */
    config->crop_expand_pm = 200u;           /* +20% per side */
    config->crop_min_size = 24u;             /* px in Model A space */
    config->max_targets = 2u;
    config->interval_ms = 200u;
    config->pad_value = 127u;                /* neutral gray */
}

aicam_result_t video_dual_model_get_status(dual_model_status_t *status)
{
    if (!status) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_dm.initialized) {
        memset(status, 0, sizeof(*status));
        return AICAM_OK;
    }
    if (osMutexAcquire(g_dm.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    *status = g_dm.status;
    osMutexRelease(g_dm.mutex);
    return AICAM_OK;
}

int video_dual_model_model_b_loaded(void)
{
    /* Single aligned word read; written only under the engine mutex.
     * Uninitialized engine = not loaded. */
    return (g_dm.initialized && g_dm.status.model_b_loaded == AICAM_TRUE) ? 1 : 0;
}

int video_dual_model_is_enabled(void)
{
    return (g_dm.initialized && g_dm.config.enabled == AICAM_TRUE) ? 1 : 0;
}

aicam_result_t video_dual_model_reset_stats(void)
{
    if (!g_dm.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_dm.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    memset(&g_dm.status.passes, 0,
           sizeof(dual_model_status_t) - offsetof(dual_model_status_t, passes));
    g_dm.consecutive_errors = 0;
    osMutexRelease(g_dm.mutex);
    return AICAM_OK;
}

aicam_result_t video_dual_model_set_target_consumer(dual_model_target_cb_t cb,
                                                    void *user_data)
{
    if (!g_dm.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_dm.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    g_dm.target_cb = cb;
    g_dm.target_cb_user_data = user_data;
    osMutexRelease(g_dm.mutex);
    return AICAM_OK;
}

aicam_result_t video_dual_model_set_result_consumer(dual_model_result_cb_t cb,
                                                    void *user_data)
{
    if (!g_dm.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_dm.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    g_dm.consumer = cb;
    g_dm.consumer_user_data = user_data;
    osMutexRelease(g_dm.mutex);
    return AICAM_OK;
}
