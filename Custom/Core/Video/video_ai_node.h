/**
 * @file video_ai_node.h
 * @brief Video AI Processing Node
 * @details AI inference node based on nn.h/nn.c device interaction with zero-copy
 */

#ifndef VIDEO_AI_NODE_H
#define VIDEO_AI_NODE_H

#include "video_pipeline.h"
#include "aicam_types.h"
#include "dev_manager.h"
#include "nn.h"
#include "ai_draw_service.h"
#include "camera.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NN_RESULT_CACHE_SIZE 5

/* ==================== AI Node Configuration ==================== */

/**
 * @brief AI node configuration
 */
typedef struct {
    uint32_t width;                       // Input width
    uint32_t height;                      // Input height
    uint32_t fps;                         // Frame rate
    uint32_t input_format;                // Input format
    uint32_t confidence_threshold;        // Confidence threshold (0-100)
    uint32_t nms_threshold;               // NMS threshold (0-100)
    uint32_t max_detections;              // Maximum detections per frame
    uint32_t processing_interval;         // Processing interval (frames)
    uint32_t inference_interval_ms;       // Inference pacing interval in ms (0 = every frame)
    uint32_t bpp;                         // Bits per pixel
    aicam_bool_t enabled;                 // AI processing enabled
    aicam_bool_t overlay_results;         // Overlay detection results
    aicam_bool_t enable_drawing;          // Enable AI result drawing
    ai_draw_config_t draw_config;         // Drawing configuration
} video_ai_config_t;

/**
 * @brief AI node statistics
 */
typedef struct {
    uint64_t frames_processed;            // Total frames processed
    uint64_t frames_skipped;              // Frames skipped (disabled/interval)
    uint64_t detections_found;            // Total detections found
    uint64_t processing_errors;           // Processing errors
    uint64_t drawing_errors;              // Drawing errors
    uint64_t avg_processing_time_us;      // Average processing time
    uint64_t max_processing_time_us;      // Maximum processing time
    uint64_t avg_drawing_time_us;         // Average drawing time
    uint64_t max_drawing_time_us;         // Maximum drawing time
    uint32_t current_detection_count;     // Current frame detection count
} video_ai_stats_t;

typedef struct {
    nn_result_t result;
    //uint64_t timestamp;        // Frame timestamp (milliseconds)
    uint32_t frame_id;         // Frame ID for synchronization
} nn_result_with_frame_id_t;

/**
 * @brief Inference result callback
 * @details Invoked on the AI node's processing thread immediately after a
 *          successful inference, while the postprocess output buffers still
 *          hold that inference's data. The callee must copy whatever it needs
 *          before returning and must not block.
 */
typedef void (*video_ai_result_callback_t)(const nn_result_t *result,
                                           uint32_t frame_id,
                                           uint32_t inference_time_ms,
                                           void *user_data);

/**
 * @brief Inference frame hook
 * @details Like video_ai_result_callback_t, but additionally receives the
 *          raw pipe2 RGB888 buffer and runs BEFORE the buffer is returned to
 *          the camera — the callee may read pixels (crop/warp for a secondary
 *          model) and run further NPU work (serialized through the global NPU
 *          mutex in nn.c). Keep the work bounded: the pipe2 buffer pool is
 *          only three deep and no new primary inference happens until this
 *          returns. Invoked on the AI node's processing thread.
 */
typedef void (*video_ai_frame_hook_t)(const nn_result_t *result,
                                      const uint8_t *pipe2_buffer,
                                      uint32_t pipe2_size,
                                      uint32_t frame_id,
                                      void *user_data);

/**
 * @brief AI node private data
 */
typedef struct {
    device_t *ai_device;                  // AI device handle
    video_ai_config_t config;             // AI configuration
    video_ai_stats_t stats;               // AI statistics
    nn_model_info_t model_info;           // NN model information
    uint32_t frame_counter;               // Frame counter for interval processing
    uint32_t last_inference_tick;         // Tick of the last inference attempt (pacing)
    video_ai_result_callback_t result_callback; // Post-inference result callback
    void *result_callback_user_data;      // Opaque pointer for result_callback
    /* Frame hook: runs between a successful primary inference and the pipe2
     * buffer return, with raw pixel access (e.g. face-recognition embedding
     * on crops of the frame the detection came from). */
    video_ai_frame_hook_t frame_hook;     // Raw-frame consumer (NULL = none)
    void *frame_hook_user_data;           // Opaque pointer for frame_hook
    uint8_t *current_buffer;              // Current frame buffer
    /* Letterbox staging buffer (model_w x model_h RGB888): PIPE2 delivers
     * the aspect-true full-frame rows (model_w x eff_h) and the pad rows
     * top/bottom are filled here before every inference. NULL when the
     * model geometry fills PIPE2 exactly (no letterbox needed). */
    uint8_t *lb_buffer;                   // Letterbox staging buffer
    uint32_t lb_eff_h;                    // Valid rows in the PIPE2 frame
    uint32_t lb_pad_top;                  // Pad rows above the valid rows
    uint32_t lb_size;                     // Full model input buffer size
    aicam_bool_t is_initialized;          // Initialization status
    aicam_bool_t is_running;              // Running status
    aicam_bool_t draw_service_initialized; // Drawing service initialization status
    
    // NN result cache (circular queue with 3 buffers)
    nn_result_with_frame_id_t nn_result_cache[NN_RESULT_CACHE_SIZE];
    uint32_t write_index;                   // Write index for circular queue
    uint32_t read_index;                    // Read index for circular queue  
    uint32_t cache_count;                   // Number of items in cache (0-NN_RESULT_CACHE_SIZE)
    aicam_bool_t cache_initialized;
    osMutexId_t cache_mutex;
} video_ai_node_data_t;

