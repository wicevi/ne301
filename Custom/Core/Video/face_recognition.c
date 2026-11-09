/**
 * @file face_recognition.c
 * @brief Local face recognition demo implementation
 * @details Consumes Model B face embeddings from the generic dual-model
 *          engine, matches them against the local template database with
 *          cosine similarity, confirms identities through per-track voting
 *          and burns the verdict into the encoded stream. See
 *          face_recognition.h for the threading contract.
 *
 *          Lock order (never violated): dual-model engine mutex ->
 *          face_recognition mutex -> face_db mutex.
 */

#include "face_recognition.h"
#include "video_dual_model.h"
#include "camera.h"
#include "buffer_mgr.h"
#include "debug.h"
#include "dev_manager.h"
#include "draw.h"
#include "fonts.h"
#include "mem.h"
#include "pixel_format_map.h"
#include "json_config_mgr.h"
#include "cmsis_os2.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ==================== Internal Definitions ==================== */

#define FR_LABEL_MAX 48

/* RGB565 overlay colors */
#define FR_COLOR_KNOWN       0x07E0u  /* green */
#define FR_COLOR_UNKNOWN     0xC618u  /* gray */
#define FR_COLOR_IDENTIFYING 0xFD20u  /* orange */
#define FR_COLOR_LABEL_BG    0x0000u  /* black */
#define FR_LABEL_ALPHA       128u

typedef struct {
    aicam_bool_t in_use;
    aicam_bool_t confirmed;      /* past confirm_frames hits */
    uint32_t id;
    uint32_t hits;
    uint32_t missed;
    float x, y, w, h;            /* normalized, Model A input space */
    float det_conf;
    uint32_t last_update;        /* kernel tick */

    /* Identity */
    fr_ident_status_t status;
    char name[FACE_DB_NAME_MAX];
    uint32_t db_id;
    float similarity;

    /* Voting (candidate identity awaiting confirmation) */
    uint32_t vote_db_id;         /* 0 = candidate "unknown" */
    char vote_name[FACE_DB_NAME_MAX];
    uint32_t vote_count;

    /* Model B job gating (async pipeline): one in-flight crop per track */
    aicam_bool_t infer_pending;  /* a crop for this track is queued/running */
    uint32_t infer_tick;         /* when it was submitted (staleness guard) */
    uint32_t last_identity_tick; /* last completed Model B result */

    /* Identity demotion streak: the async pipeline delivers results from
     * crops taken 200-500 ms ago - a single stale low-similarity result
     * (subject turned head when the crop was taken) must not demote a
     * confirmed identity. Only N consecutive low results do. */
    uint32_t low_sim_streak;

    /* Frontal-pose state of the latest detection (from the 5 landmarks) */
    aicam_bool_t frontal;
    float pose_score;            /* 0..1, 1 = perfectly frontal */

    /* Landmark smoothing (EMA): YuNet landmarks jitter ±2-4 px frame to
     * frame; ~15% of frames jump far enough to misalign the ArcFace warp
     * (similarity drops from 0.8 to 0.2). The EMA filters those spikes
     * while tracking real movement. The smoothed values are written back
     * into the dual_model target's kpts so the warp uses them. */
    aicam_bool_t kpts_valid;
    float smooth_kpts[8][2];
} fr_track_t;

typedef struct {
    aicam_bool_t initialized;
    osMutexId_t mutex;

    face_recognition_config_t config;
    fr_stats_t stats;
    fr_track_t tracks[FR_MAX_TRACKS];
    uint32_t total_tracks;
    uint32_t last_pass_frame;    /* dual-model frame id of the last pass */
    uint32_t last_pass_tick;     /* kernel tick of the last pass start */
    uint32_t pass_interval_ms;   /* consumer-maintained estimate (~200-600ms) */

    /* Consumer diagnostics ring (exposed via /face-recognition/status) */
#define FR_DBG_RING 8
#define FR_DBG_TRACKS 3
    struct {
        uint32_t seq;            /* consumer call counter */
        uint32_t frame_id;
        float tx, ty, tw, th;    /* target box (normalized) */
        float mx, my, mw, mh;    /* matched/created track box after update */
        float best_iou;
        int32_t matched_track;   /* track id matched, 0 = created new, -1 = slots full, -2 = rejected (type/dim) */
        uint32_t n_tracks;       /* in_use tracks after the update */
        uint32_t first_of_pass;
        /* Live tracks as seen at entry (first FR_DBG_TRACKS, by slot) */
        struct {
            int32_t id;          /* -1 = slot empty */
            uint32_t hits;
            float x, y, w, h;
            uint32_t age_ms;     /* now - last_update */
        } live[FR_DBG_TRACKS];
    } dbg[FR_DBG_RING];
    uint32_t dbg_idx;
    uint32_t dbg_seq;

    /* Last track-expire decision (field-service aid) */
    struct {
        uint32_t cleared_id;    /* 0 = never cleared */
        uint32_t age;
        uint32_t lifetime;
        uint32_t tick;
    } last_expire_dbg;

    /* Probe scratch (consumer thread, guarded by mutex; avoids 1KB stack) */
    float probe[FACE_DB_EMBED_DIM_MAX];

    /* Enrollment state machine */
    struct {
        fr_enroll_state_t state;
        char name[FACE_DB_NAME_MAX];
        uint32_t samples_needed;
        uint32_t samples_collected;
        uint32_t dim;
        float *samples;          /* samples_needed * dim floats */
        uint32_t start_tick;
        uint32_t new_id;         /* Set when state == FR_ENROLL_DONE */
        aicam_bool_t capture_armed; /* manual mode: grab ONE frontal sample */
    } enroll;

    /* Drawing (camera node thread, lazy init) */
    device_t *draw_dev;
    DRAW_Font_t font16;
    aicam_bool_t font_ready;
} face_recognition_ctx_t;

static face_recognition_ctx_t g_fr = {0};

/* Lock-free liveness flag for the camera draw path: when the demo owns the
 * overlay, ai_service skips drawing the raw Model A boxes (they would
 * duplicate the recognition boxes). Written only on init/config change.
 * The Model B probe in face_recognition_owns_overlay() is live: with no
 * usable Model B (missing package or a load-contract mismatch) the demo
 * cannot draw anything, so ownership falls back to ai_service and the raw
 * Model A boxes keep rendering (single-model degradation). */
static volatile aicam_bool_t g_fr_overlay_active = AICAM_FALSE;

static void fr_update_overlay_flag(void)
{
    g_fr_overlay_active = (g_fr.initialized && g_fr.config.enabled && g_fr.config.show_box)
                              ? AICAM_TRUE
                              : AICAM_FALSE;
}

/* ==================== Internal Function Declarations ==================== */

static float fr_box_iou(const fr_track_t *t, float x, float y, float w, float h);
static fr_track_t *fr_track_create(face_recognition_ctx_t *ctx);
static void fr_expire_tracks(face_recognition_ctx_t *ctx, uint32_t now,
                             uint32_t interval_ms);
static void fr_check_enroll_timeout(face_recognition_ctx_t *ctx, uint32_t now);
static void fr_finish_enroll(face_recognition_ctx_t *ctx);
static aicam_bool_t fr_name_is_ascii(const char *s);
static void fr_draw_rect_hw(face_recognition_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, int w, int h, uint32_t thickness, uint32_t color);
static void fr_draw_text_hw(face_recognition_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, const char *str);
static void fr_blend_rect_hw(face_recognition_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                             int x, int y, int w, int h, uint32_t color, uint8_t alpha);
static uint32_t fr_font_width(face_recognition_ctx_t *ctx, const char *str);
static aicam_bool_t fr_lazy_draw_init(face_recognition_ctx_t *ctx);

/* ==================== Tracker Helpers ==================== */

static float fr_box_iou(const fr_track_t *t, float x, float y, float w, float h)
{
    float ix = (t->x > x) ? t->x : x;
    float iy = (t->y > y) ? t->y : y;
    float ix2 = (t->x + t->w < x + w) ? t->x + t->w : x + w;
    float iy2 = (t->y + t->h < y + h) ? t->y + t->h : y + h;
    float iw = ix2 - ix;
    float ih = iy2 - iy;
    if (iw <= 0.0f || ih <= 0.0f) {
        return 0.0f;
    }
    float inter = iw * ih;
    float uni = t->w * t->h + w * h - inter;
    return (uni > 0.0f) ? inter / uni : 0.0f;
}

