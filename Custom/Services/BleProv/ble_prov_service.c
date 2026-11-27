#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "debug.h"
#include "ble_prov_service.h"
#include "ble_prov_config.h"
#include "ct_prov_codec.h"
#include "ct_prov_dispatch.h"
#include "sl_rsi_ble_gatt.h"
#include "sl_rsi_ble_ctprov.h"
#include "sl_utility.h"
#include "device_service.h"
#include "web_server.h"
#include "drtc.h"
#include "storage.h"
#include "buffer_mgr.h"
#include "common_utils.h"

#define LOG_TAG "[BLE_PROV] "

#define BLE_PROV_WINDOW_MS           (5U * 60U * 1000U)  ///< advertising stays on this long after a double click
#define BLE_PROV_RECONNECT_GRACE_MS  (60U * 1000U)       ///< advertising resumes this long after a link drops mid-session
#define BLE_PROV_SESSION_IDLE_MS     (5U * 60U * 1000U)  ///< a silent link is dropped after this
#define BLE_PROV_TICK_MS             2000U
#define BLE_PROV_NWP_READY_WAIT_MS   10000U              ///< double click right after boot: wait for the Wi-Fi chip
#define BLE_PROV_WRITE_QUEUE_LEN     8U
#define BLE_PROV_FRAME_MAX           (SL_BLE_GATT_ATT_VALUE_MAX + CTPROV_FRAG_HEADER_SIZE)

#define BLE_PROV_BOND_MAX            4U
#define BLE_PROV_BOND_NVS_KEY         "ble_bonds"
#define BLE_PROV_BOND_MAGIC           0x424C4B31U        /* "BLK1" */

typedef enum {
    PROV_OFF = 0,
    PROV_ADVERTISING,
    PROV_CONNECTED,     ///< link up, the client has not talked yet
    PROV_READY,         ///< the client sent its first request
} ble_prov_state_t;

static const char *const s_state_name[] = { "off", "advertising", "connected", "ready" };

// Events of the service thread
#define EVT_TOGGLE        0x0001
#define EVT_WRITE         0x0002
#define EVT_CONNECT       0x0004
#define EVT_DISCONNECT    0x0008
#define EVT_TICK          0x0010
#define EVT_ALL           0x001F

typedef struct {
    uint8_t dev_addr[6];
    uint16_t handle;
    uint16_t len;
    bool with_response;
    uint8_t data[SL_BLE_GATT_ATT_VALUE_MAX];
} ble_prov_write_t;

typedef struct {
    uint32_t magic;
    uint32_t count;
    sl_ble_gatt_bond_t bonds[BLE_PROV_BOND_MAX];
} ble_prov_bond_store_t;

static struct {
    bool running;
    volatile uint8_t state;
    bool hal_ready;         ///< callbacks registered on the current NWP driver instance
    bool subscribed;        ///< client enabled D2C indications
    bool delivered;         ///< a config.set was stored in this window
    bool close_pending;     ///< session.close answered; drop the link
    bool led_held;
    uint32_t window_deadline;
    uint32_t last_activity;
    uint8_t tx_message_id;
    char name[SL_CTPROV_NAME_MAX_LEN + 1];
    uint32_t wq_head;       ///< consumed by the service thread
    uint32_t wq_tail;       ///< produced by the HAL thread
    osThreadId_t worker;
    osEventFlagsId_t events;
    osTimerId_t timer;
    osSemaphoreId_t apply_sem;
    bool apply_thread_started;
} g_prov;

// Large buffers live in PSRAM
static ct_prov_assembler_t g_rx IN_PSRAM;
static uint8_t g_tx[CTPROV_PACKET_MAX] IN_PSRAM;
static uint8_t g_frame[BLE_PROV_FRAME_MAX] IN_PSRAM;
static ble_prov_write_t g_write_queue[BLE_PROV_WRITE_QUEUE_LEN] IN_PSRAM;
static ble_prov_bond_store_t g_bonds IN_PSRAM;

