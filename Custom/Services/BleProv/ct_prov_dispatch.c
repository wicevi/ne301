#include <string.h>
#include "ct_prov_dispatch.h"

// Request op -> response op. The NeoLinker session client checks most of
// them strictly (device.info.get.result, communication.scan.result, ...);
// capabilities is accepted with or without the "get".
typedef struct {
    const char *request;
    const char *result;
} ct_prov_op_t;

static const ct_prov_op_t s_ops[] = {
    { "device.info.get",               "device.info.get.result" },
    { "communication.capabilities.get", "communication.capabilities.result" },
    { "communication.scan",            "communication.scan.result" },
    { "config.set",                    "config.set.result" },
    { "config.get",                    "config.get.result" },
    { "session.close",                 "session.close.result" },
};

const char *ct_prov_err_name(ct_prov_err_t err)
{
    switch (err) {
        case CTPROV_OK:                       return "OK";
        case CTPROV_ERR_INVALID_MESSAGE:      return "INVALID_MESSAGE";
        case CTPROV_ERR_UNSUPPORTED_VERSION:  return "UNSUPPORTED_VERSION";
        case CTPROV_ERR_UNSUPPORTED_OPERATION: return "UNSUPPORTED_OPERATION";
        case CTPROV_ERR_INVALID_ARGUMENT:     return "INVALID_ARGUMENT";
        case CTPROV_ERR_BUSY:                 return "BUSY";
        case CTPROV_ERR_SECTION_NOT_SUPPORTED: return "SECTION_NOT_SUPPORTED";
        case CTPROV_ERR_FIELD_NOT_SUPPORTED:  return "FIELD_NOT_SUPPORTED";
        case CTPROV_ERR_STORAGE_FAILED:       return "STORAGE_FAILED";
        case CTPROV_ERR_INTERNAL_ERROR:       return "INTERNAL_ERROR";
        default:                              return "INTERNAL_ERROR";
    }
}

void ct_prov_session_init(ct_prov_session_t *session)
{
    session->event_seq = 0;
}

void ct_prov_result_set(ct_prov_result_t *result, ct_prov_err_t code, const char *message)
{
    result->code = code;
    if (message != NULL) {
        strncpy(result->message, message, sizeof(result->message) - 1);
        result->message[sizeof(result->message) - 1] = '\0';
    } else {
        result->message[0] = '\0';
    }
}


static uint16_t ct_prov_render(cJSON *envelope, uint8_t *out, uint16_t out_cap)
{
    char *text = cJSON_PrintUnformatted(envelope);
    size_t len;

    cJSON_Delete(envelope);
    if (text == NULL) {
        return 0;
    }
    len = strlen(text);
    if (len > out_cap) {
        cJSON_free(text);
        return 0;
    }
    memcpy(out, text, len);
    cJSON_free(text);
    return (uint16_t)len;
}

/// Render a protocol.error event (no request id to attach it to)
static uint16_t ct_prov_render_event(ct_prov_session_t *session, ct_prov_err_t code, const char *message,
                                     uint8_t *out, uint16_t out_cap)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data;

    if (root == NULL) {
        return 0;
    }
    cJSON_AddNumberToObject(root, "seq", (double)session->event_seq);
    session->event_seq++;
    cJSON_AddStringToObject(root, "event", "protocol.error");
    data = cJSON_AddObjectToObject(root, "data");
    cJSON_AddStringToObject(data, "code", ct_prov_err_name(code));
    cJSON_AddStringToObject(data, "message", message);
    return ct_prov_render(root, out, out_cap);
}

static cJSON *ct_prov_error_response(uint32_t id, const char *result_op, ct_prov_err_t code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddStringToObject(root, "op", result_op);
    cJSON_AddBoolToObject(root, "ok", false);
    error = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(error, "code", ct_prov_err_name(code));
    cJSON_AddStringToObject(error, "message", (message != NULL && message[0] != '\0') ? message
                                                                                       : "operation failed");
    return root;
}

static cJSON *ct_prov_ok_response(uint32_t id, const char *result_op, cJSON *data)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddStringToObject(root, "op", result_op);
    cJSON_AddBoolToObject(root, "ok", true);
    if (data == NULL) {
        data = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "data", data);
    return root;
}

/// Integers must not be expressed as floats (section 6).
static bool ct_prov_is_uint32(const cJSON *item, uint32_t *value)
{
    double number;

    if (!cJSON_IsNumber(item)) {
        return false;
    }
    number = cJSON_GetNumberValue(item);
    if (number < 0.0 || number > 4294967295.0 || number != (double)(uint32_t)number) {
        return false;
    }
    *value = (uint32_t)number;
    return true;
}

