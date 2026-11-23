/**
 * @file video_ai_node.c
 * @brief Video AI Processing Node Implementation
 * @details AI inference node based on nn.h/nn.c device interaction with zero-copy
 */

#include "video_ai_node.h"
#include "video_frame_mgr.h"
#include "debug.h"
#include "buffer_mgr.h"
#include "nn.h"
#include "ai_draw_service.h"
#include <string.h>
#include <stdio.h>
#include "mem_map.h"
#include "cmsis_os2.h"
#include "upgrade_manager.h"
#include "drtc.h"
#include "json_config_mgr.h"
#include "image_utils.h"

/* NN bridge R/B byte order: DMA2D's RGB888 output packs logical R,G,B while
 * the model's historical input stream was B,G,R (pipe2 RGB888 + swap). 1
 * restores the historical order; set 0 if detection quality regresses
 * (empirically verifiable with a face-detection check). */
#define NN_BRIDGE_SWAP_RB 1


/* ==================== Internal Function Declarations ==================== */
static aicam_result_t video_ai_node_load_model_active(video_node_t *node);
static aicam_result_t video_ai_node_init_callback(video_node_t *node);
static aicam_result_t video_ai_node_deinit_callback(video_node_t *node);
static aicam_result_t video_ai_node_process_callback(video_node_t *node,
                                                    video_frame_t **input_frames,
                                                    uint32_t input_count,
                                                    video_frame_t **output_frames,
                                                    uint32_t *output_count);
static aicam_result_t video_ai_node_control_callback(video_node_t *node,
                                                    uint32_t cmd,
                                                    void *param);

static aicam_result_t video_ai_start_device(video_ai_node_data_t *data);
static aicam_result_t video_ai_stop_device(video_ai_node_data_t *data);
static aicam_result_t video_ai_process_frame(video_ai_node_data_t *data, 
                                            video_frame_t **output_frame);
static aicam_result_t video_ai_init_draw_service(video_ai_node_data_t *data);
static aicam_result_t video_ai_deinit_draw_service(video_ai_node_data_t *data);


/* ==================== API Implementation ==================== */


void video_ai_get_default_config(video_ai_config_t *config) {
    if (!config) return;
    
    memset(config, 0, sizeof(video_ai_config_t));
    config->width = PIPE1_DEFAULT_WIDTH;
    config->height = PIPE1_DEFAULT_HEIGHT;
    config->fps = CAMERA_FPS;
    config->input_format = PIPE1_DEFAULT_FORMAT;
    config->bpp = PIPE1_DEFAULT_BPP;
    config->confidence_threshold = 50;
    config->nms_threshold = 50;
    config->max_detections = 32;
    config->processing_interval = 1;  // Process every frame
    config->enabled = AICAM_TRUE;
    config->overlay_results = AICAM_TRUE;  // Overlay on by default (stock behavior)
    config->enable_drawing = AICAM_TRUE;
    
    // Initialize drawing configuration
    ai_draw_get_default_config(&config->draw_config);
}

video_node_t* video_ai_node_create(const char *name, const video_ai_config_t *config) {
    if (!name || !config) {
        LOG_CORE_ERROR("Invalid parameters for AI node creation");
        return NULL;
    }
    
    // Create node
    video_node_t *node = video_node_create(name, VIDEO_NODE_TYPE_SOURCE);
    if (!node) {
        LOG_CORE_ERROR("Failed to create AI node");
        return NULL;
    }
    
    // Allocate private data
    video_ai_node_data_t *data = buffer_calloc(1, sizeof(video_ai_node_data_t));
    if (!data) {
        LOG_CORE_ERROR("Failed to allocate AI node data");
        video_node_destroy(node);
        return NULL;
    }
    
    // Initialize private data
    memset(data, 0, sizeof(video_ai_node_data_t));
    memcpy(&data->config, config, sizeof(video_ai_config_t));
    
    // Initialize NN result cache (circular queue)
    memset(data->nn_result_cache, 0, sizeof(data->nn_result_cache));
    data->write_index = 0;
    data->read_index = 0;
    data->cache_count = 0;
    data->cache_initialized = AICAM_FALSE;
    
    // Create cache mutex
    data->cache_mutex = osMutexNew(NULL);
    if (!data->cache_mutex) {
        LOG_CORE_ERROR("Failed to create cache mutex");
        buffer_free(data);
        video_node_destroy(node);
        return NULL;
    }
    
    // Set node callbacks
    video_node_callbacks_t callbacks = {
        .init = video_ai_node_init_callback,
        .deinit = video_ai_node_deinit_callback,
        .process = video_ai_node_process_callback,
        .control = video_ai_node_control_callback
    };
    
    aicam_result_t result = video_node_set_callbacks(node, &callbacks);
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to set AI node callbacks");
        buffer_free(data);
        video_node_destroy(node);
        return NULL;
    }
    
    // Set private data
    result = video_node_set_private_data(node, data);
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to set AI node private data");
        buffer_free(data);
        video_node_destroy(node);
        return NULL;
    }
    
    LOG_CORE_INFO("AI node created: %s", name);
    return node;
}