static fr_track_t *fr_track_create(face_recognition_ctx_t *ctx)
{
    for (uint32_t i = 0; i < FR_MAX_TRACKS; i++) {
        if (!ctx->tracks[i].in_use) {
            fr_track_t *t = &ctx->tracks[i];
            memset(t, 0, sizeof(*t));
            t->in_use = AICAM_TRUE;
            t->id = ++ctx->total_tracks;
            return t;
        }
    }
    return NULL;
}

/* Time-based aging: unlike per-frame trackers we age by wall time because
 * passes without targets never reach the consumer. Callers MUST pass a
 * `now` captured AFTER acquiring ctx->mutex: capturing it earlier lets an
 * in-flight consumer store last_update > now, and the unsigned subtraction
 * wraps to ~49 days, instantly expiring every fresh track. */
static void fr_expire_tracks(face_recognition_ctx_t *ctx, uint32_t now,
                             uint32_t interval_ms)
{
    uint32_t lifetime = ctx->config.face_hold_ms +
                        ctx->config.track_max_missed * (interval_ms ? interval_ms : 200u);
    /* A live face updates every few hundred ms; anything below this is a
     * misconfiguration that would expire tracks between passes. */
    if (lifetime < 800u) {
        lifetime = 800u;
    }
    /* Hard ceiling: however noisy the interval estimate got, a vanished
     * face's box must leave the screen within ~1.5 s. Users wanting faster
     * cleanup tune track_max_missed down (each unit ~ one pass). */
    if (lifetime > 1500u) {
        lifetime = 1500u;
    }
    for (uint32_t i = 0; i < FR_MAX_TRACKS; i++) {
        fr_track_t *t = &ctx->tracks[i];
        if (!t->in_use) {
            continue;
        }
        uint32_t age = now - t->last_update;
        if (age > 0x80000000u) {
            continue; /* wrapped: last_update ahead of now, keep the track */
        }
        if (age > lifetime) {
            ctx->last_expire_dbg.cleared_id = t->id;
            ctx->last_expire_dbg.age = age;
            ctx->last_expire_dbg.lifetime = lifetime;
            ctx->last_expire_dbg.tick = now;
            memset(t, 0, sizeof(*t));
        }
    }
}

static void fr_check_enroll_timeout(face_recognition_ctx_t *ctx, uint32_t now)
{
    if (ctx->enroll.state == FR_ENROLL_COLLECTING &&
        (now - ctx->enroll.start_tick) > ctx->config.enroll_timeout_ms) {
        if (ctx->enroll.samples_collected > 0) {
            /* Partial progress is NOT discarded: the user simply had too few
             * qualifying (frontal-ish) frames. Pause with the samples kept -
             * enroll_capture() resumes the window, enroll_finish() can use
             * what is already there. Only a completely empty window fails. */
            ctx->enroll.state = FR_ENROLL_PAUSED;
            LOG_CORE_WARN("FaceRec: enrollment window expired with partial progress "
                          "(%lu/%lu samples) - paused, waiting for the user",
                          (unsigned long)ctx->enroll.samples_collected,
                          (unsigned long)ctx->enroll.samples_needed);
        } else {
            if (ctx->enroll.samples) {
                hal_mem_free(ctx->enroll.samples);
                ctx->enroll.samples = NULL;
            }
            ctx->enroll.state = FR_ENROLL_FAILED_NO_FACE;
            LOG_CORE_WARN("FaceRec: enrollment window expired without any sample");
        }
    }
}

/* ==================== Enrollment ==================== */

static void fr_finish_enroll(face_recognition_ctx_t *ctx)
{
    uint32_t dim = ctx->enroll.dim;
    uint32_t n = ctx->enroll.samples_collected;

    /* Runs on the AI node thread: keep the 1KB mean off the stack */
    float *mean = hal_mem_alloc_any(sizeof(float) * FACE_DB_EMBED_DIM_MAX);
    if (!mean) {
        hal_mem_free(ctx->enroll.samples);
        ctx->enroll.samples = NULL;
        ctx->enroll.state = FR_ENROLL_FAILED_DB;
        return;
    }
    face_db_average_templates(dim, ctx->enroll.samples, n, mean);

    /* Outlier trim: with the relaxed (non-strictly-frontal) sampling gate a
     * session can pick up a strongly turned head whose embedding drags the
     * average away from the frontal probe - matched similarity then hugs the
     * threshold and recognition flips to unknown. Drop the worst-agreeing
     * sample once (keeping >= 2) when it is clearly off, then re-average. */
    if (n > 2u) {
        uint32_t worst = 0;
        float worst_sim = 2.0f;
        for (uint32_t s = 0; s < n; s++) {
            float dot = 0.0f;
            const float *v = ctx->enroll.samples + (size_t)s * dim;
            for (uint32_t k = 0; k < dim; k++) {
                dot += mean[k] * v[k];
            }
            if (dot < worst_sim) {
                worst_sim = dot;
                worst = s;
            }
        }
        /* Only trim a clear outlier, not natural sample spread. */
        if (worst_sim < 0.80f) {
            float *dst = ctx->enroll.samples + (size_t)worst * dim;
            const float *src = ctx->enroll.samples + (size_t)(worst + 1u) * dim;
            memmove(dst, src, sizeof(float) * dim * (n - worst - 1u));
            n--;
            ctx->enroll.samples_collected = n;
            face_db_average_templates(dim, ctx->enroll.samples, n, mean);
            LOG_CORE_INFO("FaceRec: enrollment trimmed one outlier sample "
                          "(sim %d%%), re-averaged over %lu",
                          (int)(worst_sim * 100.0f), (unsigned long)n);
        }
    }

    /* Sanity: samples must agree with their mean template, otherwise the
     * capture was inconsistent (face moved out, two faces interleaved) */
    float sim_sum = 0.0f;
    for (uint32_t s = 0; s < n; s++) {
        float dot = 0.0f;
        const float *v = ctx->enroll.samples + (size_t)s * dim;
        for (uint32_t k = 0; k < dim; k++) {
            dot += mean[k] * v[k];
        }
        sim_sum += dot;
    }
    float mean_sim = sim_sum / (float)n;

    uint32_t new_id = 0;
    aicam_result_t ret;
    if (mean_sim * 100.0f < (float)ctx->config.enroll_min_similarity) {
        ret = AICAM_ERROR_INVALID_DATA;
    } else {
        ret = face_db_add(ctx->enroll.name, dim, mean, &new_id);
    }
    hal_mem_free(mean);

    hal_mem_free(ctx->enroll.samples);
    ctx->enroll.samples = NULL;

    if (ret == AICAM_OK) {
        ctx->enroll.state = FR_ENROLL_DONE;
        ctx->enroll.new_id = new_id;
        LOG_CORE_INFO("FaceRec: enrolled '%s' (id %lu, %lu samples, mean sim %d%%)",
                     ctx->enroll.name, (unsigned long)new_id,
                     (unsigned long)n, (int)(mean_sim * 100.0f));
    } else if (ret == AICAM_ERROR_INVALID_DATA) {
        ctx->enroll.state = FR_ENROLL_FAILED_INCONSISTENT;
        LOG_CORE_WARN("FaceRec: enrollment samples inconsistent (mean sim %d%% < %lu%%)",
                     (int)(mean_sim * 100.0f),
                     (unsigned long)ctx->config.enroll_min_similarity);
    } else {
        ctx->enroll.state = FR_ENROLL_FAILED_DB;
        LOG_CORE_ERROR("FaceRec: enrollment DB write failed (%d)", ret);
    }
}

/* Font16 is an ASCII bitmap font: keep labels renderable */
static aicam_bool_t fr_name_is_ascii(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80) {
            return AICAM_FALSE;
        }
    }
    return AICAM_TRUE;
}

/* ==================== Dual-Model Consumer ==================== */

/* Enqueue policy constants (per-track gating so the engine never queues a
 * backlog of stale crops): identifying faces run continuously, known faces
 * refresh their identity every few seconds, unknown faces retry slower. */
