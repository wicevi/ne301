#ifndef __BLE_PROV_CONFIG_H__
#define __BLE_PROV_CONFIG_H__

#include "cJSON.h"
#include "ct_prov_dispatch.h"

/// @brief CT-Prov operations on the device's communication configuration
///        (protocol spec sections 7-10), on top of the existing services:
///        communication_service (Wi-Fi / cellular / HaLow / network type),
///        mqtt_service, webhook and the upload coordinator.
//!
//!        config.set only validates and persists; what it stored is brought
//!        up afterwards by ble_prov_config_apply() so the client gets its
//!        response within its request timeout (association alone takes
//!        seconds).

#define BLE_PROV_MODEL_NAME          "NE301"
#define BLE_PROV_PROTOCOL_VERSION    1

/// @brief CT-Prov device name ("<MODEL>-<MAC suffix>"): the stored device name
///        when it already follows the pattern, else derived from the MAC.
void ble_prov_config_device_name(char *out, size_t out_cap);

/// @brief Fill data with the device.info result
void ble_prov_config_device_info(cJSON *data, ct_prov_result_t *result);

/// @brief Fill data with the communication.capabilities result
void ble_prov_config_capabilities(cJSON *data, ct_prov_result_t *result);

/// @brief communication.scan: one page of a snapshot. An empty cursor starts a
///        fresh scan; a cursor continues the snapshot of this session.
void ble_prov_config_scan(const char *section, const char *cursor, cJSON *data, ct_prov_result_t *result);

/// @brief config.set: validate every section, then persist all of them
///        (nothing is written when any section fails validation)
void ble_prov_config_set(const cJSON *sections, cJSON *data, ct_prov_result_t *result);

/// @brief config.get: currently stored values, secrets left out
void ble_prov_config_get(cJSON *data, ct_prov_result_t *result);

/// @brief Bring up what the last successful ble_prov_config_set() stored.
///        Blocking (network joins, type switch): run it on its own thread.
void ble_prov_config_apply(void);

/// @brief Re-apply the stored Ethernet/PoE IP mode at boot (the W5500 netif
///        starts from its compiled-in DHCP defaults).
void ble_prov_config_eth_boot_apply(void);

/// @brief Forget the scan snapshot (the cursor is only valid within a connection)
void ble_prov_config_scan_reset(void);

/// @brief Mark whether an apply is running: scan and config.set answer BUSY
///        while the network configuration paths are in use.
void ble_prov_config_set_applying(bool applying);

#endif
