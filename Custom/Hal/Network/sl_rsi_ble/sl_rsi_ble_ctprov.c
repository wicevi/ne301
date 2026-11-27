#ifdef SLI_SI91X_ENABLE_BLE
#include <string.h>
#include "sl_rsi_ble_ctprov.h"

// 7B1E000{1..4}-9D5B-4B5B-8F6D-2A4C0B7E0100, BLE little-endian byte order
static const uint8_t s_ctprov_service_uuid[SL_BLE_GATT_UUID128_LEN] = {
    0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F, 0x5B, 0x4B, 0x5B, 0x9D, 0x01, 0x00, 0x1E, 0x7B
};
static const uint8_t s_ctprov_c2d_uuid[SL_BLE_GATT_UUID128_LEN] = {
    0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F, 0x5B, 0x4B, 0x5B, 0x9D, 0x02, 0x00, 0x1E, 0x7B
};
static const uint8_t s_ctprov_d2c_uuid[SL_BLE_GATT_UUID128_LEN] = {
    0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F, 0x5B, 0x4B, 0x5B, 0x9D, 0x03, 0x00, 0x1E, 0x7B
};
static const uint8_t s_ctprov_meta_uuid[SL_BLE_GATT_UUID128_LEN] = {
    0x00, 0x01, 0x7E, 0x0B, 0x4C, 0x2A, 0x6D, 0x8F, 0x5B, 0x4B, 0x5B, 0x9D, 0x04, 0x00, 0x1E, 0x7B
};

#define SL_CTPROV_AD_FLAGS         0x01
#define SL_CTPROV_AD_SERVICE_DATA  0x21
#define SL_CTPROV_AD_COMPLETE_NAME 0x09

#define SL_CTPROV_FLAGS_MASK       (SL_CTPROV_FLAG_BUSY | SL_CTPROV_FLAG_PROVISIONABLE)

void sl_ble_ctprov_profile_init(sl_ble_gatt_profile_t *profile, sl_ble_gatt_char_t chars[SL_CTPROV_CHAR_COUNT])
{
    memset(chars, 0, sizeof(*chars) * SL_CTPROV_CHAR_COUNT);

    chars[SL_CTPROV_CHAR_C2D].uuid128   = s_ctprov_c2d_uuid;
    chars[SL_CTPROV_CHAR_C2D].property  = SL_BLE_GATT_PROP_WRITE;
    chars[SL_CTPROV_CHAR_C2D].encrypted = true;
    chars[SL_CTPROV_CHAR_D2C].uuid128   = s_ctprov_d2c_uuid;
    chars[SL_CTPROV_CHAR_D2C].property  = SL_BLE_GATT_PROP_INDICATE;
    chars[SL_CTPROV_CHAR_D2C].encrypted = true;
    chars[SL_CTPROV_CHAR_META].uuid128  = s_ctprov_meta_uuid;
    chars[SL_CTPROV_CHAR_META].property = SL_BLE_GATT_PROP_READ;
    chars[SL_CTPROV_CHAR_META].encrypted = false;   // META is readable before pairing

    profile->uuid128    = s_ctprov_service_uuid;
    profile->chars      = chars;
    profile->char_count = SL_CTPROV_CHAR_COUNT;
}

bool sl_ble_ctprov_name_is_valid(const char *device_name)
{
    size_t len = (device_name != NULL) ? strlen(device_name) : 0;
    size_t dash = 0;
    size_t dashes = 0;

    if (len < SL_CTPROV_NAME_MIN_LEN || len > SL_CTPROV_NAME_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = device_name[i];
        bool upper_or_digit = ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
        if (c == '-') {
            dash = i;
            dashes++;
        } else if (!upper_or_digit) {
            return false;
        }
    }
    // "<MODEL>-<6 hex characters>": the separator is followed by the 6-character MAC suffix
    return (dashes == 1) && (dash >= 2) && (dash <= SL_CTPROV_NAME_MAX_LEN - 7) && (dash == len - 7);
}

uint8_t sl_ble_ctprov_adv_data(uint8_t *adv, uint8_t model_code, const uint8_t mac_suffix[3], uint8_t flags)
{
    // Flags: General Discoverable, BR/EDR Not Supported
    adv[0] = 0x02;
    adv[1] = SL_CTPROV_AD_FLAGS;
    adv[2] = 0x06;
    // Service Data - 128-bit UUID: 23 bytes follow the AD type
    adv[3] = 0x17;
    adv[4] = SL_CTPROV_AD_SERVICE_DATA;
    memcpy(&adv[5], s_ctprov_service_uuid, SL_BLE_GATT_UUID128_LEN);
    adv[21] = SL_CTPROV_FORMAT_VERSION;
    adv[22] = model_code;
    memcpy(&adv[23], mac_suffix, 3);
    adv[26] = (uint8_t)(flags & SL_CTPROV_FLAGS_MASK);
    return 27;
}

uint8_t sl_ble_ctprov_scan_response(uint8_t *scan_rsp, const char *device_name)
{
    size_t len = (device_name != NULL) ? strlen(device_name) : 0;

    if (!sl_ble_ctprov_name_is_valid(device_name)) {
        return 0;
    }
    scan_rsp[0] = (uint8_t)(1 + len);   // AD type byte + name, without the Length byte itself
    scan_rsp[1] = SL_CTPROV_AD_COMPLETE_NAME;
    memcpy(&scan_rsp[2], device_name, len);
    return (uint8_t)(2 + len);
}

uint8_t sl_ble_ctprov_meta(uint8_t *meta, const char *device_name, uint8_t flags)
{
    size_t len = (device_name != NULL) ? strlen(device_name) : 0;

    if (!sl_ble_ctprov_name_is_valid(device_name)) {
        return 0;
    }
    meta[0] = SL_CTPROV_FORMAT_VERSION;
    meta[1] = (uint8_t)(flags & SL_CTPROV_FLAGS_MASK);
    meta[2] = (uint8_t)len;
    memcpy(&meta[3], device_name, len);
    return (uint8_t)(3 + len);
}
#endif