static bool ct_prov_is_whitespace(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

static const char *ct_prov_result_op(const char *request_op)
{
    static char result_op[64];
    size_t len = strlen(request_op);

    for (size_t i = 0; i < sizeof(s_ops) / sizeof(s_ops[0]); i++) {
        if (strcmp(s_ops[i].request, request_op) == 0) {
            return s_ops[i].result;
        }
    }
    // Unknown op: answer with its ".result" form so the client can correlate it
    if (len > sizeof(result_op) - 8) {
        len = sizeof(result_op) - 8;
    }
    memcpy(result_op, request_op, len);
    result_op[len] = '\0';
    if (len < 7 || strcmp(&result_op[len - 7], ".result") != 0) {
        strcat(result_op, ".result");
    }
    return result_op;
}

uint16_t ct_prov_dispatch_protocol_error(ct_prov_session_t *session, ct_prov_err_t code, const char *message,
                                         uint8_t *out, uint16_t out_cap)
{
    return ct_prov_render_event(session, code, message, out, out_cap);
}

uint16_t ct_prov_dispatch_request(ct_prov_session_t *session, const ct_prov_backend_t *backend,
                                  const uint8_t *request, uint16_t request_len,
                                  uint8_t *out, uint16_t out_cap)
{
    const char *parse_end = NULL;
    const char *cursor;
    uint32_t id = 0;
    ct_prov_result_t result;
    cJSON *root;
    cJSON *item;
    cJSON *data;
    cJSON *request_data;
    const char *op;
    const char *result_op;
    const char *scan_section;
    const char *scan_cursor;
    uint16_t len;
    bool close_session = false;

    if (request_len >= 3 && request[0] == 0xEF && request[1] == 0xBB && request[2] == 0xBF) {
        return ct_prov_render_event(session, CTPROV_ERR_INVALID_MESSAGE, "byte order mark is forbidden", out, out_cap);
    }
    root = cJSON_ParseWithLengthOpts((const char *)request, request_len, &parse_end, 0);
    if (root == NULL) {
        return ct_prov_render_event(session, CTPROV_ERR_INVALID_MESSAGE, "invalid JSON", out, out_cap);
    }
    for (cursor = parse_end; (size_t)(cursor - (const char *)request) < request_len && ct_prov_is_whitespace(*cursor); cursor++) {
        // skip whitespace after the top-level value
    }
    if ((size_t)(cursor - (const char *)request) < request_len) {
        cJSON_Delete(root);
        return ct_prov_render_event(session, CTPROV_ERR_INVALID_MESSAGE, "trailing content after the envelope", out, out_cap);
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ct_prov_render_event(session, CTPROV_ERR_INVALID_MESSAGE, "top level must be an object", out, out_cap);
    }

    item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!ct_prov_is_uint32(item, &id)) {
        cJSON_Delete(root);
        return ct_prov_render_event(session, CTPROV_ERR_INVALID_MESSAGE, "id must be an unsigned integer", out, out_cap);
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return ct_prov_render_event(session, CTPROV_ERR_INVALID_MESSAGE, "op must be a non-empty string", out, out_cap);
    }
    op         = item->valuestring;
    result_op  = ct_prov_result_op(op);
    request_data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (request_data != NULL && !cJSON_IsObject(request_data)) {
        cJSON_Delete(root);
        return ct_prov_render(ct_prov_error_response(id, result_op, CTPROV_ERR_INVALID_MESSAGE, "data must be an object"), out, out_cap);
    }
    if (request_data == NULL) {
        request_data = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "data", request_data);
    }

    memset(&result, 0, sizeof(result));
    result.code = CTPROV_OK;
    data        = NULL;

    if (strcmp(op, "device.info.get") == 0 && backend->device_info != NULL) {
        data = cJSON_CreateObject();
        backend->device_info(backend->user, data, &result);
    } else if (strcmp(op, "communication.capabilities.get") == 0 && backend->capabilities != NULL) {
        data = cJSON_CreateObject();
        backend->capabilities(backend->user, data, &result);
    } else if (strcmp(op, "communication.scan") == 0 && backend->scan != NULL) {
        item = cJSON_GetObjectItemCaseSensitive(request_data, "section");
        if (!cJSON_IsString(item) || item->valuestring == NULL || item->valuestring[0] == '\0') {
            ct_prov_result_set(&result, CTPROV_ERR_INVALID_ARGUMENT, "section must be a non-empty string");
        } else {
            scan_section = item->valuestring;
            item = cJSON_GetObjectItemCaseSensitive(request_data, "cursor");
            scan_cursor = NULL;
            if (item != NULL && !cJSON_IsNull(item)) {
                if (!cJSON_IsString(item) || item->valuestring == NULL) {
                    ct_prov_result_set(&result, CTPROV_ERR_INVALID_ARGUMENT, "cursor must be a string");
                    goto respond;
                }
                scan_cursor = item->valuestring;
            }
            data = cJSON_CreateObject();
            backend->scan(backend->user, scan_section, scan_cursor, data, &result);
        }
    } else if (strcmp(op, "config.set") == 0 && backend->config_set != NULL) {
        data = cJSON_CreateObject();
        backend->config_set(backend->user, request_data, data, &result);
    } else if (strcmp(op, "config.get") == 0 && backend->config_get != NULL) {
        data = cJSON_CreateObject();
        backend->config_get(backend->user, data, &result);
    } else if (strcmp(op, "session.close") == 0) {
        // session.close always succeeds
        close_session = true;
    } else {
        ct_prov_result_set(&result, CTPROV_ERR_UNSUPPORTED_OPERATION, "operation is not supported");
    }

respond:
    if (result.code != CTPROV_OK) {
        cJSON_Delete(data);
        len = ct_prov_render(ct_prov_error_response(id, result_op, result.code, result.message), out, out_cap);
    } else {
        len = ct_prov_render(ct_prov_ok_response(id, result_op, data), out, out_cap);
    }
    cJSON_Delete(root);

    if (len == 0) {
        len = ct_prov_render(ct_prov_error_response(id, result_op, CTPROV_ERR_INTERNAL_ERROR, "response does not fit"), out, out_cap);
    }
    if (close_session && backend->session_close != NULL) {
        backend->session_close(backend->user);
    }
    return len;
}