aicam_result_t video_ai_node_set_config(video_node_t *node, const video_ai_config_t *config) {
    if (!node || !config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(&data->config, config, sizeof(video_ai_config_t));

    
    LOG_CORE_INFO("AI node config updated");
    return AICAM_OK;
}

aicam_result_t video_ai_node_get_config(video_node_t *node, video_ai_config_t *config) {
    if (!node || !config) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(config, &data->config, sizeof(video_ai_config_t));
    return AICAM_OK;
}

aicam_result_t video_ai_node_get_stats(video_node_t *node, video_ai_stats_t *stats) {
    if (!node || !stats) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(stats, &data->stats, sizeof(video_ai_stats_t));
    return AICAM_OK;
}

aicam_result_t video_ai_node_reset_stats(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memset(&data->stats, 0, sizeof(video_ai_stats_t));
    LOG_CORE_INFO("AI node statistics reset");
    return AICAM_OK;
}

aicam_result_t video_ai_node_start(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    if (data->is_running) {
        LOG_CORE_WARN("AI node already running");
        return AICAM_OK;
    }
    
    aicam_result_t result = video_ai_start_device(data);
    if (result == AICAM_OK) {
        data->is_running = AICAM_TRUE;
        LOG_CORE_INFO("AI node started");
    }
    
    return result;
}

aicam_result_t video_ai_node_stop(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    if (!data->is_running) {
        LOG_CORE_WARN("AI node not running");
        return AICAM_OK;
    }
    
    aicam_result_t result = video_ai_stop_device(data);
    if (result == AICAM_OK) {
        data->is_running = AICAM_FALSE;
        LOG_CORE_INFO("AI node stopped");
    }
    
    return result;
}

aicam_bool_t video_ai_node_is_running(video_node_t *node) {
    if (!node) {
        return AICAM_FALSE;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_FALSE;
    }
    
    return data->is_running;
}

aicam_result_t video_ai_node_load_model(video_node_t *node, uintptr_t model_ptr) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    if(model_ptr == 0) {
        LOG_CORE_INFO("Load active AI model");
        aicam_result_t result = video_ai_node_load_model_active(node);
        if (result != AICAM_OK) {
            LOG_CORE_ERROR("Failed to load active AI model: %d", result);
            return result;
        }
        return AICAM_OK;
    }
    
    int nn_ret = nn_load_model(model_ptr);
    if (nn_ret == 0) {
        // Get model information
        nn_get_model_info(&data->model_info);
        LOG_CORE_INFO("AI model loaded: %s", data->model_info.name);
        json_config_sync_ai_pipe_nvs_from_input_size(data->model_info.input_width, data->model_info.input_height);
        return AICAM_OK;
    } else {
        LOG_CORE_ERROR("Failed to load AI model: %d", nn_ret);
        return AICAM_ERROR;
    }
}

aicam_result_t video_ai_node_unload_model(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    int nn_ret = nn_unload_model();
    if (nn_ret == 0) {
        memset(&data->model_info, 0, sizeof(nn_model_info_t));
        LOG_CORE_INFO("AI model unloaded");
        return AICAM_OK;
    } else {
        LOG_CORE_ERROR("Failed to unload AI model: %d", nn_ret);
        return AICAM_ERROR;
    }
}

aicam_result_t video_ai_node_get_model_info(video_node_t *node, nn_model_info_t *model_info) {
    if (!node || !model_info) {
        LOG_CORE_ERROR("Invalid parameters for AI node get model info");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid parameters for AI node get model info");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    memcpy(model_info, &data->model_info, sizeof(nn_model_info_t));
    return AICAM_OK;
}

aicam_result_t video_ai_node_reload_model(video_node_t *node) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    aicam_result_t nn_ret = video_ai_node_unload_model(node);
    if (nn_ret != AICAM_OK) {
        LOG_CORE_ERROR("Failed to unload AI model: %d", nn_ret);
        return nn_ret;
    }

    //reset cache   
    memset(data->nn_result_cache, 0, sizeof(data->nn_result_cache));
    data->write_index = 0;
    data->read_index = 0;
    data->cache_count = 0;
    data->cache_initialized = AICAM_FALSE;

    nn_ret = video_ai_node_load_model(node, 0);
    if (nn_ret != AICAM_OK) {
        LOG_CORE_ERROR("Failed to load AI model: %d", nn_ret);
        return nn_ret;
    }

    LOG_CORE_INFO("AI model reloaded");

    video_ai_node_init_callback(node);

    return AICAM_OK;
}