#define FR_RECOG_REFRESH_IDENTIFYING_MS 0u
#define FR_RECOG_REFRESH_KNOWN_MS 1000u
#define FR_RECOG_REFRESH_UNKNOWN_MS 1000u
#define FR_INFER_PENDING_TIMEOUT_MS 3000u

/* Frontal-pose gate thresholds. The yaw/roll limits are user-configurable
 * (face_recognition_config_t pose_max_yaw / pose_max_roll); these constants
 * cover the fixed guards. Geometry: the nose tip protrudes ~0.6x eye_dist
 * ahead of the eye plane, so yaw ratio ~= 0.6*sin(head turn). */
#define FR_POSE_MIN_EYE_DIST      0.02f  /* ~6px at 320 input */
#define FR_POSE_MIN_EYE_FACE      0.14f  /* eye dist / box width (near-profile guard) */
#define FR_POSE_NOSE_RANGE_LO     0.30f  /* (nose.y - eye mid.y) / eye dist */
#define FR_POSE_NOSE_RANGE_HI     1.25f
#define FR_POSE_MAX_MOUTH_OFFSET  0.45f  /* |mouth mid.x - nose.x| / eye dist */

/**
 * @brief Frontal-pose check from the 5 YuNet landmarks
 * @details kpts order: right_eye, left_eye, nose, right_mouth, left_mouth
 *          (normalized). Returns a 0..1 quality score and sets *frontal -
 *          thresholds are generous (recognition models tolerate some yaw),
 *          the goal is to reject明显的侧脸/低头/仰头.
 */
static float fr_pose_score(const float kpts[][2], uint32_t n,
                           float box_w, uint32_t yaw_x100, uint32_t roll_deg,
                           aicam_bool_t *frontal)
{
    *frontal = AICAM_FALSE;
    if (n < 5 || !kpts) {
        return 0.0f; /* no landmarks -> not provably frontal */
    }
    const float max_yaw = (float)(yaw_x100 ? yaw_x100 : 60u) / 100.0f;
    const float max_roll = (float)(roll_deg ? roll_deg : 40u);

    const float *re = kpts[0];
    const float *le = kpts[1];
    const float *no = kpts[2];
    const float *rm = kpts[3];
    const float *lm = kpts[4];

    float edx = le[0] - re[0];
    float edy = le[1] - re[1];
    float eye_dist = sqrtf(edx * edx + edy * edy);
    if (eye_dist < FR_POSE_MIN_EYE_DIST) {
        return 0.0f;
    }

    float worst = 1.0f;

    /* Roll: eye-line tilt (mandatory) */
    float roll = fabsf(atan2f(edy, edx) * 57.29578f);
    if (roll > 90.0f) {
        roll = 180.0f - roll;
    }
    aicam_bool_t roll_ok = (roll < max_roll) ? AICAM_TRUE : AICAM_FALSE;
    if (worst > roll / max_roll) {
        worst = roll / max_roll;
    }

    /* Yaw: nose horizontal offset from the eye midpoint (mandatory) */
    float eye_mid_x = (re[0] + le[0]) * 0.5f;
    float yaw = fabsf(no[0] - eye_mid_x) / eye_dist;
    aicam_bool_t yaw_ok = (yaw < max_yaw) ? AICAM_TRUE : AICAM_FALSE;
    if (worst > yaw / max_yaw) {
        worst = yaw / max_yaw;
    }

    /* Near-profile guard: eyes compress as the head turns (mandatory) */
    float eye_face = eye_dist / (box_w > 0.0f ? box_w : 1.0f);
    aicam_bool_t face_ok = (eye_face > FR_POSE_MIN_EYE_FACE) ? AICAM_TRUE : AICAM_FALSE;
    if (worst > FR_POSE_MIN_EYE_FACE / (eye_face + 1e-6f)) {
        worst = FR_POSE_MIN_EYE_FACE / (eye_face + 1e-6f);
    }

    /* Pitch: nose vertical position between eyes and mouth (advisory) */
    float eye_mid_y = (re[1] + le[1]) * 0.5f;
    float nose_v = (no[1] - eye_mid_y) / eye_dist;
    aicam_bool_t nose_ok =
        (nose_v > FR_POSE_NOSE_RANGE_LO && nose_v < FR_POSE_NOSE_RANGE_HI)
            ? AICAM_TRUE : AICAM_FALSE;

    /* Mouth symmetric below the nose (advisory) */
    float mouth_mid_x = (rm[0] + lm[0]) * 0.5f;
    float mouth_off = fabsf(mouth_mid_x - no[0]) / eye_dist;
    aicam_bool_t mouth_ok = (mouth_off < FR_POSE_MAX_MOUTH_OFFSET) ? AICAM_TRUE : AICAM_FALSE;

    /* The three horizontal checks (roll/yaw/profile) are mandatory; at least
     * one of the two vertical ones must hold, so a single landmark outlier
     * (chin height, mouth corners) cannot fail an otherwise frontal face. */
    *frontal = (roll_ok && yaw_ok && face_ok && (nose_ok || mouth_ok))
                   ? AICAM_TRUE
                   : AICAM_FALSE;
    return 1.0f - (worst > 1.0f ? 1.0f : worst);
}

/**
 * @brief Detection-stage consumer (AI node thread, pre-Model B)
 * @details Updates tracking/drawing state immediately (boxes follow at pass
 *          cadence) and decides whether THIS crop should go to Model B.
 *          Gate: only confirmed tracks, never while a previous job for the
 *          same track is still in flight, and at most at the identity
 *          refresh rate. Enrollment overrides the policy (tag 0 = enroll).
 */
