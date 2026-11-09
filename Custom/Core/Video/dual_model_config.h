/**
 * @file dual_model_config.h
 * @brief Generic dual-model (Model A -> crop -> Model B) configuration
 *        structure, shared with the persistent configuration manager
 * @details Kept dependency-free (only aicam_types.h/stdint) so both the
 *          video-layer module and the JSON/NVS config manager can include it
 *          without pulling NN headers.
 *
 *          Pipeline contract: Model A (the active model, fed the camera
 *          PIPE2 frames) produces detections; each selected detection box is
 *          expanded, cropped from the PIPE2 RGB888 frame, letterboxed
 *          (scale + pad, never stretched) to Model B's input size and run
 *          through Model B. Face recognition (embedding), license-plate
 *          recognition and similar two-stage tasks are all applications of
 *          this generic engine.
 */

#ifndef DUAL_MODEL_CONFIG_H
#define DUAL_MODEL_CONFIG_H

#include "aicam_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== Configuration ==================== */

/* Longest class-name filter accepted (empty string = accept every class) */
#define DM_CLASS_FILTER_MAX 24u
/* Longest postprocess-type name accepted in the load contract */
#define DM_POSTPROCESS_MAX 32u

typedef struct {
    aicam_bool_t enabled;              // Master switch for Model B processing
    /* Target selection (from Model A's detections) */
    uint32_t target_conf_threshold;    // Min A-detection confidence x100 (1..99)
    char target_class[DM_CLASS_FILTER_MAX]; // Accept only this class name ("" = any)
    uint32_t crop_expand_pm;           // Crop box expansion in permille per side (0..500)
    uint32_t crop_min_size;            // Min crop edge in px, Model A input space (8..1024)
    uint32_t max_targets;              // Max crops fed to Model B per pass (1..4)
    /* Pacing */
    uint32_t interval_ms;              // Min interval between Model B passes (0..10000)
    /* Letterbox */
    uint32_t pad_value;                // Letterbox pad pixel value 0..255 (127 = neutral gray)
    /* Load contract: the Model B package's postprocess type must equal this
     * string ("" disables the check). Guards against flashing a DETECTION
     * model into the Model B slot: it would load fine (same RGB888 input
     * contract) and silently burn NPU cycles producing results the
     * application consumer drops. The face demo ships "pp_face_embed"; a
     * plate-recognition application would set its own pp type here. */
    char expected_postprocess[DM_POSTPROCESS_MAX];
} dual_model_config_t;

#ifdef __cplusplus
}
#endif

#endif /* DUAL_MODEL_CONFIG_H */
