#ifndef __CT_PROV_DISPATCH_H__
#define __CT_PROV_DISPATCH_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

/// @brief CT-Prov/1 request dispatcher (protocol spec sections 6-12).
///        Transport agnostic: the BLE GATT binding feeds it the reassembled
///        request packet and gets one response packet to slice back onto the
///        link; the softAP HTTP binding can hand over a request body the same
///        way. Operations are implemented by the ct_prov_backend_t callbacks.

#define CTPROV_MESSAGE_MAX       96      ///< cap of a diagnostic error message

typedef enum {
    CTPROV_OK = 0,
    CTPROV_ERR_INVALID_MESSAGE,
    CTPROV_ERR_UNSUPPORTED_VERSION,
    CTPROV_ERR_UNSUPPORTED_OPERATION,
    CTPROV_ERR_INVALID_ARGUMENT,
    CTPROV_ERR_BUSY,
    CTPROV_ERR_SECTION_NOT_SUPPORTED,
    CTPROV_ERR_FIELD_NOT_SUPPORTED,
    CTPROV_ERR_STORAGE_FAILED,
    CTPROV_ERR_INTERNAL_ERROR,
} ct_prov_err_t;

const char *ct_prov_err_name(ct_prov_err_t err);

/// @brief Outcome of one operation: CTPROV_OK with data filled in, or an
///        error code with an optional diagnostic message.
typedef struct {
    ct_prov_err_t code;
    char message[CTPROV_MESSAGE_MAX];
} ct_prov_result_t;

/// @brief Set an operation outcome (message may be NULL)
void ct_prov_result_set(ct_prov_result_t *result, ct_prov_err_t code, const char *message);

typedef struct {
    uint32_t event_seq;     ///< seq of the next protocol.error event
} ct_prov_session_t;

typedef struct {
    void *user;
    /// @name Operations. Each fills its response data object or result.
    /// @{
    void (*device_info)(void *user, cJSON *data, ct_prov_result_t *result);
    void (*capabilities)(void *user, cJSON *data, ct_prov_result_t *result);
    void (*scan)(void *user, const char *section, const char *cursor, cJSON *data, ct_prov_result_t *result);
    void (*config_set)(void *user, const cJSON *sections, cJSON *data, ct_prov_result_t *result);
    void (*config_get)(void *user, cJSON *data, ct_prov_result_t *result);
    /// @brief The peer ended the session. Called after the response has been
    ///        rendered; the transport decides what "close" means for its link.
    void (*session_close)(void *user);
    /// @}
} ct_prov_backend_t;

void ct_prov_session_init(ct_prov_session_t *session);

/// @brief Render the protocol.error async event (a fault that cannot be
///        attributed to a request, e.g. a rejected fragment sequence).
/// @return packet length in bytes, 0 when it does not fit in out_cap
uint16_t ct_prov_dispatch_protocol_error(ct_prov_session_t *session, ct_prov_err_t code, const char *message,
                                         uint8_t *out, uint16_t out_cap);

/// @brief Handle one complete request packet and render the packet to send
///        back: the response envelope, or a protocol.error event when the
///        request could not be attributed to an id.
/// @return packet length in bytes, 0 when it does not fit in out_cap
uint16_t ct_prov_dispatch_request(ct_prov_session_t *session, const ct_prov_backend_t *backend,
                                  const uint8_t *request, uint16_t request_len,
                                  uint8_t *out, uint16_t out_cap);

#endif
