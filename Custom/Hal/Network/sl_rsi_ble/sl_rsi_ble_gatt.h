#ifndef __SL_RSI_BLE_GATT_H__
#define __SL_RSI_BLE_GATT_H__

#ifdef SLI_SI91X_ENABLE_BLE

#include <stdint.h>
#include <stdbool.h>

/// @brief BLE GATT server (peripheral) role on top of the WiseConnect rsi_ble_* APIs.
///        Every function is a blocking SDK call: run them from a worker thread,
///        never from an SDK callback. Return values are the SDK's RSI_* statuses
///        (RSI_SUCCESS == 0). Device addresses use the SDK byte order (least
///        significant address byte first).
///        The GATT database lives in the NWP, so it must be recreated with
///        sl_ble_gatt_profile_create() after every NWP re-init.
///        The callbacks below run on the HAL worker thread, which only serialises
///        the SMP procedures: keep them short and hand long work to another
///        thread.

#define SL_BLE_GATT_SUCCESS           0       ///< SDK success status (RSI_SUCCESS)
#define SL_BLE_GATT_UUID128_LEN        16      ///< 128-bit UUID length in bytes
#define SL_BLE_GATT_ADDR_LEN           6       ///< device address length in bytes
#define SL_BLE_GATT_ADV_DATA_MAX       31      ///< Legacy advertising / scan response payload cap
#define SL_BLE_GATT_ATT_MTU_DEFAULT    23      ///< ATT MTU before an MTU exchange
#define SL_BLE_GATT_ATT_VALUE_MAX      240     ///< longest attribute value reported by the SDK
#define SL_BLE_GATT_LTK_LEN            16      ///< Long term key length in bytes
#define SL_BLE_GATT_RAND_LEN           8       ///< LTK diversifier random length in bytes

// Characteristic property bits (rsi_ble_common_config.h encoding)
#define SL_BLE_GATT_PROP_READ          0x02
#define SL_BLE_GATT_PROP_WRITE_NO_RESP 0x04
#define SL_BLE_GATT_PROP_WRITE         0x08
#define SL_BLE_GATT_PROP_NOTIFY        0x10
#define SL_BLE_GATT_PROP_INDICATE      0x20

// ATT error codes used by the server
#define SL_BLE_GATT_ATT_ERR_READ_NOT_PERMITTED   0x02
#define SL_BLE_GATT_ATT_ERR_INSUFFICIENT_AUTHEN  0x05

/// @brief One local characteristic. A NOTIFY/INDICATE characteristic gets a
///        CCCD; a client writing it is reported through on_subscribe().
typedef struct {
    const uint8_t *uuid128;         ///< 16 bytes in BLE little-endian byte order
    uint8_t property;               ///< SL_BLE_GATT_PROP_* bitmask
    bool encrypted;                 ///< true: ATT rejects access over an unencrypted link
    uint16_t handle;                ///< out: value handle reported by on_write()/on_read()
    uint16_t cccd_handle;           ///< out: CCCD handle, 0 when the characteristic has none
} sl_ble_gatt_char_t;

typedef struct {
    const uint8_t *uuid128;         ///< 16 bytes, BLE little-endian
    sl_ble_gatt_char_t *chars;      ///< characteristics in declaration order; caller keeps ownership
    uint8_t char_count;
} sl_ble_gatt_profile_t;

/// @brief Keys of one bonded peer. The device (peripheral) generates the LTK
///        during pairing and has to present it again whenever the peer asks to
///        encrypt a new link, so the upper layer must keep this across reboots.
typedef struct {
    uint8_t dev_addr[6];            ///< peer address, SDK byte order
    uint16_t ediv;                  ///< local EDIV the LTK is filed under (0 with LE Secure Connections)
    uint8_t rand[SL_BLE_GATT_RAND_LEN];   ///< local RAND the LTK is filed under
    uint8_t ltk[SL_BLE_GATT_LTK_LEN];     ///< local long term key
    uint8_t peer_irk[SL_BLE_GATT_LTK_LEN]; ///< IRK the peer distributed (all zero when it did not)
    bool secure_connections;        ///< paired with LE Secure Connections
} sl_ble_gatt_bond_t;

