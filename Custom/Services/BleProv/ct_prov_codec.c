#include <string.h>
#include "ct_prov_codec.h"

static bool ct_prov_kind_is_known(uint8_t kind)
{
    return (kind == CTPROV_PACKET_REQUEST) || (kind == CTPROV_PACKET_RESPONSE) ||
           (kind == CTPROV_PACKET_EVENT) || (kind == CTPROV_PACKET_PROTOCOL_ERROR);
}

void ct_prov_assembler_reset(ct_prov_assembler_t *assembler)
{
    assembler->active = false;
}

static ct_prov_feed_result_t ct_prov_assembler_reject(ct_prov_assembler_t *assembler, const char **reason, const char *why)
{
    ct_prov_assembler_reset(assembler);
    if (reason != NULL) {
        *reason = why;
    }
    return CTPROV_FEED_REJECTED;
}

ct_prov_feed_result_t ct_prov_assembler_feed(ct_prov_assembler_t *assembler, const uint8_t *frame, uint16_t len,
                                             uint32_t now_ms, ct_prov_packet_t *packet, const char **reason)
{
    uint16_t payload_len;

    if (len < CTPROV_FRAG_HEADER_SIZE) {
        return ct_prov_assembler_reject(assembler, reason, "malformed_frame");
    }
    if (frame[0] != CTPROV_FRAG_MAGIC) {
        return ct_prov_assembler_reject(assembler, reason, "bad_magic");
    }
    if (!ct_prov_kind_is_known(frame[1])) {
        return ct_prov_assembler_reject(assembler, reason, "unknown_packet_kind");
    }

    uint8_t kind       = frame[1];
    uint8_t message_id = frame[2];
    uint8_t index      = frame[3];
    uint8_t count      = frame[4];

    if (!assembler->active) {
        if (count < 1) {
            return ct_prov_assembler_reject(assembler, reason, "malformed_frame");
        }
        if (index != 0) {
            return ct_prov_assembler_reject(assembler, reason, "out_of_order");
        }
        assembler->active      = true;
        assembler->kind        = kind;
        assembler->message_id  = message_id;
        assembler->count       = count;
        assembler->next_index  = 0;
        assembler->len         = 0;
        assembler->started_ms  = now_ms;
    } else if (kind != assembler->kind || message_id != assembler->message_id || count != assembler->count) {
        return ct_prov_assembler_reject(assembler, reason, "header_changed");
    } else if (index < assembler->next_index) {
        return ct_prov_assembler_reject(assembler, reason, "duplicate_fragment");
    } else if (index > assembler->next_index) {
        return ct_prov_assembler_reject(assembler, reason, "out_of_order");
    }

    // Non-final fragments are not checked for size: the local MTU view may
    // differ from the negotiated link value.
    payload_len = (uint16_t)(len - CTPROV_FRAG_HEADER_SIZE);
    if ((uint32_t)assembler->len + payload_len > CTPROV_PACKET_MAX) {
        return ct_prov_assembler_reject(assembler, reason, "oversized");
    }
    memcpy(&assembler->bytes[assembler->len], &frame[CTPROV_FRAG_HEADER_SIZE], payload_len);
    assembler->len = (uint16_t)(assembler->len + payload_len);

    if ((uint8_t)(index + 1) < assembler->count) {
        assembler->next_index = (uint8_t)(index + 1);
        return CTPROV_FEED_ASSEMBLING;
    }

    if (packet != NULL) {
        packet->kind       = assembler->kind;
        packet->message_id = assembler->message_id;
        packet->len        = assembler->len;
        packet->bytes      = assembler->bytes;
    }
    ct_prov_assembler_reset(assembler);
    return CTPROV_FEED_COMPLETE;
}

bool ct_prov_assembler_timed_out(const ct_prov_assembler_t *assembler, uint32_t now_ms)
{
    return assembler->active && (uint32_t)(now_ms - assembler->started_ms) >= CTPROV_REASSEMBLY_TIMEOUT_MS;
}

bool ct_prov_slicer_init(ct_prov_slicer_t *slicer, uint8_t kind, uint8_t message_id,
                         const uint8_t *payload, uint16_t len, uint16_t mtu)
{
    uint32_t payload_max;
    uint32_t count;

    memset(slicer, 0, sizeof(*slicer));
    if (payload == NULL || len == 0 || len > CTPROV_PACKET_MAX || mtu <= (CTPROV_FRAG_HEADER_SIZE + 3)) {
        return false;
    }
    payload_max = (uint32_t)mtu - 3 - CTPROV_FRAG_HEADER_SIZE;
    count        = ((uint32_t)len + payload_max - 1) / payload_max;
    if (count > 255) {
        return false;
    }
    slicer->payload      = payload;
    slicer->len          = len;
    slicer->offset       = 0;
    slicer->kind         = kind;
    slicer->message_id   = message_id;
    slicer->count        = (uint8_t)count;
    slicer->payload_max  = (uint16_t)payload_max;
    return true;
}

uint16_t ct_prov_slicer_next(ct_prov_slicer_t *slicer, uint8_t *frame, uint16_t frame_cap)
{
    uint16_t payload_len;
    uint16_t frame_len;

    if (slicer->offset >= slicer->len) {
        return 0;
    }
    payload_len = (uint16_t)(slicer->len - slicer->offset);
    if (payload_len > slicer->payload_max) {
        payload_len = slicer->payload_max;
    }
    frame_len = (uint16_t)(CTPROV_FRAG_HEADER_SIZE + payload_len);
    if (frame == NULL || frame_cap < frame_len) {
        return 0;
    }

    frame[0] = CTPROV_FRAG_MAGIC;
    frame[1] = slicer->kind;
    frame[2] = slicer->message_id;
    frame[3] = (uint8_t)(slicer->offset / slicer->payload_max);
    frame[4] = slicer->count;
    memcpy(&frame[CTPROV_FRAG_HEADER_SIZE], &slicer->payload[slicer->offset], payload_len);
    slicer->offset = (uint16_t)(slicer->offset + payload_len);
    return frame_len;
}
