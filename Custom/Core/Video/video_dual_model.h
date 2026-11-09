/**
 * @file video_dual_model.h
 * @brief Generic two-stage inference engine: Model A (active detection model)
 *        -> crop -> Model B (recognition model)
 * @details Runs as the AI node's frame hook (video_ai_node_set_frame_hook):
 *          after each successful Model A inference, Model A's detections are
 *          filtered and ranked, the top boxes are cropped from the raw PIPE2
 *          RGB888 frame, letterboxed (aspect-preserving scale + pad) into
 *          Model B's input geometry and pushed through a second NN instance.
 *          Both models share the NPU through the global mutex in nn.c.
 *
 *          Model A is whatever model the AI node already runs (active AI
 *          slot). Model B is loaded from the *other* AI slot, so the pair is
 *          (detector, recognizer) - e.g. YuNet face detection + a face
 *          embedding network, or later a plate detector + plate recognizer.
 *
 *          Thread model:
 *          - video_dual_model_on_frame(): AI node thread (frame hook)
 *          - video_dual_model_load/reload_model(), config accessors: web
 *            server / service threads
 *          - consumer callback: AI node thread, synchronous
 *          All shared state is protected by one internal mutex.
 */

#ifndef VIDEO_DUAL_MODEL_H
#define VIDEO_DUAL_MODEL_H

#include "aicam_types.h"
#include "nn.h"
#include "dual_model_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Limits ==================== */

/* Max Model A detections considered per pass (matching the tracker caps used
 * elsewhere in the video layer) */
#define DM_MAX_DETECTIONS 16

/* ==================== Types ==================== */

/* Job slots bounding Model B work: at most one pending/running crop per
 * visible face, stale crops of the same tag are replaced, so the engine
 * never queues a backlog of outdated faces. */
#define DM_JOB_SLOTS 4

/* Keypoints passed through from the Model A detection (e.g. YuNet's 5 face
 * landmarks) so applications can gate crops (frontal-pose checks etc.) */
#define DUAL_MODEL_MAX_KPTS 6

/**
 * @brief One Model B inference paired with its Model A source detection
 * @details Two lifetime contracts:
 *          - detection stage (target consumer, AI node thread): b_result is
 *            NULL, the box/frame describe the crop about to be cut;
 *            the consumer updates fast state (tracking/drawing) and decides
 *            whether this crop should go to Model B, attaching a tag that
 *            travels with the job (e.g. the track id).
 *          - result stage (result consumer, worker thread): b_result points
 *            into Model B's postprocess output buffers and stays valid only
 *            until the NEXT Model B inference - consumers must copy.
 */
typedef struct {
    uint32_t frame_id;             // Camera frame id of the source frame
    float x, y, w, h;              // Normalized crop box actually fed to B (A-input space)
    float det_conf;                // Model A detection confidence
    const nn_result_t *b_result;   // Model B output (NULL at detection stage / dropped job)
    uint32_t b_infer_ms;           // Model B inference duration
    uint32_t n_kpts;               // Keypoints carried from the detection (0 = none)
    float kpts[DUAL_MODEL_MAX_KPTS][2]; // Normalized, same space as the box
} dual_model_target_t;

/**
 * @brief Detection-stage consumer: called on the AI node thread for each
 *        selected target right after Model A, BEFORE any Model B work.
 * @param target Detection description (b_result == NULL)
 * @param user_data Registration user data
 * @param out_tag Optional tag attached to the Model B job (e.g. track id)
 * @return AICAM_TRUE to submit this crop to Model B (async), FALSE to skip
 */
typedef aicam_bool_t (*dual_model_target_cb_t)(const dual_model_target_t *target,
                                               void *user_data,
                                               intptr_t *out_tag);

/**
 * @brief Result-stage consumer: called on the worker thread when Model B
 *        finishes (or when a job was dropped - b_result == NULL)
 * @param target Job result (b_result transient, copy what you keep)
 * @param tag The tag attached by the target consumer at submit time
 * @param user_data Registration user data
 */
typedef void (*dual_model_result_cb_t)(const dual_model_target_t *target,
                                       intptr_t tag,
                                       void *user_data);

