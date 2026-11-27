#ifndef __CT_PROV_CODEC_H__
#define __CT_PROV_CODEC_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/// @brief CT-Prov/1 fragmentation layer (protocol spec section 5): the wire
///        format of one C2D write / D2C indication attribute value.
//!
//!        Each value carries a 5-byte header followed by a slice of the UTF-8
//!        JSON logical packet:
//!            [0] magic_version 0xC1
//!            [1] packet_kind
//!            [2] message_id    (per direction, modulo 256)
//!            [3] fragment_index (starts at 0)
//!            [4] fragment_count (1..255)
//!        One logical packet is reassembled per direction at a time.

#define CTPROV_FRAG_MAGIC            0xC1
#define CTPROV_FRAG_HEADER_SIZE      5
#define CTPROV_PACKET_MAX            4096    ///< logical packet payload cap in bytes
#define CTPROV_REASSEMBLY_TIMEOUT_MS 3000    ///< drop an incomplete packet after this

typedef enum {
    CTPROV_PACKET_REQUEST        = 0x01,
    CTPROV_PACKET_RESPONSE       = 0x02,
    CTPROV_PACKET_EVENT          = 0x03,
    CTPROV_PACKET_PROTOCOL_ERROR = 0x7F,
} ct_prov_packet_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t message_id;
    uint16_t len;
    const uint8_t *bytes;      ///< points into the assembler buffer; valid until the next feed
} ct_prov_packet_t;

/// @brief Single-direction reassembler. Roughly 4 KB: allocate it outside the stack.
typedef struct {
    bool active;
    uint8_t kind;
    uint8_t message_id;
    uint8_t count;
    uint8_t next_index;
    uint16_t len;
    uint32_t started_ms;
    uint8_t bytes[CTPROV_PACKET_MAX];
} ct_prov_assembler_t;

typedef enum {
    CTPROV_FEED_ASSEMBLING = 0,   ///< fragment accepted, more expected
    CTPROV_FEED_COMPLETE,         ///< logical packet assembled; *packet describes it
    CTPROV_FEED_REJECTED,         ///< protocol violation: the assembly was dropped
} ct_prov_feed_result_t;

void ct_prov_assembler_reset(ct_prov_assembler_t *assembler);

/// @brief Feed one attribute value.
/// @param now_ms  monotonic milliseconds, used for the reassembly timeout
/// @param packet  filled in when the result is CTPROV_FEED_COMPLETE
/// @param reason  set to a stable reason token on CTPROV_FEED_REJECTED
///               (malformed_frame, bad_magic, unknown_packet_kind,
///                out_of_order, header_changed, duplicate_fragment, oversized)
ct_prov_feed_result_t ct_prov_assembler_feed(ct_prov_assembler_t *assembler, const uint8_t *frame, uint16_t len,
                                             uint32_t now_ms, ct_prov_packet_t *packet, const char **reason);

/// @brief True when an incomplete assembly is older than CTPROV_REASSEMBLY_TIMEOUT_MS
bool ct_prov_assembler_timed_out(const ct_prov_assembler_t *assembler, uint32_t now_ms);

typedef struct {
    const uint8_t *payload;
    uint16_t len;
    uint16_t offset;
    uint8_t kind;
    uint8_t message_id;
    uint8_t count;
    uint16_t payload_max;
} ct_prov_slicer_t;

/// @brief Slice one logical packet for a link with the given ATT MTU. Every
///        fragment but the last is filled to payload_max (MTU - 3 - 5).
/// @return false when the packet is empty, too large, or needs more than 255 fragments
bool ct_prov_slicer_init(ct_prov_slicer_t *slicer, uint8_t kind, uint8_t message_id,
                         const uint8_t *payload, uint16_t len, uint16_t mtu);

/// @brief Render the next fragment (header included). Returns its length, or 0 when done.
uint16_t ct_prov_slicer_next(ct_prov_slicer_t *slicer, uint8_t *frame, uint16_t frame_cap);

#endif
