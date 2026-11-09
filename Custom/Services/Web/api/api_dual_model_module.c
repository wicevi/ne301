/**
 * @file api_dual_model_module.c
 * @brief Generic dual-model engine API module
 * @details REST surface of the Model A -> crop -> Model B engine:
 *          config (validated, NVS-persisted, applied live), status and a
 *          Model B reload (after flashing a new package to the slot).
 */

#include "web_api.h"
#include "video_dual_model.h"
#include "json_config_mgr.h"
#include "cJSON.h"
#include "debug.h"
#include <string.h>
#include <stdio.h>
#include "api_dual_model_module.h"
#include "web_server.h"
#include "storage.h"
#include "mem_map.h"

/* ==================== Helpers ==================== */

static cJSON *dual_model_config_to_json(const dual_model_config_t *cfg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", cfg->enabled);
    cJSON_AddNumberToObject(json, "target_conf_threshold", cfg->target_conf_threshold);
    cJSON_AddStringToObject(json, "target_class", cfg->target_class);
    cJSON_AddNumberToObject(json, "crop_expand_pm", cfg->crop_expand_pm);
    cJSON_AddNumberToObject(json, "crop_min_size", cfg->crop_min_size);
    cJSON_AddNumberToObject(json, "max_targets", cfg->max_targets);
    cJSON_AddNumberToObject(json, "interval_ms", cfg->interval_ms);
    cJSON_AddNumberToObject(json, "pad_value", cfg->pad_value);
    cJSON_AddStringToObject(json, "expected_postprocess", cfg->expected_postprocess);
    return json;
}

static cJSON *dual_model_status_to_json(const dual_model_status_t *st)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", st->enabled);
    cJSON_AddBoolToObject(json, "model_b_loaded", st->model_b_loaded);
    cJSON_AddStringToObject(json, "model_b_name", st->model_b_name);
    cJSON_AddNumberToObject(json, "model_b_input_width", st->model_b_input_w);
    cJSON_AddNumberToObject(json, "model_b_input_height", st->model_b_input_h);
    cJSON_AddStringToObject(json, "model_b_postprocess", st->model_b_postprocess);
    cJSON_AddNumberToObject(json, "model_b_addr", st->model_b_addr);
    cJSON_AddNumberToObject(json, "passes", st->passes);
    cJSON_AddNumberToObject(json, "inferences", st->inferences);
    cJSON_AddNumberToObject(json, "errors", st->errors);
    cJSON_AddNumberToObject(json, "last_pass_ms", st->last_pass_ms);
    cJSON_AddNumberToObject(json, "last_targets", st->last_targets);
    cJSON_AddNumberToObject(json, "avg_infer_ms", st->avg_infer_ms);
    cJSON_AddNumberToObject(json, "skipped_throttle", st->skipped_throttle);
    cJSON_AddNumberToObject(json, "jobs_dropped", st->jobs_dropped);
    cJSON_AddBoolToObject(json, "disabled_by_errors", st->disabled_by_errors);
    cJSON_AddStringToObject(json, "model_b_error", st->model_b_error);
    return json;
}

/* ==================== API Handlers ==================== */

/**
 * @brief GET /api/v1/dual-model/config
 */
static aicam_result_t dual_model_get_config_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    dual_model_config_t config;
    if (video_dual_model_get_config(&config) != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to get config");
    }

    cJSON *data = dual_model_config_to_json(&config);
    char *json_string = cJSON_Print(data);
    api_response_success(ctx, json_string, "Dual model config retrieved");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/dual-model/config  (partial merge, range-validated)
 */
static aicam_result_t dual_model_set_config_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    cJSON *request = web_api_parse_body(ctx);
    if (!request) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid JSON body");
    }

    /* Merge into the current config so omitted fields keep their values */
    dual_model_config_t config;
    if (video_dual_model_get_config(&config) != AICAM_OK) {
        cJSON_Delete(request);
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to get config");
    }

    cJSON *item;
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "enabled")) && cJSON_IsBool(item)) {
        config.enabled = cJSON_IsTrue(item) ? AICAM_TRUE : AICAM_FALSE;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "target_conf_threshold")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > 99) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "target_conf_threshold out of range (1..99)");
        }
        config.target_conf_threshold = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "target_class")) && cJSON_IsString(item)) {
        strncpy(config.target_class, item->valuestring, DM_CLASS_FILTER_MAX - 1u);
        config.target_class[DM_CLASS_FILTER_MAX - 1u] = '\0';
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "crop_expand_pm")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v > 500) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "crop_expand_pm out of range (0..500)");
        }
        config.crop_expand_pm = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "crop_min_size")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 8 || v > 1024) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "crop_min_size out of range (8..1024)");
        }
        config.crop_min_size = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "max_targets")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > 4) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "max_targets out of range (1..4)");
        }
        config.max_targets = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "interval_ms")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v > 10000) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "interval_ms out of range (0..10000)");
        }
        config.interval_ms = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "pad_value")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v > 255) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "pad_value out of range (0..255)");
        }
        config.pad_value = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "expected_postprocess")) && cJSON_IsString(item)) {
        /* Load contract; empty disables the check. A change only takes
         * effect on the next /dual-model/reload. */
        strncpy(config.expected_postprocess, item->valuestring, DM_POSTPROCESS_MAX - 1u);
        config.expected_postprocess[DM_POSTPROCESS_MAX - 1u] = '\0';
    }
    cJSON_Delete(request);

    /* Persist + apply live */
    if (json_config_set_dual_model_config(&config) != AICAM_OK) {
        LOG_SVC_WARN("Dual model config NVS persist failed (applying anyway)");
    }
    if (video_dual_model_set_config(&config) != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to apply config");
    }

    cJSON *data = dual_model_config_to_json(&config);
    char *json_string = cJSON_Print(data);
    api_response_success(ctx, json_string, "Dual model config updated");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief GET /api/v1/dual-model/status
 */