static sl_ble_gatt_char_t g_chars[SL_CTPROV_CHAR_COUNT];
static sl_ble_gatt_profile_t g_profile;
static ct_prov_session_t g_session;
static bool g_bond_store_loaded;

static uint8_t g_worker_stack[1024 * 16] ALIGN_32 IN_PSRAM;
static uint8_t g_apply_stack[1024 * 16] ALIGN_32 IN_PSRAM;

static void apply_request(void);

// ==================== bond store ====================

static void bonds_save(void)
{
    // storage_nvs_write() returns the number of bytes written, a negative
    // errno on failure
    if (storage_nvs_write(NVS_USER, BLE_PROV_BOND_NVS_KEY, &g_bonds, sizeof(g_bonds)) < 0) {
        LOG_SVC_ERROR(LOG_TAG "failed to save pairing keys\r\n");
    }
}

static void bonds_load(void)
{
    ble_prov_bond_store_t *stored = (ble_prov_bond_store_t *)buffer_calloc(1, sizeof(*stored));

    g_bonds.magic = BLE_PROV_BOND_MAGIC;
    g_bonds.count = 0;
    g_bond_store_loaded = true;
    if (stored == NULL) {
        return;
    }
    if (storage_nvs_read(NVS_USER, BLE_PROV_BOND_NVS_KEY, stored, sizeof(*stored)) > 0 &&
        stored->magic == BLE_PROV_BOND_MAGIC && stored->count <= BLE_PROV_BOND_MAX) {
        memcpy(&g_bonds, stored, sizeof(g_bonds));
        LOG_SVC_INFO(LOG_TAG "%u bonded client(s) loaded\r\n", (unsigned)g_bonds.count);
    }
    buffer_free(stored);
}

static void bonds_forget(void)
{
    g_bonds.count = 0;
    bonds_save();
}