aicam_result_t video_ai_node_get_nn_result(video_node_t *node, nn_result_t *result) {
    if (!node || !result) {
        LOG_CORE_ERROR("Invalid parameters for AI node get nn result");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid AI node data");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Lock cache mutex
    osStatus_t mutex_status = osMutexAcquire(data->cache_mutex, osWaitForever); 
    if (mutex_status != osOK) {
        LOG_CORE_ERROR("Failed to lock cache mutex: %d", mutex_status);
        return AICAM_ERROR;
    }
    
    // Check if cache is empty
    if (data->cache_count == 0) {
        // Release mutex first
        osMutexRelease(data->cache_mutex);
        LOG_CORE_DEBUG("NN result cache is empty");
        result->od.nb_detect = 0;
        return AICAM_OK;
    }
    
    // Copy the oldest result (FIFO) - extract nn_result_t from nn_result_with_frame_id_t
    memcpy(result, &data->nn_result_cache[data->read_index].result, sizeof(nn_result_t));
    
    // Move read index (circular)
    data->read_index = (data->read_index + 1) % NN_RESULT_CACHE_SIZE;
    data->cache_count--;
    
    // Release cache mutex
    osMutexRelease(data->cache_mutex);
    
    LOG_CORE_DEBUG("Retrieved NN result from cache: %d detections", result->od.nb_detect);
    return AICAM_OK;
}

aicam_result_t video_ai_node_get_best_nn_result(video_node_t *node, nn_result_t *result, uint32_t frame_id) {
    if (!node || !result) {
        LOG_CORE_ERROR("Invalid parameters for AI node get latest nn result");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid AI node data");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Lock cache mutex
    osStatus_t mutex_status = osMutexAcquire(data->cache_mutex, osWaitForever); 
    if (mutex_status != osOK) {
        LOG_CORE_ERROR("Failed to lock cache mutex: %d", mutex_status);
        return AICAM_ERROR;
    }
    
    // Check if cache is empty
    if (data->cache_count == 0) {
        // Release mutex first
        osMutexRelease(data->cache_mutex);
        LOG_CORE_DEBUG("NN result cache is empty");
        result->od.nb_detect = 0;
        return AICAM_OK;
    }

    uint32_t best_index = 0;
    uint32_t min_diff = UINT32_MAX;

    for (uint32_t i = 0; i < data->cache_count; i++)
    {
        uint32_t index = (data->write_index + NN_RESULT_CACHE_SIZE - data->cache_count + i) % NN_RESULT_CACHE_SIZE;
        nn_result_with_frame_id_t *entry = &data->nn_result_cache[index];

        uint32_t diff = (entry->frame_id > frame_id) ? (entry->frame_id - frame_id) : (frame_id - entry->frame_id);

        if (diff < min_diff)
        {
            min_diff = diff;
            best_index = index;
        }
    }

    // Copy best match result
    memcpy(result, &data->nn_result_cache[best_index].result, sizeof(nn_result_t));

    // Release cache mutex (no index changes for non-destructive read)
    osMutexRelease(data->cache_mutex);

    //LOG_CORE_DEBUG("Retrieved latest NN result from cache: %d detections", result->od.nb_detect);
    return AICAM_OK;
}

aicam_result_t video_ai_node_set_result_callback(video_node_t *node,
                                                 video_ai_result_callback_t callback,
                                                 void *user_data)
{
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    data->result_callback = callback;
    data->result_callback_user_data = user_data;
    return AICAM_OK;
}

/* ==================== Internal Functions ==================== */

static aicam_result_t video_ai_start_device(video_ai_node_data_t *data) {
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Check NN state
    nn_state_t nn_state = nn_get_state();
    if (nn_state == NN_STATE_READY) {
        // Start NN inference
        int nn_ret = nn_start_inference();
        if (nn_ret != 0) {
            LOG_CORE_ERROR("Failed to start NN inference: %d", nn_ret);
            return AICAM_ERROR;
        }
        //LOG_CORE_INFO("NN inference started");
        data->is_running = AICAM_TRUE;
    } else if (nn_state == NN_STATE_UNINIT) {
        LOG_CORE_WARN("NN module not initialized, AI will work in pass-through mode");
    } else {
        LOG_CORE_INFO("NN state: %d", nn_state);
    }
    
    return AICAM_OK;
}

static aicam_result_t video_ai_stop_device(video_ai_node_data_t *data) {
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    LOG_CORE_INFO("AI node stop device");

    // if(!data->config.enabled) {
    //     return AICAM_OK;
    // }

    //unload model
    nn_state_t nn_state = nn_get_state();
    
    // Stop NN inference
    if (nn_state == NN_STATE_RUNNING) {
        int nn_ret = nn_stop_inference();
        if (nn_ret != 0) {
            LOG_CORE_ERROR("Failed to stop NN inference: %d", nn_ret);
            return AICAM_ERROR;
        }
        LOG_CORE_INFO("NN inference stopped");
    }

    // nn_state = nn_get_state(); 
    // if(nn_state == NN_STATE_RUNNING || nn_state == NN_STATE_READY) {
    //     nn_unload_model();
    // }
    
    return AICAM_OK;
}

static aicam_result_t video_ai_process_frame(video_ai_node_data_t *data,
                                             video_frame_t **output_frame)
{
    if (!data || !output_frame)
    {
        return AICAM_ERROR_INVALID_PARAM;
    }

    /* NOTE: the AI toggle (config.enabled) intentionally does NOT gate
     * inference here. Original semantics (product decision): /ai/toggle only
     * switches the drawing path (camera node's ai_enabled && overlay_results
     * check) while inference keeps running - other consumers of AI results
     * (capture AI filter, result cache) must not be affected. It was briefly
     * a real inference switch during the tearing investigation, then
     * reverted. Inference itself paces this loop; the NN-not-ready and
     * bridge-mismatch branches below carry their own pacing. */

    // Pace inference to the configured interval. Sleep in bounded slices so
    // config changes and pipeline stop are picked up within ~100 ms, and
    // never inside a camera ioctl (it holds the camera mutex while blocking).
    if (data->config.inference_interval_ms != 0)
    {
        uint32_t elapsed_ms = osKernelGetTickCount() - data->last_inference_tick;
        if (elapsed_ms < data->config.inference_interval_ms)
        {
            uint32_t remaining_ms = data->config.inference_interval_ms - elapsed_ms;
            osDelay(remaining_ms < 100 ? remaining_ms : 100);
            *output_frame = NULL;
            return AICAM_OK;
        }
    }

    // Check processing interval
    if (data->config.processing_interval > 1)
    {
        data->frame_counter++;
        if (data->frame_counter % data->config.processing_interval != 0)
        {
            // Skip this frame
            *output_frame = NULL;
            data->stats.frames_skipped++;
            return AICAM_OK;
        }
    }

    // Check NN state
    nn_state_t nn_state = nn_get_state();
    if (nn_state != NN_STATE_READY && nn_state != NN_STATE_RUNNING)
    {
        LOG_CORE_WARN("NN not ready (state=%d), passing through frame", nn_state);
        /* Pace this branch: it returns without any GET/inference work, so
         * without a delay the SOURCE-type node busy-spins while the model
         * is missing/unloaded (same CPU-starvation class as the paced
         * bridge-mismatch branch below). */
        osDelay(20);
        *output_frame = NULL;
        data->stats.frames_skipped++;
        return AICAM_OK;
    }

    /* Input-format gate + NN bridge decision:
     * - exact size match (PIPE1-preview mode: pipe2 RGB888 == model input):
     *   feed the raw pipe2 buffer directly - the historical path, unchanged.
     * - PIPE2-preview mode (pipe2 ARGB8888, packed-RGB888 model of the same
     *   geometry): convert a COPY via DMA2D (~5 ms) and feed the model from
     *   it - the NN bridge. The raw pipe2 buffer is returned immediately
     *   after the conversion (shortest hold).
     * - anything else: idle (paced, logged once) - without this gate the node
     *   would spin at full speed, win every READY-buffer race and starve the
     *   preview camera node of pipe2 frames entirely. */
    uint32_t model_bytes = nn_expected_input_size();
    uint32_t pipe2_frame_bytes =
        (uint32_t)data->config.width * data->config.height * data->config.bpp;
    uint32_t pipe2_rgb888_bytes =
        (uint32_t)data->config.width * data->config.height * 3;
    aicam_bool_t use_bridge = AICAM_FALSE;
    uint32_t bridge_src_fmt = 0;
    if (model_bytes != 0) {
        if (model_bytes == pipe2_frame_bytes) {
            use_bridge = AICAM_FALSE;                    /* direct feed */
        } else if (data->config.bpp == 4 && model_bytes == pipe2_rgb888_bytes) {
            use_bridge = AICAM_TRUE;                     /* NN bridge: ARGB8888 */
            bridge_src_fmt = DMA2D_INPUT_ARGB8888;
        } else if (data->config.bpp == 2 && model_bytes == pipe2_rgb888_bytes) {
            use_bridge = AICAM_TRUE;                     /* NN bridge: RGB565 */
            bridge_src_fmt = DMA2D_INPUT_RGB565;
        } else {
            static uint8_t mismatch_logged = 0;
            if (!mismatch_logged) {
                mismatch_logged = 1;
                LOG_CORE_WARN("AI idle: pipe2 frame %lux%lux%lu=%lu B != model input %lu B",
                              (unsigned long)data->config.width,
                              (unsigned long)data->config.height,
                              (unsigned long)data->config.bpp,
                              (unsigned long)pipe2_frame_bytes,
                              (unsigned long)model_bytes);
            }
            data->last_inference_tick = osKernelGetTickCount();
            /* Pace the idle loop: without the GET/inference to throttle it,
             * this branch returns instantly and the node busy-spins at
             * ~10000 loops/s, starving the preview threads of CPU. */
            osDelay(20);
            *output_frame = NULL;
            data->stats.frames_skipped++;
            return AICAM_OK;
        }
    }

    // get input buffer from pipe2 (use cached device handle)
    device_t *camera_dev = device_find_pattern(CAMERA_DEVICE_NAME, DEV_TYPE_VIDEO);
    if (!camera_dev)
    {
        LOG_CORE_ERROR("Camera device not found");
        *output_frame = NULL;
        return AICAM_ERROR;
    }

    uint8_t *input_frame_buffer = NULL;
    uint32_t frame_id = 0;
    camera_buffer_with_frame_id_t camera_buffer_with_frame_id = {0};
    int result = device_ioctl(camera_dev, CAM_CMD_GET_PIPE2_BUFFER_WITH_FRAME_ID,
                            (uint8_t *)&camera_buffer_with_frame_id, 0);


    if (result == AICAM_OK)
    {
        // Reduce logging frequency - only log every 200 frames
        input_frame_buffer = camera_buffer_with_frame_id.buffer;
        frame_id = camera_buffer_with_frame_id.frame_id;
        static uint32_t frame_log_count = 0;
        if ((++frame_log_count % 200) == 1)
        {
            LOG_CORE_DEBUG("Got pipe2 buffer for AI processing: %p (frame %u)", input_frame_buffer, frame_log_count);
        }
    }
    else if (result == AICAM_ERROR_NOT_FOUND)
    {
        *output_frame = NULL;
        return AICAM_OK;
    }
    else if (result == AICAM_ERROR_NOT_SUPPORTED)
    {
        /* Camera pipes are stopped (e.g. an interrupted OTA upload stopped the
         * device and its cleanup path never ran). That is a persistent state
         * problem, not a per-frame failure: anchor the pacing tick + yield so
         * the node thread cannot busy-spin, log rate-limited (once per 5 s),
         * and report OK-no-frame instead of erroring so the pipeline error log
         * is not flooded at the retry rate. ai_pipeline_start() normally
         * restarts the camera before this state can be observed. */
        static uint32_t pipe_stopped_log_tick = 0;
        uint32_t now = osKernelGetTickCount();
        if (pipe_stopped_log_tick == 0 || (now - pipe_stopped_log_tick) >= 5000U) {
            pipe_stopped_log_tick = now;
            LOG_CORE_ERROR("Camera pipe2 stopped, AI inference idle until camera restart");
        }
        data->last_inference_tick = now;
        osDelay(5);
        *output_frame = NULL;
        data->stats.frames_skipped++;
        return AICAM_OK;
    }
    else
    {
        LOG_CORE_ERROR("Failed to get pipe2 buffer for AI processing, size: %d", camera_buffer_with_frame_id.size);
        // Camera pipe not producing (stopped/deinit'ed): yield so the node thread
        // cannot busy-spin against a dead camera, and anchor the pacing tick so
        // retries keep the configured inference cadence instead of bypassing it
        // (the gate at the top of this function only sleeps after an anchor).
        data->last_inference_tick = osKernelGetTickCount();
        osDelay(5);
        *output_frame = NULL;
        return AICAM_ERROR;
    }

    // Prepare NN result structure (use stack allocation for temporary result)
    nn_result_t nn_result;
    memset(&nn_result, 0, sizeof(nn_result_t));

    /* NN bridge (PIPE2-preview mode): pipe2 delivers ARGB8888, the model
     * eats packed RGB888. Convert a copy via DMA2D (~5 ms at 320x320) and
     * hand the RAW pipe2 buffer straight back - the shortest possible hold,
     * and the model runs on a stable private copy. (nn.c also bridges as a
     * safety net for other callers; this pre-conversion keeps the raw
     * buffer's hold minimal.) The DMA2D full-frame read of the pool is the
     * access pattern that can disturb in-flight DCMIPP writes (~0.3% of
     * frames under hard motion, no error flags) - accepted here: inference
     * is paced (not per-frame), garbage inputs are absorbed by the result
     * cache smoothing. */
    uint8_t *nn_input = input_frame_buffer;
    uint32_t nn_input_size = camera_buffer_with_frame_id.size;
    if (use_bridge) {
        static uint8_t *bridge_buf = NULL;   /* packed RGB888, model-sized */
        if (bridge_buf == NULL) {
            bridge_buf = buffer_calloc(1, model_bytes);
        }
        aicam_bool_t bridged = AICAM_FALSE;
        if (bridge_buf != NULL &&
            image_bridge_to_rgb888(input_frame_buffer,
                                   data->config.width,
                                   data->config.height,
                                   bridge_src_fmt, bridge_buf,
                                   NN_BRIDGE_SWAP_RB) == AICAM_OK) {
            nn_input = bridge_buf;
            nn_input_size = model_bytes;
            bridged = AICAM_TRUE;
        }
        /* The raw buffer is not needed beyond the conversion either way. */
        device_ioctl(camera_dev, CAM_CMD_RETURN_PIPE2_BUFFER, input_frame_buffer, 0);
        if (!bridged) {
            /* Bridge failure (alloc/DMA2D): skip this round, keep cadence. */
            static uint8_t bridge_fail_logged = 0;
            if (!bridge_fail_logged) {
                bridge_fail_logged = 1;
                LOG_CORE_WARN("NN bridge conversion failed - inference skipped");
            }
            data->last_inference_tick = osKernelGetTickCount();
            *output_frame = NULL;
            data->stats.frames_skipped++;
            return AICAM_OK;
        }
    }

    // Call NN module to process frame
    uint32_t infer_start_tick = osKernelGetTickCount();
    int nn_ret = nn_inference_frame(nn_input, nn_input_size, &nn_result);
    uint32_t inference_time_ms = osKernelGetTickCount() - infer_start_tick;
    // Anchor pacing to the attempt so inference errors keep the configured cadence
    data->last_inference_tick = infer_start_tick;

    // return pipe2 buffer (bridge path already returned it above)
    if (!use_bridge) {
        device_ioctl(camera_dev, CAM_CMD_RETURN_PIPE2_BUFFER, input_frame_buffer, 0);
    }

    /* Pace the failed-inference loop: nn rejects instantly (e.g. geometry
     * mismatch after a hot model swap), and without this delay the node
     * busy-spins and drags the preview fps down with it. */
    if (nn_ret != 0) {
        osDelay(20);
    }

    // Save result to cache if inference was successful
    if (nn_ret == 0)
    {
        /* Keep the node statistics alive: /ai/status exposes them and they
         * are the remote evidence that inference keeps running regardless
         * of the /ai/toggle drawing switch. Lock-free on purpose (same as
         * the rest of the node stats) - diagnostics, not accounting. */
        {
            uint64_t proc_us = (uint64_t)inference_time_ms * 1000u;
            uint32_t nb = 0;
            if (nn_result.type == PP_TYPE_OD)       nb = nn_result.od.nb_detect;
            else if (nn_result.type == PP_TYPE_MPE) nb = nn_result.mpe.nb_detect;
            else if (nn_result.type == PP_TYPE_ISEG) nb = nn_result.iseg.nb_detect;
            data->stats.frames_processed++;
            data->stats.avg_processing_time_us = (data->stats.avg_processing_time_us == 0)
                ? proc_us
                : (data->stats.avg_processing_time_us * 3 + proc_us) / 4;
            if (proc_us > data->stats.max_processing_time_us) {
                data->stats.max_processing_time_us = proc_us;
            }
            data->stats.detections_found += nb;
            data->stats.current_detection_count = nb;
        }

        // Optimized cache write with reduced lock time
        uint32_t current_write_idx = data->write_index;

        // Prepare result with timestamp and frame_id
        nn_result_with_frame_id_t result_with_ts;
        memcpy(&result_with_ts.result, &nn_result, sizeof(nn_result_t));
        result_with_ts.frame_id = frame_id;

        // Copy result to cache before acquiring lock (reduce critical section)
        memcpy(&data->nn_result_cache[current_write_idx], &result_with_ts, sizeof(nn_result_with_frame_id_t));

        // Lock cache mutex for minimal time
        osStatus_t mutex_status = osMutexAcquire(data->cache_mutex, 10); // Reduced timeout
        if (mutex_status == osOK)
        {
            // Update indices atomically
            data->write_index = (data->write_index + 1) % NN_RESULT_CACHE_SIZE;

            if (data->cache_count >= NN_RESULT_CACHE_SIZE)
            {
                data->read_index = (data->read_index + 1) % NN_RESULT_CACHE_SIZE;
            }
            else
            {
                data->cache_count++;
            }

            if (!data->cache_initialized)
            {
                data->cache_initialized = AICAM_TRUE;
            }

            osMutexRelease(data->cache_mutex);
        }
        else
        {
            // If lock fails, just log once to avoid spam
            static uint32_t lock_fail_count = 0;
            if ((++lock_fail_count % 100) == 1)
            {
                LOG_CORE_WARN("Cache mutex timeout, failed %lu times", lock_fail_count);
            }
        }

        // Hand the fresh result to the registered consumer while the
        // postprocess output buffers still hold this inference's data
        if (data->result_callback)
        {
            data->result_callback(&nn_result, frame_id, inference_time_ms,
                                  data->result_callback_user_data);
        }
    }

    // No output frame generated - results are cached internally
    *output_frame = NULL;

    return AICAM_OK;
}

/* ==================== Callback Functions ==================== */

static aicam_result_t video_ai_node_load_model_active(video_node_t *node) {
    LOG_CORE_INFO("AI node load model");
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    

    uintptr_t model_ptr = json_config_get_ai_1_active() ? AI_2_BASE + 1024 : AI_1_BASE + 1024;
    LOG_CORE_INFO("Load model from %p", model_ptr);
    int nn_ret = nn_load_model(model_ptr);
    if(nn_ret != 0) {
        LOG_CORE_ERROR("Failed to load model: %d", nn_ret);
        return AICAM_ERROR;
    }

    if(nn_get_model_info(&data->model_info) != AICAM_OK) {
        LOG_CORE_ERROR("Failed to get model info");
        return AICAM_ERROR;
    }

    json_config_sync_ai_pipe_nvs_from_input_size(data->model_info.input_width, data->model_info.input_height);

    LOG_CORE_INFO("AI model loaded: %dx%d from %p", data->model_info.input_width, data->model_info.input_height, model_ptr);
    return AICAM_OK;
}

static aicam_result_t video_ai_node_init_callback(video_node_t *node) {
    LOG_CORE_INFO("AI node init callback");
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        LOG_CORE_ERROR("Invalid AI node data");
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Find AI device
    data->ai_device = device_find_pattern("nn", DEV_TYPE_AI);
    if (!data->ai_device) {
        LOG_CORE_ERROR("AI device not found");
        return AICAM_ERROR;
    }
    
    // Check NN state
    nn_state_t nn_state = nn_get_state();
    if (nn_state == NN_STATE_READY || nn_state == NN_STATE_RUNNING) {
        LOG_CORE_INFO("NN module is ready, AI processing enabled");
        
        // Get model information if available
        nn_get_model_info(&data->model_info);
    }else if(nn_state == NN_STATE_INIT || nn_state == NN_STATE_UNINIT) {
        LOG_CORE_INFO("NN module is initialized, AI node will work in pass-through mode");
        // load active model
        aicam_result_t result = video_ai_node_load_model_active(node);
        if(result != AICAM_OK) {
            LOG_CORE_ERROR("Failed to load active model: %d", result);
            return result;
        }
    }

    aicam_result_t result = video_ai_start_device(data);
    if(result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to start AI device: %d", result);
        return result;
    }

    // set confidence threshold
    nn_set_confidence_threshold((float)data->config.confidence_threshold / 100.0f);
    nn_set_nms_threshold((float)data->config.nms_threshold / 100.0f);

    //set pipe2 config and cache camera device
    device_t* camera_dev = device_find_pattern(CAMERA_DEVICE_NAME, DEV_TYPE_VIDEO);
    if (!camera_dev) {
        LOG_CORE_ERROR("Camera device not found");
        return AICAM_ERROR;
    }
    pipe_params_t pipe_param = {0};
    /* EXPERIMENT H: honor the service-config geometry when it is set (the
     * stream mode dictates pipe2 size - e.g. the aligned-row 512x512 test);
     * only fall back to the model dims when the config left it open. The old
     * unconditional model-dims override stomped the configured geometry. */
    /* Geometry authority: in model-input mode (bpp==3, the non-pipe2-stream
     * formats) the freshly loaded model_info wins - after a HOT model swap
     * ai_pipeline_start reuses this node and config still carries the
     * previous model's size, which used to be re-applied to PIPE2 here
     * while the NPU already expects the new geometry ("nn: input size !=
     * model - inference disabled"). In pipe2-stream mode (bpp==2) the
     * service-config geometry (ai_pipe_size, synced from the model on
     * load) keeps precedence - EXPERIMENT H. */
    if (data->config.bpp == 3) {
        pipe_param.width = data->model_info.input_width;
        pipe_param.height = data->model_info.input_height;
    } else {
        pipe_param.width = (data->config.width != 0) ? data->config.width
                                                     : data->model_info.input_width;
        pipe_param.height = (data->config.height != 0) ? data->config.height
                                                       : data->model_info.input_height;
    }
    pipe_param.fps = data->config.fps;
    /* The service config IS the pipe2 format owner (it is the same source the
     * stream-params apply and the boot node creation use - EXPERIMENT A runs
     * it as RGB565/2, stock runs model RGB888/3). The old "keep the driver's
     * current format" logic kept the camera driver's cold-boot DEFAULT
     * (model RGB888/bpp3) on every boot without a UI stream-config apply:
     * pipe2 then delivered 3-byte frames while the camera/encoder nodes ran
     * 2-byte, and the encoder rejected every frame on size (ret=-2, zero
     * preview). Note DCMIPP_PIXEL_PACKER_FORMAT_RGB888_YUV444_1 == 0, so a
     * zero-init'd pipe_params_t ALSO reads as "RGB888" - never treat 0 as
     * "not configured" for this field. */
    pipe_param.format = data->config.input_format;
    pipe_param.bpp = data->config.bpp;
    /* Pool size follows the consumer count: in pipe2-STREAM mode (bpp==2)
     * the AI node and the PIPE2-streaming camera node are two concurrent
     * consumers - a 3-buffer pool destabilized the system (repeated
     * watchdog resets during the experiment), so 4 is mandatory there. In
     * model-input mode (bpp==3, PIPE1 preview) the AI node is the ONLY
     * consumer and 3 frames suffice (1 DCMIPP write + 1 inference hold +
     * 1 spare). */
    pipe_param.buffer_nb = (data->config.bpp == 3) ? 3 : 4;
    printf("Set pipe2 param: %dx%d@%dfps, format=%d, bpp=%d\r\n",
                 pipe_param.width, pipe_param.height, pipe_param.fps, pipe_param.format, pipe_param.bpp);
    device_ioctl(camera_dev, CAM_CMD_SET_PIPE2_PARAM, (uint8_t*)&pipe_param, sizeof(pipe_params_t));


    data->config.width = data->model_info.input_width;
    data->config.height = data->model_info.input_height;
    LOG_CORE_INFO("AI model loaded: %dx%d", data->config.width, data->config.height);

    uint8_t pipe_ctrl = CAMERA_CTRL_PIPE1_BIT | CAMERA_CTRL_PIPE2_BIT;
    device_ioctl(camera_dev, CAM_CMD_SET_PIPE_CTRL,
        &pipe_ctrl, 0);


    data->is_initialized = AICAM_TRUE;
    LOG_CORE_INFO("AI node initialized: %dx%d@%dfps, enabled=%d, drawing=%d, nn_state=%d",
                 data->config.width, data->config.height, data->config.fps,
                 data->config.enabled, data->config.enable_drawing, nn_state);
    
    return AICAM_OK;
}

static aicam_result_t video_ai_node_deinit_callback(video_node_t *node) {
    LOG_CORE_INFO("AI node deinit callback");
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }

    data->is_initialized = AICAM_FALSE;
    data->is_running = AICAM_FALSE;

    // Deinitialize drawing service
    if (data->draw_service_initialized) {
        video_ai_deinit_draw_service(data);
        data->draw_service_initialized = AICAM_FALSE;
    }

    // Stop device if running
    if (data->is_running) {
        video_ai_stop_device(data);
    }
    
    // Clean up cache mutex
    if (data->cache_mutex) {
        osMutexDelete(data->cache_mutex);
        data->cache_mutex = NULL;
    }
    
    // Clear cache
    data->cache_initialized = AICAM_FALSE;
    memset(data->nn_result_cache, 0, sizeof(data->nn_result_cache));
    
    LOG_CORE_INFO("AI node deinitialized");
    return AICAM_OK;
}

static aicam_result_t video_ai_node_process_callback(video_node_t *node,
                                                    video_frame_t **input_frames,
                                                    uint32_t input_count,
                                                    video_frame_t **output_frames,
                                                    uint32_t *output_count) {
    if (!node || !input_frames || !output_frames || !output_count) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data || !data->is_initialized) {
        return AICAM_OK;
    }

    //printf("AI node process callback\r\n");
    
    *output_count = 0;
    
    // Process AI inference
    video_frame_t *output_frame = NULL;
    aicam_result_t result = video_ai_process_frame(data, &output_frame);

    if (result != AICAM_OK) {
        LOG_CORE_ERROR("AI node process failed: %d", result);
        *output_count = 0;
        return result;
    }
    
    // AI node does not generate output frames - results are cached internally
    *output_count = 0;
    //LOG_CORE_INFO("AI node processing completed, results cached");

    return result;
}