aicam_bool_t face_recognition_on_target(const dual_model_target_t *target,
                                        void *user_data,
                                        intptr_t *out_tag)
{
    (void)user_data;
    if (!g_fr.initialized || !target) {
        return AICAM_FALSE;
    }

    if (osMutexAcquire(g_fr.mutex, 100) != osOK) {
        return AICAM_FALSE;
    }

    face_recognition_config_t *cfg = &g_fr.config;
    if (!cfg->enabled) {
        osMutexRelease(g_fr.mutex);
        return AICAM_FALSE;
    }

    /* now AFTER the lock - see fr_expire_tracks (same wrap hazard) */
    uint32_t now = osKernelGetTickCount();

    /* Pass boundary: the engine delivers targets of one pass largest-first,
     * so the first target of a frame is the biggest face on screen. */
    aicam_bool_t first_of_pass = (target->frame_id != g_fr.last_pass_frame);
    g_fr.last_pass_frame = target->frame_id;

    /* Maintain a pass-interval estimate (sliding average over pass starts):
     * read paths use it for aging instead of querying the engine - that
     * query would block the camera thread on the engine mutex. */
    if (first_of_pass) {
        if (g_fr.last_pass_tick != 0) {
            uint32_t dt = now - g_fr.last_pass_tick;
            /* Clamp the sample: a long idle gap (no faces -> no passes, or a
             * face flickering at the frame edge) is not pass cadence. Without
             * the clamp one 30s gap inflates the estimate and the track
             * lifetime (= hold + misses * interval) grows to tens of seconds
             * - a vanished face then keeps its stale "unknown" box on screen
             * almost forever. 400ms keeps the estimate near the real pass
             * cadence (~200ms) while tolerating a dropped pass. */
            if (dt > 400u) {
                dt = 400u;
            }
            g_fr.pass_interval_ms = (g_fr.pass_interval_ms * 3u + dt) / 4u;
        }
        g_fr.last_pass_tick = now;
    }

    fr_check_enroll_timeout(&g_fr, now);

    /* Diagnostics ring */
    uint32_t dbg_slot = g_fr.dbg_idx % FR_DBG_RING;
    g_fr.dbg_idx++;
    g_fr.dbg[dbg_slot].seq = ++g_fr.dbg_seq;
    g_fr.dbg[dbg_slot].frame_id = target->frame_id;
    g_fr.dbg[dbg_slot].tx = target->x;
    g_fr.dbg[dbg_slot].ty = target->y;
    g_fr.dbg[dbg_slot].tw = target->w;
    g_fr.dbg[dbg_slot].th = target->h;
    g_fr.dbg[dbg_slot].matched_track = -2; /* overwritten below on accept */
    g_fr.dbg[dbg_slot].first_of_pass = first_of_pass;
    {
        uint32_t seen = 0;
        for (uint32_t i = 0; i < FR_MAX_TRACKS && seen < FR_DBG_TRACKS; i++) {
            if (g_fr.tracks[i].in_use) {
                g_fr.dbg[dbg_slot].live[seen].id = (int32_t)g_fr.tracks[i].id;
                g_fr.dbg[dbg_slot].live[seen].hits = g_fr.tracks[i].hits;
                g_fr.dbg[dbg_slot].live[seen].x = g_fr.tracks[i].x;
                g_fr.dbg[dbg_slot].live[seen].y = g_fr.tracks[i].y;
                g_fr.dbg[dbg_slot].live[seen].w = g_fr.tracks[i].w;
                g_fr.dbg[dbg_slot].live[seen].h = g_fr.tracks[i].h;
                g_fr.dbg[dbg_slot].live[seen].age_ms = now - g_fr.tracks[i].last_update;
                seen++;
            }
        }
        for (; seen < FR_DBG_TRACKS; seen++) {
            g_fr.dbg[dbg_slot].live[seen].id = -1;
        }
    }

    /* 1) Tracker update for this target's box.
     * Matching accepts either plain IoU or center-containment: passes land
     * a few hundred ms apart and a moving face can slide past the IoU gate
     * entirely, which used to spawn a fresh track (and a duplicate box) per
     * pass. */
    fr_track_t *best = NULL;
    float best_iou = 0.0f;
    float best_score = 0.0f;
    const float iou_min = (float)cfg->track_iou_threshold / 100.0f;
    const float cx = target->x + target->w * 0.5f;
    const float cy = target->y + target->h * 0.5f;
    for (uint32_t i = 0; i < FR_MAX_TRACKS; i++) {
        fr_track_t *t = &g_fr.tracks[i];
        if (!t->in_use) {
            continue;
        }
        float iou = fr_box_iou(t, target->x, target->y, target->w, target->h);
        float score = iou;
        if (cx >= t->x && cx <= t->x + t->w && cy >= t->y && cy <= t->y + t->h) {
            /* target center inside the track box: strong continuity signal */
            if (score < 0.5f) {
                score = 0.5f;
            }
        }
        if (score > best_score) {
            best_score = score;
            best_iou = iou;
            best = t;
        }
    }

    g_fr.dbg[dbg_slot].best_iou = best_score;

    if (best && best_score >= iou_min) {
        /* Smooth the box when the match is tight, snap when it moved hard */
        float blend = (best_iou >= 0.5f) ? 0.6f : 1.0f;
        best->x = best->x * (1.0f - blend) + target->x * blend;
        best->y = best->y * (1.0f - blend) + target->y * blend;
        best->w = best->w * (1.0f - blend) + target->w * blend;
        best->h = best->h * (1.0f - blend) + target->h * blend;
        best->det_conf = target->det_conf;
        best->missed = 0;
        best->hits++;
        g_fr.dbg[dbg_slot].matched_track = (int32_t)best->id;
    } else {
        best = fr_track_create(&g_fr);
        if (!best) {
            g_fr.dbg[dbg_slot].matched_track = -1;
            osMutexRelease(g_fr.mutex);
            return AICAM_FALSE;
        }
        best->x = target->x;
        best->y = target->y;
        best->w = target->w;
        best->h = target->h;
        best->det_conf = target->det_conf;
        best->hits = 1;
        best->status = FR_IDENT_IDENTIFYING;
        g_fr.dbg[dbg_slot].matched_track = 0;
    }
    best->last_update = now;
    if (!best->confirmed && best->hits >= 2u) {
        best->confirmed = AICAM_TRUE;
    }

    /* Landmark EMA smoothing: filter YuNet's frame-to-frame jitter before
     * the pose gate and (critically) before the dual-model warp. Raw
     * landmarks with ±4px spikes misalign the ArcFace warp on ~15% of
     * frames (similarity 0.8 -> 0.2). alpha=0.4 keeps 40% new / 60%
     * history: responsive to real head movement, immune to single-frame
     * detection spikes. The smoothed values are written BACK into the
     * target descriptor so dual_model warps from them. */
    if (target->n_kpts >= 5 && target->n_kpts <= 8) {
        const float alpha = 0.4f;
        if (!best->kpts_valid) {
            /* First detection for this track: adopt as-is */
            for (uint32_t k = 0; k < target->n_kpts; k++) {
                best->smooth_kpts[k][0] = target->kpts[k][0];
                best->smooth_kpts[k][1] = target->kpts[k][1];
            }
            best->kpts_valid = AICAM_TRUE;
        } else {
            for (uint32_t k = 0; k < target->n_kpts; k++) {
                best->smooth_kpts[k][0] += alpha * (target->kpts[k][0] - best->smooth_kpts[k][0]);
                best->smooth_kpts[k][1] += alpha * (target->kpts[k][1] - best->smooth_kpts[k][1]);
            }
        }
        /* Write smoothed landmarks back so the warp uses stable positions.
         * The const on the callback parameter is a read-only VIEW; the
         * dual_model engine passes its own mutable descriptor and expects
         * landmark refinement here. */
        dual_model_target_t *mut_target = (dual_model_target_t *)(uintptr_t)target;
        for (uint32_t k = 0; k < target->n_kpts; k++) {
            mut_target->kpts[k][0] = best->smooth_kpts[k][0];
            mut_target->kpts[k][1] = best->smooth_kpts[k][1];
        }
    }

    /* Frontal-pose gate: landmarks decide whether this detection is worth a
     * (relatively slow) recognition pass - boxes keep tracking regardless.
     * The yaw/roll tolerance is user-configurable.
     * Landmark y arrives in LETTERBOXED NN space (padded rows inflate the
     * vertical axis, ~0.57x for a 16:9 sensor and a square input): undo the
     * pad before the vertical ratios or every nose/mouth measurement reads
     * low and frontal faces fail the gate (enrollment then starves). */
    aicam_bool_t frontal = AICAM_FALSE;
    float pose;
    {
        float kpts_unlb[8][2];
        uint32_t n = target->n_kpts;
        if (n > 8) n = 8;
        for (uint32_t k = 0; k < n; k++) {
            kpts_unlb[k][0] = target->kpts[k][0];
            kpts_unlb[k][1] = target->kpts[k][1];
        }
        /* Letterbox y-unwrap: landmark y is in the padded NN space; undo
         * the pad before the vertical pose ratios. */
        {
            uint32_t model_h = 0, eff_h = 0, pad_top = 0;
            if (camera_get_pipe2_letterbox_geo(NULL, &model_h, &eff_h, &pad_top) == 0 &&
                model_h > 0 && eff_h > 0 && eff_h < model_h) {
                for (uint32_t k = 0; k < n; k++) {
                    kpts_unlb[k][1] =
                        (kpts_unlb[k][1] * (float)model_h - (float)pad_top) / (float)eff_h;
                }
            }
        }
        pose = fr_pose_score(kpts_unlb, target->n_kpts, best->w,
                             cfg->pose_max_yaw, cfg->pose_max_roll, &frontal);
    }
    best->frontal = frontal;
    best->pose_score = pose;

    g_fr.dbg[dbg_slot].mx = best->x;
    g_fr.dbg[dbg_slot].my = best->y;
    g_fr.dbg[dbg_slot].mw = best->w;
    g_fr.dbg[dbg_slot].mh = best->h;
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < FR_MAX_TRACKS; i++) {
            if (g_fr.tracks[i].in_use) {
                n++;
            }
        }
        g_fr.dbg[dbg_slot].n_tracks = n;
    }

    /* 2) Enqueue decision for Model B */
    aicam_bool_t submit = AICAM_FALSE;
    intptr_t tag = 0;

    if (g_fr.enroll.state == FR_ENROLL_COLLECTING) {
        /* Manual enrollment: one armed capture per press, largest face of
         * the pass. Pose gate is RELAXED vs live recognition (score floor
         * instead of strict frontal): users naturally turn their head while
         * sampling, the averaged template tolerates mild rotation - a strict
         * frontal-only gate starves the window whenever the face is not
         * perfectly frontal ("not enough face samples"). */
        if (g_fr.enroll.capture_armed && first_of_pass && (frontal || pose >= 0.35f)) {
            submit = AICAM_TRUE;
            tag = 0;
            g_fr.enroll.capture_armed = AICAM_FALSE; /* consumed */
        }
    } else if (best->confirmed && frontal) {
        /* A job that never completed (dropped / crash) must not wedge the
         * track's identity refresh forever */
        if (best->infer_pending && (now - best->infer_tick) > FR_INFER_PENDING_TIMEOUT_MS) {
            best->infer_pending = AICAM_FALSE;
        }
        if (!best->infer_pending) {
            uint32_t since = now - best->last_identity_tick;
            uint32_t refresh;
            switch (best->status) {
                case FR_IDENT_KNOWN:   refresh = FR_RECOG_REFRESH_KNOWN_MS; break;
                case FR_IDENT_UNKNOWN: refresh = FR_RECOG_REFRESH_UNKNOWN_MS; break;
                default:               refresh = FR_RECOG_REFRESH_IDENTIFYING_MS; break;
            }
            if (since >= refresh) {
                submit = AICAM_TRUE;
                tag = (intptr_t)best->id;
                best->infer_pending = AICAM_TRUE;
                best->infer_tick = now;
            }
        }
    }

    osMutexRelease(g_fr.mutex);
    if (submit && out_tag) {
        *out_tag = tag;
    }
    return submit;
}

