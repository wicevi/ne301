/**
 * @file face_recognition_config.h
 * @brief Face recognition demo configuration (shared with the persistent
 *        configuration manager)
 * @details Kept dependency-free so both the video-layer module and the
 *          JSON/NVS config manager can include it without NN headers.
 *
 *          This demo rides on the generic dual-model engine (Model A face
 *          detection + Model B face embedding); engine-wide settings live in
 *          dual_model_config_t, this section holds the matching/identity
 *          policy and the demo presentation switches.
 */

#ifndef FACE_RECOGNITION_CONFIG_H
#define FACE_RECOGNITION_CONFIG_H

#include "aicam_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration ==================== */

/* Enrollment sample storage cap (samples are dim floats each; keep the
 * enrollment scratch well under 100KB even for 256-d embeddings) */
#define FR_ENROLL_SAMPLES_MAX 10u

typedef struct {
    aicam_bool_t enabled;              // Demo master switch (identity + OSD)
    /* Matching / identity policy */
    uint32_t match_threshold;          // Min cosine similarity x100 (1..99)
    uint32_t confirm_votes;            // Consecutive consistent matches before
                                       // an identity is confirmed (1..10)
    /* Tracking (operates at Model B pass cadence, not camera fps) */
    uint32_t track_iou_threshold;      // IoU match threshold x100 (1..99)
    uint32_t track_max_missed;         // B-passes a track survives unmatched (1..50)
    uint32_t face_hold_ms;             // Anti-blink hold when a pass finds no
                                       // face (100..2000)
    /* Enrollment */
    uint32_t enroll_samples;           // Samples averaged per enrollment (1..FR_ENROLL_SAMPLES_MAX)
    uint32_t enroll_timeout_ms;        // Sampling window (1000..60000)
    uint32_t enroll_min_similarity;    // Min mean sample-to-template similarity
                                       // x100, sanity against garbage samples (1..99)
    /* Frontal-pose gate (from the 5 detection landmarks) */
    uint32_t pose_max_yaw;             // Max yaw ratio x100 (20..99):
                                      // |nose.x - eye mid.x| / eye dist.
                                      // 60 ~= +/-55 deg turn; 99 ~ off
    uint32_t pose_max_roll;            // Max head roll (eye-line tilt) in deg (10..60)
    /* Overlay drawing */
    aicam_bool_t show_box;
    aicam_bool_t show_name;
    aicam_bool_t show_similarity;
} face_recognition_config_t;

#ifdef __cplusplus
}
#endif

#endif /* FACE_RECOGNITION_CONFIG_H */