static int bond_find(const uint8_t dev_addr[6])
{
    for (uint32_t i = 0; i < g_bonds.count; i++) {
        if (memcmp(g_bonds.bonds[i].dev_addr, dev_addr, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// ==================== HAL callbacks (HAL thread) ====================

static void hal_on_connect(const uint8_t dev_addr[6])
{
    (void)dev_addr;
    osEventFlagsSet(g_prov.events, EVT_CONNECT);
}

static void hal_on_disconnect(const uint8_t dev_addr[6], uint16_t reason)
{
    (void)dev_addr;
    (void)reason;
    osEventFlagsSet(g_prov.events, EVT_DISCONNECT);
}

static void hal_on_write(const uint8_t dev_addr[6], uint16_t handle, const uint8_t *data, uint16_t len, bool with_response)
{
    uint32_t next = (g_prov.wq_tail + 1) % BLE_PROV_WRITE_QUEUE_LEN;

    // The ATT acknowledgement only says "received": answer it right away, the
    // service thread processes the content.
    if (with_response) {
        sl_ble_gatt_write_response(dev_addr);
    }
    if (next == g_prov.wq_head) {
        LOG_SVC_WARN(LOG_TAG "write queue full, fragment dropped\r\n");
        return;
    }
    ble_prov_write_t *item = &g_write_queue[g_prov.wq_tail];
    memcpy(item->dev_addr, dev_addr, 6);
    item->handle        = handle;
    item->with_response = with_response;
    item->len           = (len > sizeof(item->data)) ? (uint16_t)sizeof(item->data) : len;
    memcpy(item->data, data, item->len);
    g_prov.wq_tail = next;
    osEventFlagsSet(g_prov.events, EVT_WRITE);
}

static void hal_on_read(const uint8_t dev_addr[6], uint16_t handle, uint16_t offset)
{
    uint8_t meta[SL_CTPROV_META_MAX_LEN];
    uint8_t len;

    // META is the only readable attribute; the HAL thread may call blocking
    // SDK functions (it is not the SDK event thread)
    len = sl_ble_ctprov_meta(meta, g_prov.name, SL_CTPROV_FLAG_PROVISIONABLE);
    if (handle == g_chars[SL_CTPROV_CHAR_META].handle && len > 0 && offset < len) {
        sl_ble_gatt_read_response(dev_addr, handle, offset, &meta[offset], (uint16_t)(len - offset));
    } else {
        sl_ble_gatt_read_error(dev_addr, handle, SL_BLE_GATT_ATT_ERR_READ_NOT_PERMITTED);
    }
}

static void hal_on_subscribe(const uint8_t dev_addr[6], uint16_t handle, bool enabled)
{
    (void)dev_addr;
    if (handle == g_chars[SL_CTPROV_CHAR_D2C].handle) {
        g_prov.subscribed = enabled;
        LOG_SVC_INFO(LOG_TAG "D2C indications %s\r\n", enabled ? "on" : "off");
    }
}

static void hal_on_mtu(const uint8_t dev_addr[6], uint16_t mtu)
{
    (void)dev_addr;
    LOG_SVC_INFO(LOG_TAG "link MTU %u\r\n", mtu);
}

static void hal_on_encrypt(const uint8_t dev_addr[6], bool encrypted)
{
    LOG_SVC_INFO(LOG_TAG "link %s: %02X:%02X:%02X:%02X:%02X:%02X\r\n", encrypted ? "encrypted" : "not encrypted",
                 dev_addr[5], dev_addr[4], dev_addr[3], dev_addr[2], dev_addr[1], dev_addr[0]);
}

static bool hal_is_bonded(const uint8_t dev_addr[6])
{
    return bond_find(dev_addr) >= 0;
}

static bool hal_on_ltk_request(const uint8_t dev_addr[6], uint16_t ediv, const uint8_t rand[SL_BLE_GATT_RAND_LEN],
                               uint8_t ltk[SL_BLE_GATT_LTK_LEN])
{
    int index = bond_find(dev_addr);

    if (index < 0 || g_bonds.bonds[index].ediv != ediv ||
        memcmp(g_bonds.bonds[index].rand, rand, SL_BLE_GATT_RAND_LEN) != 0) {
        LOG_SVC_WARN(LOG_TAG "no pairing key for this client, encryption refused\r\n");
        return false;
    }
    memcpy(ltk, g_bonds.bonds[index].ltk, SL_BLE_GATT_LTK_LEN);
    return true;
}

static void hal_on_bond(const sl_ble_gatt_bond_t *bond)
{
    int index = bond_find(bond->dev_addr);

    // Reported again at every re-encryption with a stored key: only a new or
    // changed key is worth a flash write
    if (index >= 0 && memcmp(&g_bonds.bonds[index], bond, sizeof(*bond)) == 0) {
        return;
    }

    if (index < 0) {
        if (g_bonds.count >= BLE_PROV_BOND_MAX) {
            // Oldest entry first: drop it for the new client
            memmove(&g_bonds.bonds[0], &g_bonds.bonds[1], (BLE_PROV_BOND_MAX - 1) * sizeof(g_bonds.bonds[0]));
            g_bonds.count = BLE_PROV_BOND_MAX - 1;
        }
        index = (int)g_bonds.count++;
    }
    g_bonds.bonds[index] = *bond;
    LOG_SVC_INFO(LOG_TAG "pairing keys stored for %02X:%02X:%02X:%02X:%02X:%02X (%u/%u)\r\n",
                 bond->dev_addr[5], bond->dev_addr[4], bond->dev_addr[3], bond->dev_addr[2], bond->dev_addr[1],
                 bond->dev_addr[0], (unsigned)g_bonds.count, BLE_PROV_BOND_MAX);
    bonds_save();
}

static const sl_ble_gatt_cbs_t g_hal_cbs = {
    .on_connect     = hal_on_connect,
    .on_disconnect  = hal_on_disconnect,
    .on_write       = hal_on_write,
    .on_read        = hal_on_read,
    .on_subscribe   = hal_on_subscribe,
    .on_mtu         = hal_on_mtu,
    .on_encrypt     = hal_on_encrypt,
    .is_bonded     = hal_is_bonded,
    .on_ltk_request = hal_on_ltk_request,
    .on_bond        = hal_on_bond,
};

// ==================== dispatcher backend ====================

static void cb_device_info(void *user, cJSON *data, ct_prov_result_t *result)
{
    (void)user;
    ble_prov_config_device_info(data, result);
}

static void cb_capabilities(void *user, cJSON *data, ct_prov_result_t *result)
{
    (void)user;
    ble_prov_config_capabilities(data, result);
}

static void cb_scan(void *user, const char *section, const char *cursor, cJSON *data, ct_prov_result_t *result)
{
    (void)user;
    ble_prov_config_scan(section, cursor, data, result);
}

static void cb_config_set(void *user, const cJSON *sections, cJSON *data, ct_prov_result_t *result)
{
    (void)user;
    ble_prov_config_set(sections, data, result);
    if (result->code == CTPROV_OK) {
        // Stored: bring it up on the apply thread so this link stays responsive
        g_prov.delivered = true;
        apply_request();
    }
}

static void cb_config_get(void *user, cJSON *data, ct_prov_result_t *result)
{
    (void)user;
    ble_prov_config_get(data, result);
}

static void cb_session_close(void *user)
{
    (void)user;
    g_prov.close_pending = true;
}

static const ct_prov_backend_t g_backend = {
    .user          = NULL,
    .device_info   = cb_device_info,
    .capabilities  = cb_capabilities,
    .scan          = cb_scan,
    .config_set    = cb_config_set,
    .config_get    = cb_config_get,
    .session_close = cb_session_close,
};

// ==================== link output ====================

static void send_packet(uint8_t kind, const uint8_t *payload, uint16_t len)
{
    ct_prov_slicer_t slicer;
    uint16_t mtu = sl_ble_gatt_mtu();
    uint16_t frame_len;
    int32_t status = SL_BLE_GATT_SUCCESS;

    if (!g_prov.subscribed) {
        LOG_SVC_WARN(LOG_TAG "response dropped: D2C is not subscribed\r\n");
        return;
    }
    if (mtu < SL_BLE_GATT_ATT_MTU_DEFAULT) {
        mtu = SL_BLE_GATT_ATT_MTU_DEFAULT;
    }
    if (!ct_prov_slicer_init(&slicer, kind, g_prov.tx_message_id++, payload, len, mtu)) {
        LOG_SVC_ERROR(LOG_TAG "response does not fit the link (len=%u mtu=%u)\r\n", len, mtu);
        return;
    }
    while ((frame_len = ct_prov_slicer_next(&slicer, g_frame, sizeof(g_frame))) > 0 && status == SL_BLE_GATT_SUCCESS) {
        status = sl_ble_gatt_indicate(g_chars[SL_CTPROV_CHAR_D2C].handle, g_frame, frame_len);
    }
    if (status != SL_BLE_GATT_SUCCESS) {
        LOG_SVC_ERROR(LOG_TAG "indication failed: %ld\r\n", (long)status);
    }
}

static void send_protocol_error(ct_prov_err_t code, const char *message)
{
    uint16_t len = ct_prov_dispatch_protocol_error(&g_session, code, message, g_tx, sizeof(g_tx));

    LOG_SVC_WARN(LOG_TAG "protocol error: %s (%s)\r\n", ct_prov_err_name(code), message);
    if (len > 0) {
        send_packet(CTPROV_PACKET_EVENT, g_tx, len);
    }
}

// ==================== window state ====================

static void led_take(void)
{
    device_service_indicator_hold(AICAM_TRUE);
    device_service_set_indicator_state(SYSTEM_INDICATOR_BLE_PROVISIONING);
    g_prov.led_held = true;
}

static void led_release(void)
{
    if (g_prov.led_held) {
        device_service_indicator_hold(AICAM_FALSE);
        g_prov.led_held = false;
    }
}

/// @brief Name suffix "<MODEL>-XXXXXX" -> the 3 MAC bytes in display order
static void mac_suffix_from_name(uint8_t suffix[3])
{
    const char *hex = &g_prov.name[strlen(g_prov.name) - 6];

    memset(suffix, 0, 3);
    for (int i = 0; i < 6; i++) {
        char c = hex[i];
        uint8_t nibble = (c >= '0' && c <= '9') ? (uint8_t)(c - '0') : (uint8_t)(c - 'A' + 10);
        suffix[i / 2] |= (uint8_t)(nibble << ((i % 2) ? 0 : 4));
    }
}

/// @brief Wi-Fi chip up, HAL callbacks registered, GATT database present.
///        Blocking; false when the chip did not come up in time.
static bool ble_prov_prepare(void)
{
    uint8_t adv[SL_BLE_GATT_ADV_DATA_MAX];
    uint8_t scan_rsp[SL_BLE_GATT_ADV_DATA_MAX];
    uint8_t mac_suffix[3];
    int32_t status;

    for (uint32_t waited = 0; !sl_si91x_is_device_initialized(); waited += 500) {
        if (waited >= BLE_PROV_NWP_READY_WAIT_MS) {
            LOG_SVC_ERROR(LOG_TAG "Wi-Fi chip is not up, provisioning window not opened\r\n");
            return false;
        }
        osDelay(500);
    }
    // A fresh NWP driver instance starts with an empty callback table
    if (!g_prov.hal_ready) {
        status = sl_ble_gatt_init(&g_hal_cbs);
        if (status != SL_BLE_GATT_SUCCESS) {
            LOG_SVC_ERROR(LOG_TAG "BLE HAL init failed: %ld\r\n", (long)status);
            return false;
        }
        g_prov.hal_ready = true;
    }
    if (sl_ble_gatt_profile_is_valid()) {
        return true;
    }
    ble_prov_config_device_name(g_prov.name, sizeof(g_prov.name));
    sl_ble_ctprov_profile_init(&g_profile, g_chars);
    status = sl_ble_gatt_profile_create(&g_profile);
    if (status != SL_BLE_GATT_SUCCESS) {
        LOG_SVC_ERROR(LOG_TAG "GATT profile creation failed: %ld\r\n", (long)status);
        return false;
    }
    mac_suffix_from_name(mac_suffix);
    status = sl_ble_gatt_adv_data_set(adv, sl_ble_ctprov_adv_data(adv, SL_CTPROV_MODEL_CODE_NE301, mac_suffix,
                                                                  SL_CTPROV_FLAG_PROVISIONABLE),
                                      scan_rsp, sl_ble_ctprov_scan_response(scan_rsp, g_prov.name));
    if (status != SL_BLE_GATT_SUCCESS) {
        LOG_SVC_ERROR(LOG_TAG "advertising data rejected: %ld\r\n", (long)status);
        sl_ble_gatt_profile_invalidate();
        return false;
    }
    LOG_SVC_INFO(LOG_TAG "GATT profile ready, device name %s\r\n", g_prov.name);
    return true;
}

static void session_reset(void)
{
    ct_prov_assembler_reset(&g_rx);
    ct_prov_session_init(&g_session);
    ble_prov_config_scan_reset();
    g_prov.wq_head = g_prov.wq_tail;
    g_prov.subscribed     = false;
    g_prov.close_pending  = false;
    g_prov.tx_message_id  = 0;
}

static void adv_start(void)
{
    int32_t status = sl_ble_gatt_adv_start();

    if (status != SL_BLE_GATT_SUCCESS) {
        LOG_SVC_ERROR(LOG_TAG "advertising failed to start: %ld\r\n", (long)status);
        return;
    }
    g_prov.state = PROV_ADVERTISING;
    LOG_SVC_INFO(LOG_TAG "advertising as %s\r\n", g_prov.name);
}

static void window_open(void)
{
    session_reset();
    g_prov.delivered       = false;
    g_prov.window_deadline = (uint32_t)rtc_get_uptime_ms() + BLE_PROV_WINDOW_MS;
    g_prov.last_activity   = (uint32_t)rtc_get_uptime_ms();

    led_take();
    if (ble_prov_prepare()) {
        adv_start();
    } else {
        led_release();
        g_prov.state = PROV_OFF;
    }
}

static void window_close(const char *why)
{
    if (sl_ble_gatt_is_connected()) {
        sl_ble_gatt_disconnect();
    }
    sl_ble_gatt_adv_stop();
    g_prov.state = PROV_OFF;
    led_release();
    LOG_SVC_INFO(LOG_TAG "provisioning window closed (%s)\r\n", why);
}

static void ble_prov_apply_thread(void *argument)
{
    (void)argument;

    for (;;) {
        osSemaphoreAcquire(g_prov.apply_sem, osWaitForever);
        ble_prov_config_set_applying(true);
        ble_prov_config_apply();
        ble_prov_config_set_applying(false);
    }
}

static void apply_request(void)
{
    if (!g_prov.apply_thread_started) {
        static const osThreadAttr_t apply_attr = {
            .name = "bleProvApply",
            .priority = (osPriority_t) osPriorityBelowNormal,
            .stack_mem = g_apply_stack,
            .stack_size = sizeof(g_apply_stack),
        };
        if (osThreadNew(ble_prov_apply_thread, NULL, &apply_attr) == NULL) {
            LOG_SVC_ERROR(LOG_TAG "apply thread unavailable\r\n");
            return;
        }
        g_prov.apply_thread_started = true;
    }
    osSemaphoreRelease(g_prov.apply_sem);
}

// ==================== service thread ====================

static void handle_write(const ble_prov_write_t *item)
{
    uint32_t now = (uint32_t)rtc_get_uptime_ms();
    ct_prov_packet_t packet;
    const char *reason = NULL;
    ct_prov_feed_result_t feed;
    uint16_t len;

    if (g_prov.state != PROV_CONNECTED && g_prov.state != PROV_READY) {
        return;
    }
    g_prov.state       = PROV_READY;
    g_prov.last_activity = now;

    if (ct_prov_assembler_timed_out(&g_rx, now)) {
        ct_prov_assembler_reset(&g_rx);
        send_protocol_error(CTPROV_ERR_INVALID_MESSAGE, "fragment reassembly timeout");
    }
    feed = ct_prov_assembler_feed(&g_rx, item->data, item->len, now, &packet, &reason);
    if (feed == CTPROV_FEED_REJECTED) {
        send_protocol_error(CTPROV_ERR_INVALID_MESSAGE, reason);
        return;
    }
    if (feed != CTPROV_FEED_COMPLETE) {
        return;
    }
    if (packet.kind != CTPROV_PACKET_REQUEST || packet.len == 0) {
        send_protocol_error(CTPROV_ERR_INVALID_MESSAGE, packet.len ? "unexpected packet kind" : "empty packet");
        return;
    }

    LOG_SVC_INFO(LOG_TAG "request (%u bytes)\r\n", packet.len);
    len = ct_prov_dispatch_request(&g_session, &g_backend, packet.bytes, packet.len, g_tx, sizeof(g_tx));
    if (len == 0) {
        send_protocol_error(CTPROV_ERR_INTERNAL_ERROR, "response does not fit");
        return;
    }
    send_packet(CTPROV_PACKET_RESPONSE, g_tx, len);

    if (g_prov.close_pending) {
        // session.close was answered: the server disconnects (spec section 11)
        g_prov.close_pending = false;
        sl_ble_gatt_disconnect();
    }
}

static void handle_disconnect(void)
{
    uint32_t now = (uint32_t)rtc_get_uptime_ms();
    bool finished = g_prov.delivered || g_prov.close_pending;

    session_reset();
    if (finished) {
        window_close("session complete");
    } else {
        // Link lost mid-session (or between pairing and GATT): let the client
        // reconnect, but only within the grace, not the whole window
        g_prov.window_deadline = now + BLE_PROV_RECONNECT_GRACE_MS;
        adv_start();
    }
}

static void handle_tick(uint32_t now)
{
    // Keep the device awake and its AP up while the window is open
    web_server_ap_sleep_timer_reset();

    if (!sl_ble_gatt_profile_is_valid()) {
        // A recovery path rebooted the Wi-Fi chip: database, callbacks and
        // link are gone; start over so the client can reconnect.
        LOG_SVC_WARN(LOG_TAG "Wi-Fi chip was restarted, rebuilding the BLE server\r\n");
        g_prov.hal_ready = false;
        bool reopen = (g_prov.state == PROV_ADVERTISING);   // a live session cannot be resumed
        session_reset();
        g_prov.state = PROV_OFF;
        if (reopen && (int32_t)(g_prov.window_deadline - now) > 0 && ble_prov_prepare()) {
            adv_start();
        } else {
            window_close("Wi-Fi chip restarted");
        }
        return;
    }
    if (g_prov.state == PROV_ADVERTISING && (int32_t)(now - g_prov.window_deadline) >= 0) {
        window_close("no client within the window");
    } else if (g_prov.state != PROV_OFF && g_prov.state != PROV_ADVERTISING &&
               (uint32_t)(now - g_prov.last_activity) >= BLE_PROV_SESSION_IDLE_MS) {
        window_close("session idle");
    }
}

static void ble_prov_worker(void *argument)
{
    (void)argument;

    while (g_prov.running) {
        uint32_t flags = osEventFlagsWait(g_prov.events, EVT_ALL, osFlagsWaitAny, osWaitForever);

        if (!(flags & EVT_ALL)) {
            continue;
        }
        if (flags & EVT_TOGGLE) {
            if (g_prov.state != PROV_OFF) {
                window_close("double click");
            } else {
                window_open();
            }
        }
        if (g_prov.state == PROV_OFF) {
            continue;
        }
        if (flags & EVT_DISCONNECT) {
            handle_disconnect();
        }
        if (g_prov.state == PROV_OFF) {
            continue;
        }
        if (flags & EVT_CONNECT) {
            session_reset();
            g_prov.state         = PROV_CONNECTED;
            g_prov.last_activity = (uint32_t)rtc_get_uptime_ms();
            // A live session keeps the window open: a reconnect after it must
            // still be inside the grace, not outside the original deadline
            g_prov.window_deadline = g_prov.last_activity + BLE_PROV_WINDOW_MS;
        }
        if (flags & EVT_WRITE) {
            while (g_prov.wq_head != g_prov.wq_tail && g_prov.state != PROV_OFF) {
                ble_prov_write_t item = g_write_queue[g_prov.wq_head];
                g_prov.wq_head = (g_prov.wq_head + 1) % BLE_PROV_WRITE_QUEUE_LEN;
                handle_write(&item);
            }
        }
        if (g_prov.state != PROV_OFF && (flags & EVT_TICK)) {
            handle_tick((uint32_t)rtc_get_uptime_ms());
        }
    }
}

static void ble_prov_tick(void *argument)
{
    (void)argument;
    osEventFlagsSet(g_prov.events, EVT_TICK);
}

// ==================== debug command ====================

static int ble_prov_cmd(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: bleprov <status|on|off|bonds|forget>\r\n");
        return -1;
    }
    if (strcmp(argv[1], "status") == 0) {
        printf("ble prov: window=%s state=%s profile=%u adv=%u conn=%u enc=%u mtu=%u bonds=%u/%u\r\n",
               ble_prov_service_is_active() ? "open" : "closed", s_state_name[g_prov.state],
               sl_ble_gatt_profile_is_valid() ? 1 : 0, sl_ble_gatt_is_advertising() ? 1 : 0,
               sl_ble_gatt_is_connected() ? 1 : 0, sl_ble_gatt_is_encrypted() ? 1 : 0, sl_ble_gatt_mtu(),
               (unsigned)g_bonds.count, BLE_PROV_BOND_MAX);
        return 0;
    }
    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        bool want = (argv[1][1] == 'n');
        if (!g_prov.running) {
            printf("ble prov: service is not running\r\n");
            return -1;
        }
        if (want != ble_prov_service_is_active()) {
            osEventFlagsSet(g_prov.events, EVT_TOGGLE);
        }
        printf("ble prov: provisioning window %s requested\r\n", want ? "open" : "close");
        return 0;
    }
    if (strcmp(argv[1], "bonds") == 0) {
        for (uint32_t i = 0; i < g_bonds.count; i++) {
            const sl_ble_gatt_bond_t *bond = &g_bonds.bonds[i];
            printf("  [%u] %02X:%02X:%02X:%02X:%02X:%02X ediv=0x%04X sc=%u\r\n", (unsigned)i, bond->dev_addr[5],
                   bond->dev_addr[4], bond->dev_addr[3], bond->dev_addr[2], bond->dev_addr[1], bond->dev_addr[0],
                   bond->ediv, bond->secure_connections ? 1 : 0);
        }
        printf("ble prov: %u bonded client(s)\r\n", (unsigned)g_bonds.count);
        return 0;
    }
    if (strcmp(argv[1], "forget") == 0) {
        // The clients keep their side of the pairing: remove it there too before reconnecting
        bonds_forget();
        printf("ble prov: pairing keys cleared\r\n");
        return 0;
    }
    printf("Usage: bleprov <status|on|off|bonds|forget>\r\n");
    return -1;
}

static debug_cmd_reg_t ble_prov_cmd_table[] = {
    { "bleprov", "BLE provisioning: bleprov <status|on|off|bonds|forget>", ble_prov_cmd },
};

// ==================== public API ====================

void ble_prov_service_toggle(void)
{
    if (g_prov.running) {
        osEventFlagsSet(g_prov.events, EVT_TOGGLE);
    }
}

aicam_bool_t ble_prov_service_is_active(void)
{
    return (g_prov.state != PROV_OFF) ? AICAM_TRUE : AICAM_FALSE;
}

aicam_result_t ble_prov_service_init(void *config)
{
    (void)config;
    if (!g_bond_store_loaded) {
        bonds_load();
    }
    debug_cmdline_register(ble_prov_cmd_table, 1);
    return AICAM_OK;
}

aicam_result_t ble_prov_service_start(void)
{
    static const osThreadAttr_t worker_attr = {
        .name = "bleProv",
        .priority = (osPriority_t) osPriorityBelowNormal,
        .stack_mem = g_worker_stack,
        .stack_size = sizeof(g_worker_stack),
    };

    if (g_prov.running) {
        return AICAM_OK;
    }
    if (g_prov.events == NULL) {
        g_prov.events = osEventFlagsNew(NULL);
    }
    if (g_prov.apply_sem == NULL) {
        g_prov.apply_sem = osSemaphoreNew(1, 0, NULL);
    }
    if (g_prov.events == NULL || g_prov.apply_sem == NULL) {
        return AICAM_ERROR_NO_MEMORY;
    }
    g_prov.state = PROV_OFF;
    g_prov.running = true;
    g_prov.worker = osThreadNew(ble_prov_worker, NULL, &worker_attr);
    if (g_prov.worker == NULL) {
        g_prov.running = false;
        return AICAM_ERROR_NO_MEMORY;
    }
    if (g_prov.timer == NULL) {
        g_prov.timer = osTimerNew(ble_prov_tick, osTimerPeriodic, NULL, NULL);
    }
    if (g_prov.timer != NULL) {
        osTimerStart(g_prov.timer, BLE_PROV_TICK_MS);
    }
    // The W5500 netif starts from compiled-in defaults: bring back a
    // provisioned static IP before the device reports itself ready
    ble_prov_config_eth_boot_apply();
    LOG_SVC_INFO(LOG_TAG "started (double click opens the provisioning window)\r\n");
    return AICAM_OK;
}

aicam_result_t ble_prov_service_stop(void)
{
    if (!g_prov.running) {
        return AICAM_OK;
    }
    if (g_prov.timer != NULL) {
        osTimerStop(g_prov.timer);
    }
    g_prov.running = false;
    osEventFlagsSet(g_prov.events, EVT_ALL);
    osDelay(100);
    if (g_prov.state != PROV_OFF) {
        window_close("service stopped");
    }
    g_prov.worker = NULL;
    return AICAM_OK;
}

aicam_result_t ble_prov_service_deinit(void)
{
    ble_prov_service_stop();
    return AICAM_OK;
}

service_state_t ble_prov_service_get_state(void)
{
    return g_prov.running ? SERVICE_STATE_RUNNING : SERVICE_STATE_UNINITIALIZED;
}