/**
 * @brief Result-stage consumer (Model B worker thread)
 * @details Runs after each Model B inference (or on job drop, b_result ==
 *          NULL): routes the embedding to the enrollment sampler (tag 0) or
 *          updates the identity of the track the crop came from.
 */
void face_recognition_on_embed(const dual_model_target_t *target,
                               intptr_t tag,
                               void *user_data)
{
    (void)user_data;
    if (!g_fr.initialized || !target) {
        return;
    }

    if (osMutexAcquire(g_fr.mutex, 200) != osOK) {
        return;
    }

    face_recognition_config_t *cfg = &g_fr.config;
    uint32_t now = osKernelGetTickCount();

    /* Locate the destination track (enrollment uses the tag-0 channel) */
    fr_track_t *track = NULL;
    if (tag != 0) {
        for (uint32_t i = 0; i < FR_MAX_TRACKS; i++) {
            if (g_fr.tracks[i].in_use && g_fr.tracks[i].id == (uint32_t)tag) {
                track = &g_fr.tracks[i];
                break;
            }
        }
    }

    if (!target->b_result) {
        /* Job dropped (slot exhaustion) or inference failed: release the
         * per-track pending gate so the next pass can retry. */
        if (track) {
            track->infer_pending = AICAM_FALSE;
        }
        osMutexRelease(g_fr.mutex);
        return;
    }

    if (!cfg->enabled) {
        if (track) {
            track->infer_pending = AICAM_FALSE;
        }
        osMutexRelease(g_fr.mutex);
        return;
    }

    const nn_result_t *b = target->b_result;
    if (b->type != PP_TYPE_EMBEDDING || !b->is_valid) {
        if (track) {
            track->infer_pending = AICAM_FALSE;
        }
        osMutexRelease(g_fr.mutex);
        return;
    }
    const float *vec = b->embed.vec;
    uint32_t dim = b->embed.dim;
    if (!vec || dim == 0 || dim > FACE_DB_EMBED_DIM_MAX) {
        if (track) {
            track->infer_pending = AICAM_FALSE;
        }
        osMutexRelease(g_fr.mutex);
        return;
    }

    if (track) {
        track->infer_pending = AICAM_FALSE;
        track->last_identity_tick = now;
    }

    g_fr.stats.embed_count++;
    g_fr.stats.last_embed_ms = target->b_infer_ms;
    g_fr.stats.last_update_tick = now;

    /* Copy + (re)normalize the probe into our scratch */
    memcpy(g_fr.probe, vec, sizeof(float) * dim);
    if (!b->embed.normalized) {
        double norm2 = 0.0;
        for (uint32_t k = 0; k < dim; k++) {
            norm2 += (double)g_fr.probe[k] * (double)g_fr.probe[k];
        }
        float norm = (float)sqrt(norm2);
        if (norm > 1e-12f) {
            float inv = 1.0f / norm;
            for (uint32_t k = 0; k < dim; k++) {
                g_fr.probe[k] *= inv;
            }
        }
    }

    /* Enrollment collects the largest face of each pass (tag 0) */
    if (tag == 0) {
        if (g_fr.enroll.state == FR_ENROLL_COLLECTING) {
            if (g_fr.enroll.dim == 0) {
                g_fr.enroll.dim = dim;
            }
            if (dim == g_fr.enroll.dim) {
                memcpy(g_fr.enroll.samples +
                           (size_t)g_fr.enroll.samples_collected * g_fr.enroll.dim,
                       g_fr.probe, sizeof(float) * dim);
                g_fr.enroll.samples_collected++;
                if (g_fr.enroll.samples_collected >= g_fr.enroll.samples_needed) {
                    fr_finish_enroll(&g_fr);
                }
            } else {
                /* Model B changed mid-enrollment (reload): bail out */
                hal_mem_free(g_fr.enroll.samples);
                g_fr.enroll.samples = NULL;
                g_fr.enroll.state = FR_ENROLL_FAILED_INCONSISTENT;
            }
        }
        osMutexRelease(g_fr.mutex);
        return;
    }

    if (!track) {
        /* Track expired before the (relatively slow) inference completed:
         * the identity has nobody to attach to - drop the result. */
        osMutexRelease(g_fr.mutex);
        return;
    }

    /* Match + vote (only confirmed tracks carry a visible identity) */
    face_db_match_t m;
    face_db_match(dim, g_fr.probe, (float)cfg->match_threshold / 100.0f, &m);
    track->similarity = m.similarity;

    if (m.matched) {
        if (track->vote_db_id == m.id && m.id != 0) {
            track->vote_count++;
        } else {
            track->vote_db_id = m.id;
            strncpy(track->vote_name, m.name, FACE_DB_NAME_MAX - 1u);
            track->vote_count = 1;
        }

        if (track->status != FR_IDENT_KNOWN && track->vote_count >= cfg->confirm_votes) {
            track->status = FR_IDENT_KNOWN;
            track->db_id = m.id;
            strncpy(track->name, m.name, FACE_DB_NAME_MAX - 1u);
            track->name[FACE_DB_NAME_MAX - 1u] = '\0';
            g_fr.stats.recognized_count++;
        } else if (track->status == FR_IDENT_KNOWN && track->db_id == m.id) {
            /* Same person, keep the identity fresh */
            strncpy(track->name, m.name, FACE_DB_NAME_MAX - 1u);
        }
    } else {
        /* Failure vote. Two guards keep a freshly-appeared face from being
         * locked as UNKNOWN off its first inferences: right after entering
         * the frame the crop is often motion-blurred and the box still
         * settling, so the early embeddings are unreliable. (a) the track
         * must have settled for a few passes before failure votes count,
         * (b) confirming UNKNOWN needs MORE votes than a known match. */
        aicam_bool_t track_settled = (track->hits >= 6u);
        if (track->vote_db_id == 0 && track->vote_count > 0) {
            if (track_settled) {
                track->vote_count++;
            }
        } else {
            track->vote_db_id = 0;
            track->vote_name[0] = '\0';
            track->vote_count = track_settled ? 1u : 0u;
        }
        if (track->status == FR_IDENT_IDENTIFYING && track_settled &&
            track->vote_count >= cfg->confirm_votes + 2u) {
            track->status = FR_IDENT_UNKNOWN;
            track->db_id = 0;
            track->name[0] = '\0';
            g_fr.stats.unknown_count++;
        }

        /* Identity re-check: a confirmed (KNOWN) identity whose live
         * similarity fell to STRANGER level for SEVERAL consecutive
         * results is STALE - the face in the frame is no longer the
         * enrolled person (subject swap). The async pipeline (crop taken
         * 200-500 ms before the result arrives) can produce a single
         * spurious low score when the subject moved; requiring a streak
         * filters those out while still swapping within ~1 s. */
        if (track->status == FR_IDENT_KNOWN &&
            m.similarity < (float)cfg->match_threshold / 100.0f * 0.6f) {
            track->low_sim_streak++;
            if (track->low_sim_streak >= 3) {
                track->status = FR_IDENT_IDENTIFYING;
                track->db_id = 0;
                track->name[0] = '\0';
                track->vote_db_id = 0;
                track->vote_count = 0;
                track->last_identity_tick = 0;
                track->low_sim_streak = 0;
                LOG_CORE_INFO("FaceRec: identity swap (3x low sim, last %d%%)",
                              (int)(m.similarity * 100.0f));
            }
        } else {
            track->low_sim_streak = 0;
        }
    }

    osMutexRelease(g_fr.mutex);
}

