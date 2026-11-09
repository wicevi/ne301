/**
 * @file face_recognition.h
 * @brief Local face recognition demo on top of the dual-model engine
 * @details Model A (face detection) + Model B (face embedding, pp_face_embed)
 *          + a local NVS template database: every Model B embedding of a
 *          tracked face is matched by cosine similarity; a per-track vote
 *          confirms the identity before it is shown. No cloud, no external
 *          recognition service - everything runs on the device.
 *
 *          Thread model:
 *          - face_recognition_on_target(): AI node thread, inside the
 *            engine mutex (must stay quick, no video_dual_model_* calls) -
 *            tracking + drawing state + Model B enqueue gating
 *          - face_recognition_on_embed(): Model B worker thread, inside the
 *            engine mutex - identity updates by track tag
 *          - face_recognition_draw_overlay(): camera node thread
 *          - config/status/enroll accessors: web server thread
 *          All shared state is protected by one internal mutex.
 */

#ifndef FACE_RECOGNITION_H
#define FACE_RECOGNITION_H

#include "aicam_types.h"
#include "video_dual_model.h"
#include "face_db.h"
#include "face_recognition_config.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Limits ==================== */

#define FR_MAX_TRACKS 8

/* ==================== Types ==================== */

/* Identity state of a track (display order: identifying -> known/unknown) */
typedef enum {
    FR_IDENT_IDENTIFYING = 0,   /* Not enough votes yet */
    FR_IDENT_KNOWN,             /* Confirmed match (name + similarity) */
    FR_IDENT_UNKNOWN,           /* Confirmed no-match above threshold */
} fr_ident_status_t;

/* Snapshot of one track for /results and the web overlay */
typedef struct {
    uint32_t track_id;
    float x, y, w, h;               /* Normalized box (Model A input space) */
    float det_conf;                 /* Model A detection confidence */
    fr_ident_status_t status;
    char name[FACE_DB_NAME_MAX];    /* "" until known */
    uint32_t db_id;                 /* 0 until known */
    float similarity;               /* Best cosine similarity so far (0..1) */
    aicam_bool_t frontal;           /* Latest detection is frontal (landmarks) */
    float pose_score;               /* 0..1 frontal-pose quality */
} fr_track_info_t;

typedef struct {
    uint32_t count;
    fr_track_info_t tracks[FR_MAX_TRACKS];
} fr_results_t;

/* Runtime statistics (web-visible) */
typedef struct {
    aicam_bool_t enabled;
    uint32_t confirmed_tracks;      /* Tracks currently visible */
    uint32_t total_tracks;          /* Cumulative track ids assigned */
    uint32_t embed_count;           /* Embeddings consumed */
    uint32_t recognized_count;      /* Identity confirmations (known) */
    uint32_t unknown_count;         /* Unknown confirmations */
    uint32_t db_count;              /* Enrolled templates */
    uint32_t last_embed_ms;         /* Last Model B inference time */
    uint32_t last_update_tick;      /* Kernel tick of the last pass */
} fr_stats_t;

/* Enrollment state machine status (web-visible) */
typedef enum {
    FR_ENROLL_IDLE = 0,
    FR_ENROLL_COLLECTING,
    FR_ENROLL_PAUSED,               /* Window expired WITH partial samples:
                                     * progress kept, user continues capturing
                                     * (enroll_capture resumes) or finishes */
    FR_ENROLL_DONE,
    FR_ENROLL_FAILED_NO_FACE,       /* Window expired without ANY sample */
    FR_ENROLL_FAILED_INCONSISTENT,  /* Samples disagree (moved too fast / 2 faces) */
    FR_ENROLL_FAILED_DB,            /* Database full / save error */
} fr_enroll_state_t;

typedef struct {
    fr_enroll_state_t state;
    uint32_t samples_collected;
    uint32_t samples_needed;
    uint32_t new_id;                /* Valid when state == DONE */
    float mean_similarity;          /* Mean sample-to-template similarity */
    uint32_t capture_armed;         /* 1 = armed, waiting for a frontal pose */
} fr_enroll_status_t;