/* Runtime status for /status endpoints */
typedef struct {
    aicam_bool_t enabled;
    aicam_bool_t model_b_loaded;
    char model_b_name[64];         // Model B name ("" when not loaded)
    uint32_t model_b_input_w;      // Model B input geometry
    uint32_t model_b_input_h;
    char model_b_postprocess[32];  // Model B postprocess type
    uint32_t model_b_addr;         // Flash address Model B was loaded from
    uint32_t passes;               // Completed Model A passes with >=1 target
    uint32_t inferences;           // Total Model B inferences
    uint32_t errors;               // Model B inference errors
    uint32_t last_pass_ms;         // Duration of the last pass (crops + submits)
    uint32_t last_targets;         // Targets processed in the last pass
    uint32_t avg_infer_ms;         // Rolling average per-B-inference time
    uint32_t skipped_throttle;     // Passes skipped by interval throttling
    uint32_t jobs_dropped;         // Submits dropped (no free job slot)
    aicam_bool_t disabled_by_errors; // Fail-safe latch after repeated errors
    uint32_t last_run_tick;        // Kernel tick of the last pass
    char model_b_error[96];        // Why the last load attempt failed ("" when loaded / never tried)
} dual_model_status_t;

/* ==================== API Functions ==================== */

/**
 * @brief Initialize the engine (idempotent)
 * @details Loads the persisted configuration when the config manager is up,
 *          otherwise factory defaults (retried lazily on first frame).
 */
aicam_result_t video_dual_model_init(void);

/**
 * @brief Deinitialize: unload Model B and release resources
 */
aicam_result_t video_dual_model_deinit(void);

/**
 * @brief Load Model B from the AI partition that is NOT active for Model A
 * @details Active slot -> Model A, other slot -> Model B (mirrors the
 *          json_config_get_ai_1_active() convention of the primary loader).
 *          When the partition holds no valid model package the engine
 *          degrades gracefully: two-stage processing stays off, single-model
 *          Model A results are unaffected.
 * @return AICAM_OK when loaded; AICAM_ERROR_* otherwise (status reflects it)
 */
aicam_result_t video_dual_model_load_model(void);

/**
 * @brief Quiesce the frame hook, unload and re-load Model B (web "reload")
 * @details Used after a new Model B package has been flashed to the slot.
 *          The AI node thread is paced by its inference interval; the hook
 *          is disabled and a settle delay runs before the instance is freed,
 *          so an in-flight pass cannot touch a destroyed instance.
 */
aicam_result_t video_dual_model_reload_model(void);

/**
 * @brief Frame hook body - signature matches video_ai_frame_hook_t
 * @details Installed by the AI service on the AI node. Do not call directly.
 */
void video_dual_model_on_frame(const nn_result_t *result,
                               const uint8_t *pipe2_buffer,
                               uint32_t pipe2_size,
                               uint32_t frame_id,
                               void *user_data);

/* Configuration */
aicam_result_t video_dual_model_get_config(dual_model_config_t *config);
aicam_result_t video_dual_model_set_config(const dual_model_config_t *config);
void video_dual_model_get_default_config(dual_model_config_t *config);

/* Status & maintenance */
aicam_result_t video_dual_model_get_status(dual_model_status_t *status);

/**
 * @brief Lock-free Model B loaded probe (single-word read, safe from the
 *        camera draw path). Consumers that "own" the overlay only when the
 *        two-stage stage is actually running use this to degrade cleanly
 *        to the raw Model A boxes while Model B is missing.
 * @return 1 when a Model B package is currently loaded, 0 otherwise
 */
int video_dual_model_model_b_loaded(void);

/**
 * @brief Lock-free enabled probe (single-word read). Mirrors the persisted
 *        engine config: when the engine is disabled no targets reach the
 *        consumers, so overlay owners must fall back to the raw Model A
 *        boxes even while a Model B package stays loaded.
 * @return 1 when the engine is enabled, 0 otherwise
 */
int video_dual_model_is_enabled(void);
aicam_result_t video_dual_model_reset_stats(void);

/**
 * @brief Register the detection-stage consumer (fast path, pre-Model B)
 * @details Called on the AI node thread for each selected target right
 *          after Model A, while the PIPE2 buffer is still held (keep it
 *          quick). The return value gates Model B submission; a tag can be
 *          attached that is handed back with the result. Pass NULL to
 *          unregister (every target is then submitted with tag 0 while a
 *          result consumer exists).
 */
aicam_result_t video_dual_model_set_target_consumer(dual_model_target_cb_t cb,
                                                    void *user_data);

/**
 * @brief Register the result-stage consumer (Model B worker thread)
 * @details Called after each Model B inference with the tag attached at
 *          submit time, or with b_result == NULL when a job was dropped
 *          (slot exhaustion) so the consumer can release per-track state.
 *          The payload in target->b_result is transient (valid until the
 *          next Model B inference). Pass NULL to unregister.
 */
aicam_result_t video_dual_model_set_result_consumer(dual_model_result_cb_t cb,
                                                    void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_DUAL_MODEL_H */