static aicam_result_t video_ai_node_control_callback(video_node_t *node,
                                                    uint32_t cmd,
                                                    void *param) {
    if (!node) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    video_ai_node_data_t *data = (video_ai_node_data_t*)video_node_get_private_data(node);
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    switch (cmd) {
        case AI_CMD_START_PROCESSING:
            return video_ai_node_start(node);
            
        case AI_CMD_STOP_PROCESSING:
            return video_ai_node_stop(node);
            
        case AI_CMD_SET_CONFIDENCE:
            if (param) {
                uint32_t threshold = *(uint32_t*)param;
                data->config.confidence_threshold = threshold;
                
                // Update NN module
                nn_state_t nn_state = nn_get_state();
                if (nn_state == NN_STATE_READY || nn_state == NN_STATE_RUNNING) {
                    float confidence_threshold = (float)threshold / 100.0f;
                    nn_set_confidence_threshold(confidence_threshold);
                }
                
                LOG_CORE_INFO("AI confidence threshold set to %d", threshold);
                return AICAM_OK;
            }
            break;
            
        case AI_CMD_SET_MAX_DETECTIONS:
            if (param) {
                uint32_t max_detections = *(uint32_t*)param;
                data->config.max_detections = max_detections;
                
                // Update NN module
                nn_state_t nn_state = nn_get_state();
                if (nn_state == NN_STATE_READY || nn_state == NN_STATE_RUNNING) {
                    //nn_set_max_detection_count(max_detections);
                }
                
                LOG_CORE_INFO("AI max detections set to %d", max_detections);
                return AICAM_OK;
            }
            break;
            
        case AI_CMD_LOAD_MODEL:
            if (param) {
                uintptr_t model_ptr = *(uintptr_t*)param;
                return video_ai_node_load_model(node, model_ptr);
            }
            break;
            
        case AI_CMD_UNLOAD_MODEL:
            return video_ai_node_unload_model(node);
            
        case AI_CMD_GET_MODEL_INFO:
            if (param) {
                nn_model_info_t *model_info = (nn_model_info_t*)param;
                return video_ai_node_get_model_info(node, model_info);
            }
            break;
            
        case AI_CMD_ENABLE_DRAWING:
            data->config.enable_drawing = AICAM_TRUE;
            if (!data->draw_service_initialized) {
                aicam_result_t result = video_ai_init_draw_service(data);
                if (result == AICAM_OK) {
                    data->draw_service_initialized = AICAM_TRUE;
                    LOG_CORE_INFO("AI drawing enabled");
                } else {
                    LOG_CORE_ERROR("Failed to enable AI drawing: %d", result);
                    return result;
                }
            }
            return AICAM_OK;
            
        case AI_CMD_DISABLE_DRAWING:
            data->config.enable_drawing = AICAM_FALSE;
            if (data->draw_service_initialized) {
                video_ai_deinit_draw_service(data);
                data->draw_service_initialized = AICAM_FALSE;
                LOG_CORE_INFO("AI drawing disabled");
            }
            return AICAM_OK;
            
        case AI_CMD_SET_DRAW_CONFIG:
            if (param) {
                ai_draw_config_t *draw_config = (ai_draw_config_t*)param;
                memcpy(&data->config.draw_config, draw_config, sizeof(ai_draw_config_t));
                
                // Update drawing service configuration if initialized
                if (data->draw_service_initialized) {
                    aicam_result_t result = ai_draw_set_config(draw_config);
                    if (result != AICAM_OK) {
                        LOG_CORE_ERROR("Failed to update draw config: %d", result);
                        return result;
                    }
                }
                
                LOG_CORE_INFO("AI draw configuration updated");
                return AICAM_OK;
            }
            break;
            
        case AI_CMD_GET_DRAW_CONFIG:
            if (param) {
                ai_draw_config_t *draw_config = (ai_draw_config_t*)param;
                memcpy(draw_config, &data->config.draw_config, sizeof(ai_draw_config_t));
                return AICAM_OK;
            }
            break;
            
        default:
            LOG_CORE_WARN("Unknown AI control command: 0x%x", cmd);
            return AICAM_ERROR_INVALID_PARAM;
    }
    
    return AICAM_OK;
}

/* ==================== AI Drawing Service Integration ==================== */

static aicam_result_t video_ai_init_draw_service(video_ai_node_data_t *data)
{
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Update drawing configuration with current AI node settings
    data->config.draw_config.image_width = data->config.width;
    data->config.draw_config.image_height = data->config.height;
    
    // Initialize AI drawing service
    aicam_result_t result = ai_draw_service_init(&data->config.draw_config);
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to initialize AI draw service: %d", result);
        return result;
    }
    
    LOG_CORE_INFO("AI draw service initialized for %dx%d", 
                  data->config.width, data->config.height);
    
    return AICAM_OK;
}

static aicam_result_t video_ai_deinit_draw_service(video_ai_node_data_t *data)
{
    if (!data) {
        return AICAM_ERROR_INVALID_PARAM;
    }
    
    // Deinitialize AI drawing service
    aicam_result_t result = ai_draw_service_deinit();
    if (result != AICAM_OK) {
        LOG_CORE_ERROR("Failed to deinitialize AI draw service: %d", result);
        return result;
    }
    
    LOG_CORE_INFO("AI draw service deinitialized");
    
    return AICAM_OK;
}

