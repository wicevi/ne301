/**
 * @file api_face_recognition_module.c
 * @brief Face recognition demo API module
 * @details REST surface of the local face recognition demo riding on the
 *          dual-model engine: config, live track results, enrollment and
 *          the face template database. All recognition is local - no image
 *          ever leaves the device.
 */

#include "web_api.h"
#include "face_recognition.h"
#include "face_db.h"
#include "video_dual_model.h"
#include "json_config_mgr.h"
#include "cJSON.h"
#include "debug.h"
#include <string.h>
#include <stdio.h>
#include "api_face_recognition_module.h"
#include "web_server.h"

/* ==================== Helpers ==================== */

static cJSON *fr_config_to_json(const face_recognition_config_t *cfg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", cfg->enabled);
    cJSON_AddNumberToObject(json, "match_threshold", cfg->match_threshold);
    cJSON_AddNumberToObject(json, "confirm_votes", cfg->confirm_votes);
    cJSON_AddNumberToObject(json, "track_iou_threshold", cfg->track_iou_threshold);
    cJSON_AddNumberToObject(json, "track_max_missed", cfg->track_max_missed);
    cJSON_AddNumberToObject(json, "face_hold_ms", cfg->face_hold_ms);
    cJSON_AddNumberToObject(json, "enroll_samples", cfg->enroll_samples);
    cJSON_AddNumberToObject(json, "enroll_timeout_ms", cfg->enroll_timeout_ms);
    cJSON_AddNumberToObject(json, "enroll_min_similarity", cfg->enroll_min_similarity);
    cJSON_AddNumberToObject(json, "pose_max_yaw", cfg->pose_max_yaw);
    cJSON_AddNumberToObject(json, "pose_max_roll", cfg->pose_max_roll);
    cJSON_AddBoolToObject(json, "show_box", cfg->show_box);
    cJSON_AddBoolToObject(json, "show_name", cfg->show_name);
    cJSON_AddBoolToObject(json, "show_similarity", cfg->show_similarity);
    return json;
}

static const char *fr_status_str(fr_ident_status_t status)
{
    switch (status) {
        case FR_IDENT_KNOWN: return "known";
        case FR_IDENT_UNKNOWN: return "unknown";
        default: return "identifying";
    }
}

static const char *fr_enroll_state_str(fr_enroll_state_t state)
{
    switch (state) {
        case FR_ENROLL_COLLECTING: return "collecting";
        case FR_ENROLL_PAUSED: return "paused";
        case FR_ENROLL_DONE: return "done";
        case FR_ENROLL_FAILED_NO_FACE: return "failed_no_face";
        case FR_ENROLL_FAILED_INCONSISTENT: return "failed_inconsistent";
        case FR_ENROLL_FAILED_DB: return "failed_db";
        default: return "idle";
    }
}

/* ==================== API Handlers ==================== */

/**
 * @brief GET /api/v1/face-recognition/config
 */
static aicam_result_t fr_get_config_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    face_recognition_config_t config;
    if (face_recognition_get_config(&config) != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to get config");
    }

    cJSON *data = fr_config_to_json(&config);
    api_response_success(ctx, cJSON_Print(data), "Face recognition config retrieved");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/config  (partial merge, range-validated)
 */