/* ==================== API Functions ==================== */

/**
 * @brief Initialize the demo (idempotent)
 * @details Loads the persisted configuration and the face database, then
 *          registers itself as the dual-model engine's result consumer.
 */
aicam_result_t face_recognition_init(void);

aicam_result_t face_recognition_deinit(void);
aicam_bool_t face_recognition_is_initialized(void);

/* Configuration */
aicam_result_t face_recognition_get_config(face_recognition_config_t *config);
aicam_result_t face_recognition_set_config(const face_recognition_config_t *config);
void face_recognition_get_default_config(face_recognition_config_t *config);

/* Snapshots */
aicam_result_t face_recognition_get_results(fr_results_t *results);
aicam_result_t face_recognition_get_stats(fr_stats_t *stats);

/**
 * @brief Reset trackers and statistics (not the face database)
 */
aicam_result_t face_recognition_reset(void);

/**
 * @brief Detection-stage consumer - fast path before Model B
 * @details Signature matches dual_model_target_cb_t; registered by
 *          face_recognition_init(). Updates tracking/drawing state
 *          immediately and gates Model B submission per track (one
 *          in-flight job, identity-refresh pacing, tag = track id;
 *          tag 0 during enrollment). Registered by init.
 */
aicam_bool_t face_recognition_on_target(const dual_model_target_t *target,
                                        void *user_data,
                                        intptr_t *out_tag);

/**
 * @brief Result-stage consumer - Model B worker thread
 * @details Signature matches dual_model_result_cb_t. Routes embeddings to
 *          the enrollment sampler (tag 0) or updates the identity of the
 *          track the crop came from. Non-embedding results are ignored.
 */
void face_recognition_on_embed(const dual_model_target_t *target,
                               intptr_t tag,
                               void *user_data);

/**
 * @brief Draw the overlay on a camera PIPE1 RGB565 frame
 * @details Boxes + name/similarity labels (green = known, gray = unknown,
 *          orange = identifying). Safe to call every camera frame; no-ops
 *          when the demo is disabled.
 */
void face_recognition_draw_overlay(uint8_t *fb, uint32_t width, uint32_t height);

/**
 * @brief Lock-free check: does the demo currently own the stream overlay?
 * @details When true the caller (ai_service draw callback) should skip
 *          drawing raw Model A detections to avoid double boxes. Safe from
 *          the camera thread (plain volatile read).
 */
aicam_bool_t face_recognition_owns_overlay(void);

/**
 * @brief Start an enrollment session (manual, multi-pose)
 * @details Samples are captured one by one via face_recognition_enroll_capture()
 *          (each press grabs the next frontal pose); the session auto-completes
 *          at the configured sample count or early via enroll_finish().
 * @return AICAM_ERROR_NOT_INITIALIZED / _BUSY when one is already running
 */
aicam_result_t face_recognition_enroll_start(const char *name);

/**
 * @brief Arm ONE sample capture for the running enrollment
 * @details The next frontal largest-face pass consumes the arm and appends
 *          the embedding to the sample set.
 */
aicam_result_t face_recognition_enroll_capture(void);

/**
 * @brief Finish the running enrollment early with the samples collected
 * @details Requires at least one captured sample.
 */
aicam_result_t face_recognition_enroll_finish(void);

/**
 * @brief Cancel a running enrollment
 */
aicam_result_t face_recognition_enroll_cancel(void);

/**
 * @brief Enrollment progress (poll from the web layer)
 */
aicam_result_t face_recognition_enroll_get_status(fr_enroll_status_t *status);

/**
 * @brief Append the consumer diagnostics ring to a JSON array
 * @details Field-service aid: the last FR_DBG_RING consumer calls with their
 *          target box, best IoU and match decision. Thread-safe.
 * @param ring cJSON array to append entries to (created by the caller)
 */
void face_recognition_debug_snapshot(cJSON *ring);

#ifdef __cplusplus
}
#endif

#endif /* FACE_RECOGNITION_H */