/* ==================== Draw Overlay ==================== */

aicam_bool_t face_recognition_owns_overlay(void)
{
    /* Live probes: degrade to the raw Model A boxes whenever the two-stage
     * stage is not actually running — no Model B package, or the engine
     * disabled by the user (a loaded B package alone must not suppress the
     * raw boxes: nothing would draw, the stream went box-less). */
    if (!video_dual_model_model_b_loaded() || !video_dual_model_is_enabled()) {
        return AICAM_FALSE;
    }
    return g_fr_overlay_active;
}

void face_recognition_draw_overlay(uint8_t *fb, uint32_t width, uint32_t height)
{
    if (!g_fr.initialized || !fb || !g_fr.config.enabled || !g_fr.config.show_box) {
        return;
    }

    /* Snapshot under the mutex (the consumer may be running) */
    fr_track_t snap[FR_MAX_TRACKS];
    uint32_t count = 0;

    /* Aging uses the pass-interval ESTIMATE maintained by the consumer.
     * Never query video_dual_model_get_config() here: the camera thread
     * runs per frame and that call blocks on the engine mutex for up to
     * 100ms while a Model B inference (300ms+) holds it - that throttle
     * is what used to stall the whole PIPE1 encode path. */
    uint32_t interval_ms;

    if (osMutexAcquire(g_fr.mutex, 20) != osOK) {
        return;
    }
    /* now MUST be captured after the lock: see fr_expire_tracks */
    uint32_t now = osKernelGetTickCount();
    interval_ms = g_fr.pass_interval_ms;
    fr_expire_tracks(&g_fr, now, interval_ms);
    fr_check_enroll_timeout(&g_fr, now);
    for (uint32_t i = 0; i < FR_MAX_TRACKS && count < FR_MAX_TRACKS; i++) {
        if (g_fr.tracks[i].in_use && g_fr.tracks[i].confirmed) {
            memcpy(&snap[count], &g_fr.tracks[i], sizeof(fr_track_t));
            count++;
        }
    }
    aicam_bool_t enrolling = (g_fr.enroll.state == FR_ENROLL_COLLECTING);
    uint32_t enroll_collected = g_fr.enroll.samples_collected;
    uint32_t enroll_needed = g_fr.enroll.samples_needed;
    osMutexRelease(g_fr.mutex);

    if (count == 0 && !enrolling) {
        return;
    }

    if (!fr_lazy_draw_init(&g_fr)) {
        return;
    }

    /* Track coords are normalized to the PIPE2 (model input) field of view;
     * PIPE2 crops an aspect-true sensor region, so map through the view
     * rect before scaling to the encode frame or the boxes skew. */
    float view_x, view_y, view_w, view_h;
    camera_get_pipe2_view_rect(&view_x, &view_y, &view_w, &view_h);

    for (uint32_t i = 0; i < count; i++) {
        const fr_track_t *t = &snap[i];

        float fx = view_x + t->x * view_w;
        float fy = view_y + t->y * view_h;
        float fw = t->w * view_w;
        float fh = t->h * view_h;

        int bx = (int)(fx * (float)width);
        int by = (int)(fy * (float)height);
        int bw = (int)(fw * (float)width);
        int bh = (int)(fh * (float)height);

        uint32_t color = FR_COLOR_IDENTIFYING;
        if (t->status == FR_IDENT_KNOWN) {
            color = FR_COLOR_KNOWN;
        } else if (t->status == FR_IDENT_UNKNOWN) {
            color = FR_COLOR_UNKNOWN;
        }
        fr_draw_rect_hw(&g_fr, fb, width, height, bx, by, bw, bh, 2, color);

        /* Label: name + similarity% (ASCII font: non-ASCII names fall back
         * to the numeric id) */
        char label[FR_LABEL_MAX];
        size_t len = 0;
        if (t->status == FR_IDENT_KNOWN) {
            if (fr_name_is_ascii(t->name) && t->name[0]) {
                if (g_fr.config.show_similarity) {
                    len += (size_t)snprintf(label + len, sizeof(label) - len,
                                            "%s %d%%", t->name,
                                            (int)(t->similarity * 100.0f + 0.5f));
                } else {
                    len += (size_t)snprintf(label + len, sizeof(label) - len,
                                            "%s", t->name);
                }
            } else {
                len += (size_t)snprintf(label + len, sizeof(label) - len,
                                        "ID:%lu", (unsigned long)t->db_id);
            }
        } else if (t->status == FR_IDENT_UNKNOWN) {
            len += (size_t)snprintf(label + len, sizeof(label) - len, "Unknown");
        } else {
            len += (size_t)snprintf(label + len, sizeof(label) - len, "...");
        }

        if (g_fr.config.show_name && len > 0) {
            int lw = (int)fr_font_width(&g_fr, label) + 4;
            int lh = g_fr.font16.height + 2;
            int ly = by - lh - 2;
            if (ly < 0) {
                ly = by + bh + 2; /* flip below the box at the top edge */
            }
            fr_blend_rect_hw(&g_fr, fb, width, height, bx, ly, lw, lh,
                             FR_COLOR_LABEL_BG, FR_LABEL_ALPHA);
            fr_draw_text_hw(&g_fr, fb, width, height, bx + 2, ly + 1, label);
        }
    }

    /* Enrollment banner while sampling */
    if (enrolling) {
        char banner[FR_LABEL_MAX];
        snprintf(banner, sizeof(banner), "Enroll %lu/%lu",
                 (unsigned long)enroll_collected, (unsigned long)enroll_needed);
        int lw = (int)fr_font_width(&g_fr, banner) + 8;
        fr_blend_rect_hw(&g_fr, fb, width, height, 8, 8, lw, g_fr.font16.height + 6,
                         FR_COLOR_IDENTIFYING, FR_LABEL_ALPHA);
        fr_draw_text_hw(&g_fr, fb, width, height, 12, 11, banner);
    }
}

/* ==================== Draw Helpers (DMA2D, RGB565 in/out) ==================== */

static void fr_draw_rect_hw(face_recognition_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, int w, int h, uint32_t thickness, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fw) w = (int)fw - x;
    if (y + h > (int)fh) h = (int)fh - y;
    if (w <= 0 || h <= 0) {
        return;
    }

    draw_rect_param_t param = {0};
    param.p_dst = fb;
    param.dst_width = (int)fw;
    param.dst_height = (int)fh;
    param.x_pos = x;
    param.y_pos = y;
    param.width = w;
    param.height = h;
    param.line_width = (int)thickness;
    param.color = color;
    device_ioctl(ctx->draw_dev, DRAW_CMD_RECT, (uint8_t *)&param, sizeof(param));
}

static void fr_draw_text_hw(face_recognition_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                            int x, int y, const char *str)
{
    draw_printf_param_t param = {0};
    param.p_font = (DRAW_Font_t *)&ctx->font16;
    param.p_dst = fb;
    param.dst_width = (int)fw;
    param.dst_height = (int)fh;
    param.x_pos = x;
    param.y_pos = y;
    strncpy(param.str, str, MAX_LINE_CHAR - 1);
    param.str[MAX_LINE_CHAR - 1] = '\0';
    device_ioctl(ctx->draw_dev, DRAW_CMD_PRINTF, (uint8_t *)&param, sizeof(param));
}

static void fr_blend_rect_hw(face_recognition_ctx_t *ctx, uint8_t *fb, uint32_t fw, uint32_t fh,
                             int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (int)fw) w = (int)fw - x;
    if (y + h > (int)fh) h = (int)fh - y;
    if (w <= 0 || h <= 0) {
        return;
    }
    draw_colorrect_param_t param = {0};
    param.p_dst = fb;
    param.dst_width = (int)fw;
    param.dst_height = (int)fh;
    param.x_pos = x;
    param.y_pos = y;
    param.width = w;
    param.height = h;
    param.color = color;
    param.alpha = alpha;
    device_ioctl(ctx->draw_dev, DRAW_CMD_BLEND_COLOR_RECT, (uint8_t *)&param, sizeof(param));
}