static aicam_result_t fr_set_config_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    cJSON *request = web_api_parse_body(ctx);
    if (!request) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid JSON body");
    }

    face_recognition_config_t config;
    if (face_recognition_get_config(&config) != AICAM_OK) {
        cJSON_Delete(request);
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to get config");
    }

    cJSON *item;
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "enabled")) && cJSON_IsBool(item)) {
        config.enabled = cJSON_IsTrue(item) ? AICAM_TRUE : AICAM_FALSE;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "match_threshold")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > 99) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "match_threshold out of range (1..99)");
        }
        config.match_threshold = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "confirm_votes")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > 10) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "confirm_votes out of range (1..10)");
        }
        config.confirm_votes = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "track_iou_threshold")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > 99) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "track_iou_threshold out of range (1..99)");
        }
        config.track_iou_threshold = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "track_max_missed")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > 50) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "track_max_missed out of range (1..50)");
        }
        config.track_max_missed = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "face_hold_ms")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 100 || v > 2000) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "face_hold_ms out of range (100..2000)");
        }
        config.face_hold_ms = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "enroll_samples")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > FR_ENROLL_SAMPLES_MAX) {
            char msg[64];
            snprintf(msg, sizeof(msg), "enroll_samples out of range (1..%u)", FR_ENROLL_SAMPLES_MAX);
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, msg);
        }
        config.enroll_samples = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "enroll_timeout_ms")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1000 || v > 60000) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "enroll_timeout_ms out of range (1000..60000)");
        }
        config.enroll_timeout_ms = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "enroll_min_similarity")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 1 || v > 99) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "enroll_min_similarity out of range (1..99)");
        }
        config.enroll_min_similarity = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "pose_max_yaw")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 20 || v > 99) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "pose_max_yaw out of range (20..99)");
        }
        config.pose_max_yaw = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "pose_max_roll")) && cJSON_IsNumber(item)) {
        uint32_t v = (uint32_t)item->valuedouble;
        if (v < 10 || v > 60) {
            cJSON_Delete(request);
            return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "pose_max_roll out of range (10..60)");
        }
        config.pose_max_roll = v;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "show_box")) && cJSON_IsBool(item)) {
        config.show_box = cJSON_IsTrue(item) ? AICAM_TRUE : AICAM_FALSE;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "show_name")) && cJSON_IsBool(item)) {
        config.show_name = cJSON_IsTrue(item) ? AICAM_TRUE : AICAM_FALSE;
    }
    if ((item = cJSON_GetObjectItemCaseSensitive(request, "show_similarity")) && cJSON_IsBool(item)) {
        config.show_similarity = cJSON_IsTrue(item) ? AICAM_TRUE : AICAM_FALSE;
    }
    cJSON_Delete(request);

    if (json_config_set_face_recognition_config(&config) != AICAM_OK) {
        LOG_SVC_WARN("Face recognition config NVS persist failed (applying anyway)");
    }
    if (face_recognition_set_config(&config) != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to apply config");
    }

    cJSON *data = fr_config_to_json(&config);
    api_response_success(ctx, cJSON_Print(data), "Face recognition config updated");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief GET /api/v1/face-recognition/status
 * @details Demo stats + enrollment progress + Model B availability summary.
 */
