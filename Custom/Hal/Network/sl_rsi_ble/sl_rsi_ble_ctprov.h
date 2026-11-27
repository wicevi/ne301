#ifndef __SL_RSI_BLE_CTPROV_H__
#define __SL_RSI_BLE_CTPROV_H__

#ifdef SLI_SI91X_ENABLE_BLE

#include <stdint.h>
#include <stdbool.h>
#include "sl_rsi_ble_gatt.h"

/// @brief CT-Prov/1 BLE binding (camthink-ai/NeoLinker, docs/ct-prov-protocol.md):
///        GATT profile, Advertising Data, Scan Response and META payload builders.
///        The session logic (fragmentation, JSON envelopes, operations) is not
///        part of this layer.

#define SL_CTPROV_FORMAT_VERSION       0x01    ///< advertising / META format version, CT-Prov/1
#define SL_CTPROV_MODEL_CODE_NE301     0x02    ///< model enumeration in Advertising Data

#define SL_CTPROV_NAME_MIN_LEN         9       ///< "<MODEL>-<MAC_SUFFIX>", MODEL is 2..10 characters
#define SL_CTPROV_NAME_MAX_LEN         17
#define SL_CTPROV_META_MAX_LEN         20      ///< must fit one Read Response at ATT MTU 23

/// Discovery status flags (Advertising Data and META)
#define SL_CTPROV_FLAG_BUSY            0x01    ///< serving another configuration connection
#define SL_CTPROV_FLAG_PROVISIONABLE   0x02    ///< configuration may start now (0 when BUSY)

enum {
    SL_CTPROV_CHAR_C2D = 0,     ///< Client -> Device, Write (encrypted)
    SL_CTPROV_CHAR_D2C,         ///< Device -> Client, Indicate (encrypted)
    SL_CTPROV_CHAR_META,        ///< version / flags / name, Read (also before pairing)
    SL_CTPROV_CHAR_COUNT
};

/// @brief Fill the CT-Prov profile. chars[] must outlive the profile.
void sl_ble_ctprov_profile_init(sl_ble_gatt_profile_t *profile, sl_ble_gatt_char_t chars[SL_CTPROV_CHAR_COUNT]);

/// @brief true when device_name follows "<MODEL>-<MAC_SUFFIX>" (9..17 bytes of A-Z 0-9 '-')
bool sl_ble_ctprov_name_is_valid(const char *device_name);

/// @brief Advertising Data: Flags + Service Data - 128-bit UUID (27 bytes).
/// @param mac_suffix  last 3 bytes of the canonical primary MAC, display order
uint8_t sl_ble_ctprov_adv_data(uint8_t *adv, uint8_t model_code, const uint8_t mac_suffix[3], uint8_t flags);

/// @brief Scan Response: Complete Local Name. Returns 0 for an invalid name.
uint8_t sl_ble_ctprov_scan_response(uint8_t *scan_rsp, const char *device_name);

/// @brief META characteristic value: version, flags, name length, name. Returns 0 for an invalid name.
uint8_t sl_ble_ctprov_meta(uint8_t *meta, const char *device_name, uint8_t flags);

#endif

#endif /* __SL_RSI_BLE_CTPROV_H__ */