static uint32_t fr_font_width(face_recognition_ctx_t *ctx, const char *str)
{
    uint32_t w = 0;
    for (const char *p = str; *p != '\0'; p++) {
        w += ctx->font16.width;
    }
    return w;
}

static aicam_bool_t fr_lazy_draw_init(face_recognition_ctx_t *ctx)
{
    if (ctx->draw_dev && ctx->font_ready) {
        return AICAM_TRUE;
    }

    if (!ctx->draw_dev) {
        ctx->draw_dev = device_find_pattern(DRAW_DEVICE_NAME, DEV_TYPE_VIDEO);
        if (!ctx->draw_dev) {
            return AICAM_FALSE;
        }
        /* Same color mode contract as ai_draw_service (RGB565 in/out) */
        draw_colormode_param_t cm = {0};
        cm.in_colormode = fmt_dcmipp_to_dma2d(DCMIPP_PIXEL_PACKER_FORMAT_RGB565_1);
        cm.out_colormode = DMA2D_OUTPUT_RGB565;
        device_ioctl(ctx->draw_dev, DRAW_CMD_SET_COLOR_MODE, (uint8_t *)&cm, sizeof(cm));
    }

    if (!ctx->font_ready) {
        draw_fontsetup_param_t font_param = {0};
        font_param.p_font_in = &Font16;
        font_param.p_font = &ctx->font16;
        if (device_ioctl(ctx->draw_dev, DRAW_CMD_FONT_SETUP,
                         (uint8_t *)&font_param, sizeof(font_param)) != AICAM_OK) {
            return AICAM_FALSE;
        }
        ctx->font_ready = AICAM_TRUE;
    }

    return AICAM_TRUE;
}

/* ==================== Init / Deinit / Config / Status ==================== */

aicam_result_t face_recognition_init(void)
{
    if (g_fr.initialized) {
        return AICAM_OK;
    }

    g_fr.mutex = osMutexNew(NULL);
    if (!g_fr.mutex) {
        LOG_CORE_ERROR("FaceRec: mutex creation failed");
        return AICAM_ERROR;
    }

    face_recognition_get_default_config(&g_fr.config);
    if (json_config_get_face_recognition_config(&g_fr.config) == AICAM_OK) {
        LOG_CORE_INFO("FaceRec: configuration loaded from NVS");
    }

    face_db_init();
    g_fr.stats.db_count = face_db_count();

    g_fr.initialized = AICAM_TRUE;
    g_fr.pass_interval_ms = 400u; /* sane seed; the consumer refines it */
    fr_update_overlay_flag();

    /* Register with the dual-model engine: fast-path target consumer
     * (tracking + enqueue gating) and async result consumer (identity). */
    aicam_result_t ret = video_dual_model_set_target_consumer(face_recognition_on_target, NULL);
    if (ret != AICAM_OK) {
        LOG_CORE_WARN("FaceRec: dual-model target consumer registration failed (%d) "
                     "(engine not initialized yet?)", ret);
    }
    ret = video_dual_model_set_result_consumer(face_recognition_on_embed, NULL);
    if (ret != AICAM_OK) {
        LOG_CORE_WARN("FaceRec: dual-model result consumer registration failed (%d) "
                     "(engine not initialized yet?)", ret);
    }

    LOG_CORE_INFO("FaceRec: demo initialized (enabled=%d, db=%lu)",
                 g_fr.config.enabled ? 1 : 0, (unsigned long)g_fr.stats.db_count);
    return AICAM_OK;
}