static aicam_result_t fr_status_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    cJSON *data = cJSON_CreateObject();

    fr_stats_t stats;
    if (face_recognition_get_stats(&stats) == AICAM_OK) {
        cJSON_AddBoolToObject(data, "enabled", stats.enabled);
        cJSON_AddNumberToObject(data, "confirmed_tracks", stats.confirmed_tracks);
        cJSON_AddNumberToObject(data, "total_tracks", stats.total_tracks);
        cJSON_AddNumberToObject(data, "embed_count", stats.embed_count);
        cJSON_AddNumberToObject(data, "recognized_count", stats.recognized_count);
        cJSON_AddNumberToObject(data, "unknown_count", stats.unknown_count);
        cJSON_AddNumberToObject(data, "db_count", stats.db_count);
        cJSON_AddNumberToObject(data, "last_embed_ms", stats.last_embed_ms);
    }

    fr_enroll_status_t enroll;
    if (face_recognition_enroll_get_status(&enroll) == AICAM_OK) {
        cJSON *en = cJSON_CreateObject();
        cJSON_AddStringToObject(en, "state", fr_enroll_state_str(enroll.state));
        cJSON_AddNumberToObject(en, "samples_collected", enroll.samples_collected);
        cJSON_AddNumberToObject(en, "samples_needed", enroll.samples_needed);
        cJSON_AddNumberToObject(en, "new_id", enroll.new_id);
        cJSON_AddItemToObject(data, "enroll", en);
    }

    dual_model_status_t dm;
    if (video_dual_model_get_status(&dm) == AICAM_OK) {
        cJSON *b = cJSON_CreateObject();
        cJSON_AddBoolToObject(b, "loaded", dm.model_b_loaded);
        cJSON_AddStringToObject(b, "name", dm.model_b_name);
        cJSON_AddNumberToObject(b, "input_width", dm.model_b_input_w);
        cJSON_AddNumberToObject(b, "input_height", dm.model_b_input_h);
        cJSON_AddStringToObject(b, "postprocess", dm.model_b_postprocess);
        cJSON_AddNumberToObject(b, "avg_infer_ms", dm.avg_infer_ms);
        cJSON_AddBoolToObject(b, "disabled_by_errors", dm.disabled_by_errors);
        cJSON_AddItemToObject(data, "model_b", b);
    }

    /* Consumer diagnostics ring (field-service aid; harmless extra field) */
    {
        cJSON *ring = cJSON_CreateArray();
        face_recognition_debug_snapshot(ring);
        cJSON_AddItemToObject(data, "debug_ring", ring);
    }

    api_response_success(ctx, cJSON_Print(data), "Face recognition status retrieved");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief GET /api/v1/face-recognition/results  (live tracks with identity)
 */
static aicam_result_t fr_results_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    fr_results_t results;
    aicam_result_t ret = face_recognition_get_results(&results);
    if (ret != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to get results");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "count", results.count);
    cJSON *tracks = cJSON_CreateArray();
    for (uint32_t i = 0; i < results.count; i++) {
        const fr_track_info_t *t = &results.tracks[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "track_id", t->track_id);
        cJSON_AddNumberToObject(item, "x", t->x);
        cJSON_AddNumberToObject(item, "y", t->y);
        cJSON_AddNumberToObject(item, "w", t->w);
        cJSON_AddNumberToObject(item, "h", t->h);
        cJSON_AddNumberToObject(item, "detection_confidence", t->det_conf);
        cJSON_AddStringToObject(item, "status", fr_status_str(t->status));
        cJSON_AddStringToObject(item, "name", t->name);
        cJSON_AddNumberToObject(item, "db_id", t->db_id);
        cJSON_AddNumberToObject(item, "similarity", t->similarity);
        cJSON_AddBoolToObject(item, "frontal", t->frontal);
        cJSON_AddNumberToObject(item, "pose_score", t->pose_score);
        cJSON_AddItemToArray(tracks, item);
    }
    cJSON_AddItemToObject(data, "tracks", tracks);

    api_response_success(ctx, cJSON_Print(data), "Face recognition results retrieved");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief GET /api/v1/face-recognition/faces  (enrolled template list)
 */
static aicam_result_t fr_faces_list_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    face_db_list_t list;
    if (face_db_list(&list) != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to list faces");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "count", list.count);
    cJSON *faces = cJSON_CreateArray();
    for (uint32_t i = 0; i < list.count; i++) {
        const face_db_summary_t *e = &list.entries[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", e->id);
        cJSON_AddStringToObject(item, "name", e->name);
        cJSON_AddNumberToObject(item, "dim", e->dim);
        cJSON_AddNumberToObject(item, "created_at", e->created_s);
        cJSON_AddItemToArray(faces, item);
    }
    cJSON_AddItemToObject(data, "faces", faces);

    api_response_success(ctx, cJSON_Print(data), "Face list retrieved");
    cJSON_Delete(data);
    face_db_list_free(&list);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/faces/enroll  body: {"name": "..."}
 */
static aicam_result_t fr_faces_enroll_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    cJSON *request = web_api_parse_body(ctx);
    if (!request) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid JSON body");
    }

    const char *name = NULL;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(request, "name");
    if (item && cJSON_IsString(item)) {
        name = item->valuestring;
    }
    if (!name || name[0] == '\0') {
        cJSON_Delete(request);
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Field 'name' is required");
    }

    aicam_result_t ret = face_recognition_enroll_start(name);
    cJSON_Delete(request);
    if (ret == AICAM_ERROR_BUSY) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Enrollment already running");
    }
    if (ret == AICAM_ERROR_INVALID_STATE) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST,
                                  "Face recognition disabled or Model B not running");
    }
    if (ret != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to start enrollment");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "collecting");
    api_response_success(ctx, cJSON_Print(data), "Enrollment started");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief GET /api/v1/face-recognition/faces/enroll/status
 */
static aicam_result_t fr_faces_enroll_status_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "GET")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    fr_enroll_status_t enroll;
    if (face_recognition_enroll_get_status(&enroll) != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to get enrollment status");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", fr_enroll_state_str(enroll.state));
    cJSON_AddNumberToObject(data, "samples_collected", enroll.samples_collected);
    cJSON_AddNumberToObject(data, "samples_needed", enroll.samples_needed);
    cJSON_AddNumberToObject(data, "new_id", enroll.new_id);
    cJSON_AddNumberToObject(data, "capture_armed", enroll.capture_armed);
    api_response_success(ctx, cJSON_Print(data), "Enrollment status retrieved");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/faces/enroll/capture
 * @details Arms ONE sample capture for the running manual enrollment; the
 *          next frontal largest-face pass consumes it.
 */