typedef struct {
    void (*on_connect)(const uint8_t dev_addr[6]);
    void (*on_disconnect)(const uint8_t dev_addr[6], uint16_t reason);
    /// @brief Client wrote a characteristic value. A write with response must be
    ///        acknowledged with sl_ble_gatt_write_response().
    void (*on_write)(const uint8_t dev_addr[6], uint16_t handle, const uint8_t *data, uint16_t len, bool with_response);
    /// @brief Client read a characteristic value: answer with
    ///        sl_ble_gatt_read_response() or sl_ble_gatt_read_error().
    void (*on_read)(const uint8_t dev_addr[6], uint16_t handle, uint16_t offset);
    /// @brief Client wrote a CCCD. handle is the characteristic's value handle.
    void (*on_subscribe)(const uint8_t dev_addr[6], uint16_t handle, bool enabled);
    void (*on_mtu)(const uint8_t dev_addr[6], uint16_t mtu);
    /// @brief True when pairing keys are stored for this peer. Bonded peers
    /// are never sent a security request: they re-encrypt on their own.
    bool (*is_bonded)(const uint8_t dev_addr[6]);
    /// @brief Link encryption state changed
    void (*on_encrypt)(const uint8_t dev_addr[6], bool encrypted);
    /// @brief The peer asks to encrypt with a stored key. Copy the local LTK
    ///        filed for this peer under (ediv, rand) into ltk and return true to
    ///        accept; false rejects the encryption.
    bool (*on_ltk_request)(const uint8_t dev_addr[6], uint16_t ediv, const uint8_t rand[SL_BLE_GATT_RAND_LEN],
                           uint8_t ltk[SL_BLE_GATT_LTK_LEN]);
    /// @brief First encryption with this peer produced the local keys: keep
    ///        them to accept the peer's re-encryption later (bonding).
    void (*on_bond)(const sl_ble_gatt_bond_t *bond);
} sl_ble_gatt_cbs_t;

/// @brief Register the GAP/GATT/SMP callbacks and start the HAL worker. Call
///        once the NWP is up (sl_wifi_init() done). This takes over the driver's
///        single callback table: the central-role helpers in sl_rsi_ble.h are
///        not usable at the same time.
int32_t sl_ble_gatt_init(const sl_ble_gatt_cbs_t *cbs);

/// @brief Create the local GATT database and arm the SMP responder. Fills in
///        the handles of profile->chars.
/// @return RSI_SUCCESS or the SDK status of the failed step
int32_t sl_ble_gatt_profile_create(const sl_ble_gatt_profile_t *profile);

/// @brief Drop the local database state after the NWP was re-initialized by
///        someone else (netif recovery, mode switch).
void sl_ble_gatt_profile_invalidate(void);

bool sl_ble_gatt_profile_is_valid(void);

/// @brief Set legacy advertising data and scan response (each <= 31 bytes).
int32_t sl_ble_gatt_adv_data_set(const uint8_t *adv, uint8_t adv_len,
                                 const uint8_t *scan_rsp, uint8_t scan_rsp_len);

/// @brief Start connectable undirected advertising on all three channels.
int32_t sl_ble_gatt_adv_start(void);
int32_t sl_ble_gatt_adv_stop(void);
bool sl_ble_gatt_is_advertising(void);
bool sl_ble_gatt_is_connected(void);
bool sl_ble_gatt_is_encrypted(void);

/// @brief Address of the connected peer (SDK byte order). Only valid while connected.
void sl_ble_gatt_peer_addr(uint8_t dev_addr[6]);

/// @brief Negotiated ATT MTU of the current link (23 until exchanged)
uint16_t sl_ble_gatt_mtu(void);

/// @brief Send an indication on a local characteristic value handle. Blocks
///        until the peer confirms (or the SDK gives up).
int32_t sl_ble_gatt_indicate(uint16_t handle, const uint8_t *data, uint16_t len);

/// @brief Answer an on_read() request
int32_t sl_ble_gatt_read_response(const uint8_t dev_addr[6], uint16_t handle, uint16_t offset,
                                  const uint8_t *data, uint16_t len);
/// @brief Reject an on_read() request with an ATT error code
int32_t sl_ble_gatt_read_error(const uint8_t dev_addr[6], uint16_t handle, uint8_t err);
/// @brief Acknowledge an on_write() request that had with_response set
int32_t sl_ble_gatt_write_response(const uint8_t dev_addr[6]);

/// @brief Disconnect the peer
int32_t sl_ble_gatt_disconnect(void);

#endif

#endif /* __SL_RSI_BLE_GATT_H__ */