aicam_result_t face_recognition_deinit(void)
{
    if (!g_fr.initialized) {
        return AICAM_OK;
    }

    video_dual_model_set_result_consumer(NULL, NULL);

    if (osMutexAcquire(g_fr.mutex, 2000) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    if (g_fr.enroll.samples) {
        hal_mem_free(g_fr.enroll.samples);
        g_fr.enroll.samples = NULL;
    }
    memset(g_fr.tracks, 0, sizeof(g_fr.tracks));
    osMutexRelease(g_fr.mutex);

    g_fr.initialized = AICAM_FALSE;
    osMutexDelete(g_fr.mutex);
    g_fr.mutex = NULL;
    return AICAM_OK;
}

aicam_bool_t face_recognition_is_initialized(void)
{
    return g_fr.initialized;
}

aicam_result_t face_recognition_get_config(face_recognition_config_t *config)
{
    if (!config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_fr.initialized) {
        face_recognition_get_default_config(config);
        return AICAM_OK;
    }
    if (osMutexAcquire(g_fr.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    *config = g_fr.config;
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

aicam_result_t face_recognition_set_config(const face_recognition_config_t *config)
{
    if (!config || !g_fr.initialized) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (osMutexAcquire(g_fr.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    g_fr.config = *config;
    osMutexRelease(g_fr.mutex);
    fr_update_overlay_flag();
    return AICAM_OK;
}

void face_recognition_get_default_config(face_recognition_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->enabled = AICAM_TRUE;
    config->match_threshold = 60u;       /* cosine 0.60 */
    config->confirm_votes = 3u;
    config->track_iou_threshold = 30u;
    config->track_max_missed = 3u;
    config->face_hold_ms = 300u;
    config->enroll_samples = 5u;
    config->enroll_timeout_ms = 60000u;
    config->enroll_min_similarity = 30u;
    config->pose_max_yaw = 60u;          /* ratio x100 ~= +/-55 deg turn */
    config->pose_max_roll = 40u;         /* deg head tilt */
    config->show_box = AICAM_TRUE;
    config->show_name = AICAM_TRUE;
    config->show_similarity = AICAM_TRUE;
}

aicam_result_t face_recognition_get_results(fr_results_t *results)
{
    if (!results) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    memset(results, 0, sizeof(*results));
    if (!g_fr.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    uint32_t now;

    if (osMutexAcquire(g_fr.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    now = osKernelGetTickCount(); /* after the lock: see fr_expire_tracks */
    fr_expire_tracks(&g_fr, now, g_fr.pass_interval_ms);
    for (uint32_t i = 0; i < FR_MAX_TRACKS && results->count < FR_MAX_TRACKS; i++) {
        const fr_track_t *t = &g_fr.tracks[i];
        if (!t->in_use || !t->confirmed) {
            continue;
        }
        fr_track_info_t *info = &results->tracks[results->count++];
        info->track_id = t->id;
        info->x = t->x;
        info->y = t->y;
        info->w = t->w;
        info->h = t->h;
        info->det_conf = t->det_conf;
        info->status = t->status;
        info->db_id = t->db_id;
        info->similarity = t->similarity;
        info->frontal = t->frontal;
        info->pose_score = t->pose_score;
        strncpy(info->name, t->name, FACE_DB_NAME_MAX - 1u);
    }
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

aicam_result_t face_recognition_get_stats(fr_stats_t *stats)
{
    if (!stats) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_fr.initialized) {
        memset(stats, 0, sizeof(*stats));
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    uint32_t now;

    if (osMutexAcquire(g_fr.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    now = osKernelGetTickCount(); /* after the lock: see fr_expire_tracks */
    fr_expire_tracks(&g_fr, now, g_fr.pass_interval_ms);
    g_fr.stats.enabled = g_fr.config.enabled;
    g_fr.stats.db_count = face_db_count();
    uint32_t confirmed = 0;
    for (uint32_t i = 0; i < FR_MAX_TRACKS; i++) {
        if (g_fr.tracks[i].in_use && g_fr.tracks[i].confirmed) {
            confirmed++;
        }
    }
    g_fr.stats.confirmed_tracks = confirmed;
    g_fr.stats.total_tracks = g_fr.total_tracks;
    *stats = g_fr.stats;
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

aicam_result_t face_recognition_reset(void)
{
    if (!g_fr.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_fr.mutex, 200) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    memset(g_fr.tracks, 0, sizeof(g_fr.tracks));
    g_fr.total_tracks = 0;
    g_fr.stats.recognized_count = 0;
    g_fr.stats.unknown_count = 0;
    g_fr.stats.embed_count = 0;
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

/* ==================== Enrollment API ==================== */

aicam_result_t face_recognition_enroll_start(const char *name)
{
    if (!name || name[0] == '\0') {
        return AICAM_ERROR_INVALID_PARAM;
    }
    if (!g_fr.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    if (osMutexAcquire(g_fr.mutex, 200) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }

    if (!g_fr.config.enabled) {
        osMutexRelease(g_fr.mutex);
        return AICAM_ERROR_INVALID_STATE;
    }
    if (g_fr.enroll.state == FR_ENROLL_COLLECTING) {
        osMutexRelease(g_fr.mutex);
        return AICAM_ERROR_BUSY;
    }

    uint32_t n = g_fr.config.enroll_samples;
    if (n == 0 || n > FR_ENROLL_SAMPLES_MAX) {
        n = 5;
    }

    float *samples = hal_mem_alloc_any(sizeof(float) * (size_t)n * FACE_DB_EMBED_DIM_MAX);
    if (!samples) {
        osMutexRelease(g_fr.mutex);
        return AICAM_ERROR_NO_MEMORY;
    }

    memset(&g_fr.enroll, 0, sizeof(g_fr.enroll));
    g_fr.enroll.state = FR_ENROLL_COLLECTING;
    strncpy(g_fr.enroll.name, name, FACE_DB_NAME_MAX - 1u);
    g_fr.enroll.samples_needed = n;
    g_fr.enroll.samples = samples;
    g_fr.enroll.dim = 0;
    g_fr.enroll.capture_armed = AICAM_FALSE;
    g_fr.enroll.start_tick = osKernelGetTickCount();

    osMutexRelease(g_fr.mutex);
    LOG_CORE_INFO("FaceRec: manual enrollment started for '%s' (max %lu samples)",
                 name, (unsigned long)n);
    return AICAM_OK;
}

aicam_result_t face_recognition_enroll_capture(void)
{
    if (!g_fr.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_fr.mutex, 200) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    if (g_fr.enroll.state == FR_ENROLL_PAUSED) {
        /* Resume a paused (partial) enrollment: fresh window, same samples. */
        g_fr.enroll.state = FR_ENROLL_COLLECTING;
        g_fr.enroll.start_tick = osKernelGetTickCount();
    } else if (g_fr.enroll.state != FR_ENROLL_COLLECTING) {
        osMutexRelease(g_fr.mutex);
        return AICAM_ERROR_INVALID_STATE;
    }
    /* Arm exactly one capture; the next qualifying largest-face pass grabs it */
    g_fr.enroll.capture_armed = AICAM_TRUE;
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

aicam_result_t face_recognition_enroll_finish(void)
{
    if (!g_fr.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_fr.mutex, 200) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    if (g_fr.enroll.state != FR_ENROLL_COLLECTING && g_fr.enroll.state != FR_ENROLL_PAUSED) {
        osMutexRelease(g_fr.mutex);
        return AICAM_ERROR_INVALID_STATE;
    }
    /* An average needs at least 2 vectors to mean anything. */
    if (g_fr.enroll.samples_collected < 2u || g_fr.enroll.samples == NULL) {
        osMutexRelease(g_fr.mutex);
        return AICAM_ERROR_INVALID_STATE;
    }
    fr_finish_enroll(&g_fr);
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

aicam_result_t face_recognition_enroll_cancel(void)
{
    if (!g_fr.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }
    if (osMutexAcquire(g_fr.mutex, 200) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    if (g_fr.enroll.state == FR_ENROLL_COLLECTING || g_fr.enroll.state == FR_ENROLL_PAUSED) {
        if (g_fr.enroll.samples) {
            hal_mem_free(g_fr.enroll.samples);
            g_fr.enroll.samples = NULL;
        }
        g_fr.enroll.state = FR_ENROLL_IDLE;
    }
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

aicam_result_t face_recognition_enroll_get_status(fr_enroll_status_t *status)
{
    if (!status) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    memset(status, 0, sizeof(*status));
    if (!g_fr.initialized) {
        return AICAM_ERROR_NOT_INITIALIZED;
    }

    if (osMutexAcquire(g_fr.mutex, 100) != osOK) {
        return AICAM_ERROR_TIMEOUT;
    }
    fr_check_enroll_timeout(&g_fr, osKernelGetTickCount());
    status->state = g_fr.enroll.state;
    status->samples_collected = g_fr.enroll.samples_collected;
    status->samples_needed = g_fr.enroll.samples_needed;
    status->new_id = g_fr.enroll.new_id;
    status->capture_armed = g_fr.enroll.capture_armed ? 1u : 0u;
    osMutexRelease(g_fr.mutex);
    return AICAM_OK;
}

/* ==================== Field-Service Diagnostics ==================== */

void face_recognition_debug_snapshot(cJSON *ring)
{
    if (!ring || !g_fr.initialized) {
        return;
    }
    if (osMutexAcquire(g_fr.mutex, 100) != osOK) {
        return;
    }
    /* First element: last track-expire decision */
    {
        cJSON *ex = cJSON_CreateObject();
        cJSON_AddStringToObject(ex, "what", "last_expire");
        cJSON_AddNumberToObject(ex, "cleared_id", g_fr.last_expire_dbg.cleared_id);
        cJSON_AddNumberToObject(ex, "age_ms", g_fr.last_expire_dbg.age);
        cJSON_AddNumberToObject(ex, "lifetime_ms", g_fr.last_expire_dbg.lifetime);
        cJSON_AddNumberToObject(ex, "tick", g_fr.last_expire_dbg.tick);
        cJSON_AddNumberToObject(ex, "cfg_face_hold_ms", g_fr.config.face_hold_ms);
        cJSON_AddNumberToObject(ex, "cfg_track_max_missed", g_fr.config.track_max_missed);
        cJSON_AddItemToArray(ring, ex);
    }
    uint32_t idx = g_fr.dbg_idx;
    uint32_t count = idx < FR_DBG_RING ? idx : FR_DBG_RING;
    /* Oldest first: ring starts at (idx - count) % FR_DBG_RING */
    for (uint32_t k = count; k > 0; k--) {
        uint32_t slot = (idx - k) % FR_DBG_RING;
        const typeof(g_fr.dbg[0]) *e = &g_fr.dbg[slot];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", e->seq);
        cJSON_AddNumberToObject(item, "frame_id", e->frame_id);
        cJSON_AddNumberToObject(item, "t_x", e->tx);
        cJSON_AddNumberToObject(item, "t_y", e->ty);
        cJSON_AddNumberToObject(item, "t_w", e->tw);
        cJSON_AddNumberToObject(item, "t_h", e->th);
        cJSON_AddNumberToObject(item, "m_x", e->mx);
        cJSON_AddNumberToObject(item, "m_y", e->my);
        cJSON_AddNumberToObject(item, "m_w", e->mw);
        cJSON_AddNumberToObject(item, "m_h", e->mh);
        cJSON_AddNumberToObject(item, "best_iou", e->best_iou);
        cJSON_AddNumberToObject(item, "matched_track", e->matched_track);
        cJSON_AddNumberToObject(item, "n_tracks", e->n_tracks);
        cJSON_AddNumberToObject(item, "first_of_pass", e->first_of_pass);
        cJSON *live = cJSON_CreateArray();
        for (uint32_t k = 0; k < FR_DBG_TRACKS; k++) {
            cJSON *lt = cJSON_CreateObject();
            cJSON_AddNumberToObject(lt, "id", e->live[k].id);
            if (e->live[k].id >= 0) {
                cJSON_AddNumberToObject(lt, "hits", e->live[k].hits);
                cJSON_AddNumberToObject(lt, "x", e->live[k].x);
                cJSON_AddNumberToObject(lt, "y", e->live[k].y);
                cJSON_AddNumberToObject(lt, "w", e->live[k].w);
                cJSON_AddNumberToObject(lt, "h", e->live[k].h);
                cJSON_AddNumberToObject(lt, "age_ms", e->live[k].age_ms);
            }
            cJSON_AddItemToArray(live, lt);
        }
        cJSON_AddItemToObject(item, "live", live);
        cJSON_AddItemToArray(ring, item);
    }
    osMutexRelease(g_fr.mutex);
}