static aicam_result_t fr_faces_enroll_capture_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    aicam_result_t ret = face_recognition_enroll_capture();
    if (ret == AICAM_ERROR_INVALID_STATE) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "No enrollment running");
    }
    if (ret != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to arm capture");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "armed");
    api_response_success(ctx, cJSON_Print(data), "Capture armed (frontal pose required)");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/faces/enroll/finish
 * @details Completes the enrollment early with the samples captured so far.
 */
static aicam_result_t fr_faces_enroll_finish_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    aicam_result_t ret = face_recognition_enroll_finish();
    if (ret == AICAM_ERROR_INVALID_STATE) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST,
                                  "No enrollment running or no samples captured");
    }
    if (ret != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to finish enrollment");
    }

    fr_enroll_status_t enroll;
    face_recognition_enroll_get_status(&enroll);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", fr_enroll_state_str(enroll.state));
    cJSON_AddNumberToObject(data, "new_id", enroll.new_id);
    api_response_success(ctx, cJSON_Print(data), "Enrollment finished");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/faces/enroll/cancel
 */
static aicam_result_t fr_faces_enroll_cancel_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    face_recognition_enroll_cancel();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "idle");
    api_response_success(ctx, cJSON_Print(data), "Enrollment cancelled");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/faces/delete  body: {"id": N}
 */
static aicam_result_t fr_faces_delete_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    cJSON *request = web_api_parse_body(ctx);
    if (!request) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid JSON body");
    }

    cJSON *item = cJSON_GetObjectItemCaseSensitive(request, "id");
    if (!item || !cJSON_IsNumber(item)) {
        cJSON_Delete(request);
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Field 'id' is required");
    }
    uint32_t id = (uint32_t)item->valuedouble;
    cJSON_Delete(request);

    aicam_result_t ret = face_db_delete(id);
    if (ret == AICAM_ERROR_NOT_FOUND) {
        return api_response_error(ctx, API_ERROR_NOT_FOUND, "Face id not found");
    }
    if (ret != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to delete face");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", id);
    api_response_success(ctx, cJSON_Print(data), "Face deleted");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/faces/rename  body: {"id": N, "name": "..."}
 */
