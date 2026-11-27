/**
 * @file ble_prov_service.h
 * @brief BLE provisioning service — CT-Prov/1 GATT server for the
 *        communication configuration (Wi-Fi / cellular / HaLow / MQTT /
 *        webhook). A double click on the button opens and closes the
 *        provisioning window.
 */
#ifndef BLE_PROV_SERVICE_H
#define BLE_PROV_SERVICE_H
#include "aicam_types.h"
#include "service_interfaces.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* ==================== Service Lifecycle ==================== */
aicam_result_t ble_prov_service_init(void *config);
aicam_result_t ble_prov_service_start(void);
aicam_result_t ble_prov_service_stop(void);
aicam_result_t ble_prov_service_deinit(void);
service_state_t ble_prov_service_get_state(void);

/**
 * @brief Open or close the provisioning window (double click entry point).
 *        Safe from any thread: the work runs on the service thread.
 */
void ble_prov_service_toggle(void);

/**
 * @brief Whether the window is open (advertising, or in a session)
 */
aicam_bool_t ble_prov_service_is_active(void);

#ifdef __cplusplus
}
#endif
#endif /* BLE_PROV_SERVICE_H */