/* ==================== API Functions ==================== */

/**
 * @brief Create AI node
 * @param name Node name
 * @param config AI configuration
 * @return AI node handle or NULL on error
 */
video_node_t* video_ai_node_create(const char *name, const video_ai_config_t *config);

/**
 * @brief Get default AI configuration
 * @param config Configuration structure to fill
 */
void video_ai_get_default_config(video_ai_config_t *config);

/**
 * @brief Set AI parameters
 * @param node AI node handle
 * @param config New configuration
 * @return Operation result
 */
aicam_result_t video_ai_node_set_config(video_node_t *node, const video_ai_config_t *config);

/**
 * @brief Get AI parameters
 * @param node AI node handle
 * @param config Configuration structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_config(video_node_t *node, video_ai_config_t *config);

/**
 * @brief Get AI statistics
 * @param node AI node handle
 * @param stats Statistics structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_stats(video_node_t *node, video_ai_stats_t *stats);

/**
 * @brief Reset AI statistics
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_reset_stats(video_node_t *node);

/**
 * @brief Start AI processing
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_start(video_node_t *node);

/**
 * @brief Stop AI processing
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_stop(video_node_t *node);

/**
 * @brief Check if AI is running
 * @param node AI node handle
 * @return Running status
 */
aicam_bool_t video_ai_node_is_running(video_node_t *node);

/**
 * @brief Load NN model
 * @param node AI node handle
 * @param model_ptr Model pointer
 * @return Operation result
 */
aicam_result_t video_ai_node_load_model(video_node_t *node, uintptr_t model_ptr);

/**
 * @brief Unload NN model
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_unload_model(video_node_t *node);

/**
 * @brief Get NN model information
 * @param node AI node handle
 * @param model_info Model info structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_model_info(video_node_t *node, nn_model_info_t *model_info);

/**
 * @brief Reload AI model
 * @param node AI node handle
 * @return Operation result
 */
aicam_result_t video_ai_node_reload_model(video_node_t *node);

/**
 * @brief Get oldest NN result from cache (FIFO)
 * @param node AI node handle
 * @param result NN result structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_nn_result(video_node_t *node, nn_result_t *result);

/**
 * @brief Get latest NN result from cache (newest, non-destructive)
 * @param node AI node handle
 * @param result NN result structure to fill
 * @return Operation result
 */
aicam_result_t video_ai_node_get_best_nn_result(video_node_t *node, nn_result_t *result, uint32_t frame_id);

/**
 * @brief Register a callback invoked after each successful inference
 * @param node AI node handle
 * @param callback Callback function (NULL to unregister)
 * @param user_data Opaque pointer passed to the callback
 * @return Operation result
 */
aicam_result_t video_ai_node_set_result_callback(video_node_t *node,
                                                 video_ai_result_callback_t callback,
                                                 void *user_data);

/**
 * @brief Register a frame hook with raw pipe2 pixel access
 * @details The hook runs on the AI node thread after each successful primary
 *          inference, before the pipe2 buffer is recycled. Only one hook slot
 *          exists (services that need fan-out should install their own
 *          dispatcher). Passing NULL unregisters.
 * @param node AI node handle
 * @param hook Hook function (NULL to unregister)
 * @param user_data Opaque pointer passed to the hook
 * @return Operation result
 */
aicam_result_t video_ai_node_set_frame_hook(video_node_t *node,
                                            video_ai_frame_hook_t hook,
                                            void *user_data);

/**
 * @brief Get the pipe2 input geometry (the primary model's input size)
 * @details The frame hook receives pixels in this geometry; crop/warp code
 *          needs it to interpret coordinates.
 * @param node AI node handle
 * @param width Input width out (0 when no model loaded)
 * @param height Input height out (0 when no model loaded)
 * @return Operation result
 */
aicam_result_t video_ai_node_get_input_geometry(video_node_t *node,
                                                uint32_t *width, uint32_t *height);

/* ==================== Control Commands ==================== */

#define AI_CMD_START_PROCESSING           0x3001
#define AI_CMD_STOP_PROCESSING            0x3002
#define AI_CMD_SET_CONFIDENCE             0x3003
#define AI_CMD_SET_MAX_DETECTIONS         0x3004
#define AI_CMD_LOAD_MODEL                 0x3005
#define AI_CMD_UNLOAD_MODEL               0x3006
#define AI_CMD_GET_MODEL_INFO             0x3007
#define AI_CMD_ENABLE_DRAWING             0x3008
#define AI_CMD_DISABLE_DRAWING            0x3009
#define AI_CMD_SET_DRAW_CONFIG            0x300A
#define AI_CMD_GET_DRAW_CONFIG            0x300B

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_AI_NODE_H */