static aicam_result_t dual_model_status_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    dual_model_status_t status;
    if (video_dual_model_get_status(&status) != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to get status");
    }

    cJSON *data = dual_model_status_to_json(&status);
    char *json_string = cJSON_Print(data);
    api_response_success(ctx, json_string, "Dual model status retrieved");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/dual-model/reload  (re-load Model B after flashing)
 */
static aicam_result_t dual_model_reload_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    aicam_result_t result = video_dual_model_reload_model();
    if (result != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR,
                                  "No valid Model B package in the non-active AI slot");
    }

    dual_model_status_t status;
    video_dual_model_get_status(&status);
    cJSON *data = dual_model_status_to_json(&status);
    char *json_string = cJSON_Print(data);
    api_response_success(ctx, json_string, "Model B reloaded");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/dual-model/erase  - remove the Model B package
 * @details For single-model deployments: erases the leading blocks of the
 *          Model B (inactive) AI partition so the loader finds no package,
 *          then reloads - Model B unloads and the overlay degrades to the
 *          raw Model A boxes. The active Model A slot is never touched.
 */
static aicam_result_t dual_model_erase_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    /* Model B lives in the INACTIVE slot (same convention as the loader). */
    uint32_t slot_base = json_config_get_ai_1_active() ? AI_1_BASE : AI_2_BASE;
    uint32_t offset = slot_base - FLASH_BASE;

    /* Wipe the leading 64KB: the 1KB OTA header + the N6M1 package header +
     * the start of the weights - far more than enough for the loader's
     * package validation to reject the slot, while staying fast (~1s). */
    const size_t erase_blocks = 65536 / FLASH_BLOCK_SIZE;
    if (storage_flash_erase(offset, erase_blocks) != 0) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to erase the Model B slot");
    }
    LOG_SVC_INFO("Model B slot erased (offset 0x%08lX, %u blocks)",
                 (unsigned long)offset, (unsigned)erase_blocks);

    /* Hot reload: the engine now finds no package and unloads Model B. */
    (void)video_dual_model_reload_model();

    dual_model_status_t status;
    video_dual_model_get_status(&status);
    cJSON *data = dual_model_status_to_json(&status);
    char *json_string = cJSON_Print(data);
    api_response_success(ctx, json_string, "Model B removed");
    cJSON_Delete(data);
    return AICAM_OK;
}

/* ==================== Module Definition ==================== */

static const api_route_t dual_model_module_routes[] = {
    {
        .path = API_PATH_PREFIX "/dual-model/config",
        .method = "GET",
        .handler = dual_model_get_config_handler,
        .require_auth = AICAM_TRUE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/dual-model/config",
        .method = "POST",
        .handler = dual_model_set_config_handler,
        .require_auth = AICAM_TRUE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/dual-model/status",
        .method = "GET",
        .handler = dual_model_status_handler,
        .require_auth = AICAM_TRUE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/dual-model/erase",
        .method = "POST",
        .handler = dual_model_erase_handler,
        .require_auth = AICAM_TRUE,
        .user_data = NULL
    },
    {
        .path = API_PATH_PREFIX "/dual-model/reload",
        .method = "POST",
        .handler = dual_model_reload_handler,
        .require_auth = AICAM_TRUE,
        .user_data = NULL
    },
};

aicam_result_t web_api_register_dual_model_module(void) {
    LOG_CORE_INFO("Registering dual model module");

    for (size_t i = 0; i < sizeof(dual_model_module_routes) / sizeof(dual_model_module_routes[0]); i++) {
        aicam_result_t result = http_server_register_route(&dual_model_module_routes[i]);
        if (result != AICAM_OK) {
            LOG_CORE_ERROR("Failed to register dual model module: %d", result);
            return result;
        }
    }

    LOG_CORE_INFO("Dual model module registered successfully");
    return AICAM_OK;
}