static aicam_result_t fr_faces_rename_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    cJSON *request = web_api_parse_body(ctx);
    if (!request) {
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Invalid JSON body");
    }

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(request, "id");
    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(request, "name");
    if (!id_item || !cJSON_IsNumber(id_item) ||
        !name_item || !cJSON_IsString(name_item) || name_item->valuestring[0] == '\0') {
        cJSON_Delete(request);
        return api_response_error(ctx, API_ERROR_INVALID_REQUEST, "Fields 'id' and 'name' are required");
    }
    uint32_t id = (uint32_t)id_item->valuedouble;
    const char *name = name_item->valuestring;
    char name_buf[FACE_DB_NAME_MAX];
    strncpy(name_buf, name, sizeof(name_buf) - 1u);
    name_buf[sizeof(name_buf) - 1u] = '\0';
    cJSON_Delete(request);

    aicam_result_t ret = face_db_rename(id, name_buf);
    if (ret == AICAM_ERROR_NOT_FOUND) {
        return api_response_error(ctx, API_ERROR_NOT_FOUND, "Face id not found");
    }
    if (ret != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to rename face");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "id", id);
    cJSON_AddStringToObject(data, "name", name_buf);
    api_response_success(ctx, cJSON_Print(data), "Face renamed");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/faces/clear  (delete the whole database)
 */
static aicam_result_t fr_faces_clear_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    if (face_db_clear() != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to clear faces");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "count", 0);
    api_response_success(ctx, cJSON_Print(data), "Face database cleared");
    cJSON_Delete(data);
    return AICAM_OK;
}

/**
 * @brief POST /api/v1/face-recognition/reset  (clear trackers + stats)
 */
static aicam_result_t fr_reset_handler(http_handler_context_t *ctx)
{
    if (!ctx) return AICAM_ERROR_INVALID_PARAM;

    if (!web_api_verify_method(ctx, "POST")) {
        return api_response_error(ctx, API_ERROR_METHOD_NOT_ALLOWED, "Method Not Allowed");
    }

    if (face_recognition_reset() != AICAM_OK) {
        return api_response_error(ctx, API_ERROR_INTERNAL_ERROR, "Failed to reset");
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "status", "reset");
    api_response_success(ctx, cJSON_Print(data), "Face recognition reset");
    cJSON_Delete(data);
    return AICAM_OK;
}

/* ==================== Module Definition ==================== */

static const api_route_t face_recognition_module_routes[] = {
    { .path = API_PATH_PREFIX "/face-recognition/config", .method = "GET",
      .handler = fr_get_config_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/config", .method = "POST",
      .handler = fr_set_config_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/status", .method = "GET",
      .handler = fr_status_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/results", .method = "GET",
      .handler = fr_results_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces", .method = "GET",
      .handler = fr_faces_list_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/enroll", .method = "POST",
      .handler = fr_faces_enroll_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/enroll/status", .method = "GET",
      .handler = fr_faces_enroll_status_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/enroll/capture", .method = "POST",
      .handler = fr_faces_enroll_capture_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/enroll/finish", .method = "POST",
      .handler = fr_faces_enroll_finish_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/enroll/cancel", .method = "POST",
      .handler = fr_faces_enroll_cancel_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/delete", .method = "POST",
      .handler = fr_faces_delete_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/rename", .method = "POST",
      .handler = fr_faces_rename_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/faces/clear", .method = "POST",
      .handler = fr_faces_clear_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
    { .path = API_PATH_PREFIX "/face-recognition/reset", .method = "POST",
      .handler = fr_reset_handler, .require_auth = AICAM_TRUE, .user_data = NULL },
};

aicam_result_t web_api_register_face_recognition_module(void) {
    LOG_CORE_INFO("Registering face recognition module");

    for (size_t i = 0; i < sizeof(face_recognition_module_routes) / sizeof(face_recognition_module_routes[0]); i++) {
        aicam_result_t result = http_server_register_route(&face_recognition_module_routes[i]);
        if (result != AICAM_OK) {
            LOG_CORE_ERROR("Failed to register face recognition module: %d", result);
            return result;
        }
    }

    LOG_CORE_INFO("Face recognition module registered successfully");
    return AICAM_OK;
}
