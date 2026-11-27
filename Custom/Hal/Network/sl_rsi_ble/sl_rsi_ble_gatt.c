#ifdef SLI_SI91X_ENABLE_BLE
#include <string.h>
// ble_config.h must precede rsi_ble_common_config.h: the latter's defaults are
// #ifndef-guarded, the former's are not (same order as the SDK's rsi_bt_ble.c)
#include "ble_config.h"
#include "Log/debug.h"
#include "rsi_common.h"
#include "sl_constants.h"
#include "rsi_ble_apis.h"
#include "rsi_ble.h"
#include "rsi_bt_common.h"
#include "rsi_ble_common_config.h"
#include "common_utils.h"
#include "sl_rsi_ble_gatt.h"

// The host keeps the attribute records: every read/write reaches the callbacks
// and the upper layer answers it from its own thread.
#define SL_BLE_GATT_ATT_CFG(sec)      (ATT_REC_MAINTAIN_IN_HOST | (sec) | ON_LE_LINK_ONLY)
#define SL_BLE_GATT_SEC_OPEN          SL_BLE_GATT_ATT_CFG(SEC_MODE_1_LEVEL_1)
#define SL_BLE_GATT_SEC_ENCRYPTED     SL_BLE_GATT_ATT_CFG(SEC_MODE_1_LEVEL_2)

// Undirected connectable advertising on all three channels
#define SL_BLE_GATT_ADV_INT_MIN       0x00A0  // 100 ms, in 0.625 ms units
#define SL_BLE_GATT_ADV_INT_MAX       0x0140  // 200 ms
#define SL_BLE_GATT_ADV_CHANNEL_MAP   0x07
#define SL_BLE_GATT_CCCD_VALUE_MASK   0x03    // notification | indication bits

// SMP: Just Works with bonding, No Input No Output.
#define SL_BLE_GATT_SMP_IO_CAP_NO_INPUT_NO_OUTPUT   0x03
// rsi_ble_set_smp_pairing_cap_data() auth_req bits: BIT(1) bonding, BIT(2)
// MITM, BIT(3) Secure Connections, BIT(4) keypress, BIT(5) CT2.
// No Secure Connections bit: with an LE SC bond (ediv 0) Windows never
// re-encrypts a reconnect by itself nor on a rejected protected access
// (NWP 2.17.6 + Windows 11), while a legacy bond - LTK distributed with
// ediv/rand - makes it encrypt right after connecting. SC was also not
// usable for Just Works provisioning security anyway.
#define SL_BLE_GATT_SMP_AUTHREQ_BONDING             0x02
#define SL_BLE_GATT_SMP_KEY_SIZE                    16
#define SL_BLE_GATT_SMP_KEY_DIST_ENC_ID             0x03    // EncKey | IdKey

// rsi_ble_ltk_req_reply() reply_type: BIT(0) positive, BIT(1) unauthenticated
// LTK/STK-based encryption, BIT(2) authenticated. Just Works bonds answer
// with BIT(0)|BIT(1): with only BIT(0) the NWP takes the link as encrypted
// but of unlabelled type and rejects SEC_MODE_1_LEVEL_2 attributes with
// Insufficient Authentication (NWP 2.17.6).
#define SL_BLE_GATT_LTK_REPLY_UNAUTH               0x03
#define SL_BLE_GATT_ENCRYPT_ENABLED                 0x01

// Events handed from the SDK event thread to the HAL worker
#define SL_BLE_GATT_EVT_CONNECT        0x0001
#define SL_BLE_GATT_EVT_DISCONNECT     0x0002
#define SL_BLE_GATT_EVT_SMP_RESPONSE   0x0004  ///< the peer sent its pairing request
#define SL_BLE_GATT_EVT_LTK_REQUEST    0x0008
#define SL_BLE_GATT_EVT_ENCRYPT        0x0010
#define SL_BLE_GATT_EVT_SMP_FAILED     0x0020
#define SL_BLE_GATT_EVT_WRITE          0x0040
#define SL_BLE_GATT_EVT_READ           0x0080
#define SL_BLE_GATT_EVT_BOND_RECYCLE   0x0100
#define SL_BLE_GATT_EVT_ALL            0x01FF

// The pairing ceremony link is encrypted with the short-term key, which the
// NWP does not accept for SEC_MODE_1_LEVEL_2 attributes, and Windows keeps
// that link (and attaches every later GATT session to it) for ~20 s after a
// Just Works pairing. Drop it once the bond has settled so the client's
// next connection is a fresh link that re-encrypts from the stored LTK.
// 5 s: the Windows PairAsync returned at most 3 s after the encryption in
// every run; dropping while it still pending would fail and remove the bond.
#define SL_BLE_GATT_BOND_RECYCLE_MS    5000

// The SDK callback context must never make a blocking rsi_ble_* call:
// rsi_driver_process_bt_resp_handler() both releases the sync semaphore and
// runs the event callbacks on the same RX thread, so a blocking call from a
// callback deadlocks until the semaphore's ~30 s timeout - every later event
// (write fragments, next read) waits behind it and the client times out. Read
// and write events are therefore copied into slots and handled on the HAL
// worker, like the official examples do (event -> flag -> application task).
#define SL_BLE_GATT_WRITE_QUEUE_LEN    4       ///< writes with response are sequential, 4 is generous

// Security request policy, from the Windows 11 + NWP 2.17.6 experiments:
// - a new peer is asked to secure the link right when it connects. Clients
//   without a system pairing API (mobile) need it to start pairing at all;
//   Windows coalesces it with the PairAsync call the app makes right away.
// - a bonded peer is never asked: re-encryption is the client's business
//   (Windows encrypts when a protected access fails, Android right after
//   connecting), and a security request colliding with that blocks this
//   HAL's worker inside the SDK call for half a minute.
// - nothing follows a pairing the peer ran itself: a second request then
//   makes Windows start a fresh non-bonded pairing that fails.

#define SL_BLE_GATT_WORKER_STACK_SIZE  (1024 * 4)

typedef struct {
    const sl_ble_gatt_cbs_t *cbs;
    sl_ble_gatt_profile_t profile;
    rsi_ble_resp_add_serv_t serv;
    bool profile_valid;
    bool advertising;
    bool connected;
    bool encrypted;
    bool peer_bonded;             ///< pairing keys were stored when the peer connected
    uint8_t peer_addr[RSI_DEV_ADDR_LEN];
    uint16_t mtu;

    osThreadId_t worker;
    osEventFlagsId_t events;
    osTimerId_t bond_recycle_timer;

    // One slot per event, written by the SDK event thread and consumed by the
    // worker. Each event type is strictly sequential on a single link, so a
    // slot never has to hold more than the latest occurrence.
    uint8_t connect_addr[RSI_DEV_ADDR_LEN];
    uint16_t connect_status;
    uint8_t disconnect_addr[RSI_DEV_ADDR_LEN];
    uint16_t disconnect_reason;
    uint8_t pair_addr[RSI_DEV_ADDR_LEN];
    rsi_ble_read_req_t read_req;                       ///< one ATT read is outstanding at a time
    rsi_ble_event_write_t write_queue[SL_BLE_GATT_WRITE_QUEUE_LEN];
    uint8_t wq_head;                                   ///< next entry the worker takes
    uint8_t wq_tail;                                   ///< next free entry
    rsi_bt_event_le_ltk_request_t ltk_request;
    rsi_bt_event_encryption_enabled_t encrypt;
    uint16_t encrypt_status;
    rsi_bt_event_le_security_keys_t security_keys;
    uint16_t smp_fail_status;
} sl_ble_gatt_state_t;

static sl_ble_gatt_state_t g_gatt = {0};

static uint8_t g_gatt_worker_stack[SL_BLE_GATT_WORKER_STACK_SIZE] ALIGN_32 IN_PSRAM;
static const osThreadAttr_t g_gatt_worker_attr = {
    .name = "bleGatt",
    .priority = (osPriority_t) osPriorityBelowNormal,
    .stack_mem = g_gatt_worker_stack,
    .stack_size = sizeof(g_gatt_worker_stack),
};

static sl_ble_gatt_char_t *sl_ble_gatt_char_by_cccd_handle(uint16_t handle)
{
    if (!g_gatt.profile_valid) {
        return NULL;
    }
    for (uint8_t i = 0; i < g_gatt.profile.char_count; i++) {
        if (g_gatt.profile.chars[i].cccd_handle == handle) {
            return &g_gatt.profile.chars[i];
        }
    }
    return NULL;
}

// ==================== SDK event thread: copy into the slots ====================

static void sl_ble_gatt_queue_connect(const uint8_t dev_addr[RSI_DEV_ADDR_LEN], uint16_t status)
{
    memcpy(g_gatt.connect_addr, dev_addr, RSI_DEV_ADDR_LEN);
    g_gatt.connect_status = status;
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_CONNECT);
}

static void sl_ble_gatt_connect_cb(rsi_ble_event_conn_status_t *conn_status)
{
    sl_ble_gatt_queue_connect(conn_status->dev_addr, conn_status->status);
}

static void sl_ble_gatt_enhanced_connect_cb(rsi_ble_event_enhance_conn_status_t *conn_status)
{
    // CONN_STATUS and ENHANCE_CONN_STATUS can both report the same link; the
    // worker drops the second report.
    if (g_gatt.connected) {
        return;
    }
    sl_ble_gatt_queue_connect(conn_status->dev_addr, conn_status->status);
}

static void sl_ble_gatt_disconnect_cb(rsi_ble_event_disconnect_t *disconnect, uint16_t reason)
{
    memcpy(g_gatt.disconnect_addr, disconnect->dev_addr, RSI_DEV_ADDR_LEN);
    g_gatt.disconnect_reason = reason;
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_DISCONNECT);
}

static void sl_ble_gatt_write_cb(uint16_t event_id, rsi_ble_event_write_t *write_event)
{
    // SDK event thread: park the event, the worker runs the callbacks
    uint8_t next = (uint8_t)((g_gatt.wq_tail + 1) % SL_BLE_GATT_WRITE_QUEUE_LEN);

    if (next == g_gatt.wq_head) {
        LOG_DRV_WARN("BLE write queue full, event dropped\r\n");
        return;
    }
    g_gatt.write_queue[g_gatt.wq_tail] = *write_event;
    g_gatt.wq_tail = next;
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_WRITE);
}

static void sl_ble_gatt_read_req_cb(uint16_t event_id, rsi_ble_read_req_t *read_req)
{
    // SDK event thread: park the request, the worker runs the callback
    g_gatt.read_req = *read_req;
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_READ);
}

static void sl_ble_gatt_mtu_cb(rsi_ble_event_mtu_t *mtu)
{
    g_gatt.mtu = mtu->mtu_size;
    LOG_DRV_INFO("BLE MTU: %u\r\n", g_gatt.mtu);
    if (g_gatt.cbs && g_gatt.cbs->on_mtu) {
        g_gatt.cbs->on_mtu(mtu->dev_addr, mtu->mtu_size);
    }
}

static void sl_ble_gatt_indicate_confirm_cb(uint16_t event_status, rsi_ble_set_att_resp_t *resp)
{
    LOG_DRV_DEBUG("BLE indication confirmed: status=0x%X\r\n", event_status);
}

static void sl_ble_gatt_smp_request_cb(rsi_bt_event_smp_req_t *request)
{
    // Reported in central mode (the peer's security request): nothing to answer.
    LOG_DRV_DEBUG("BLE smp security request from peer, auth_req=0x%02X\r\n", request->auth_req);
}

static void sl_ble_gatt_smp_response_cb(rsi_bt_event_smp_resp_t *response)
{
    // Reported in peripheral mode: the peer's pairing request
    memcpy(g_gatt.pair_addr, response->dev_addr, RSI_DEV_ADDR_LEN);
    LOG_DRV_INFO("BLE smp pairing request: io_cap=%u oob=%u auth_req=0x%02X\r\n", response->io_cap, response->oob_data,
                 response->auth_req);
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_SMP_RESPONSE);
}

static void sl_ble_gatt_smp_failed_cb(uint16_t status, rsi_bt_event_smp_failed_t *failed)
{
    (void)failed;
    g_gatt.smp_fail_status = status;
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_SMP_FAILED);
}

static void sl_ble_gatt_encrypt_started_cb(uint16_t status, rsi_bt_event_encryption_enabled_t *enc_enabled)
{
    memcpy(&g_gatt.encrypt, enc_enabled, sizeof(g_gatt.encrypt));
    g_gatt.encrypt_status = status;
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_ENCRYPT);
}

static void sl_ble_gatt_ltk_request_cb(rsi_bt_event_le_ltk_request_t *ltk_request)
{
    memcpy(&g_gatt.ltk_request, ltk_request, sizeof(g_gatt.ltk_request));
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_LTK_REQUEST);
}

static void sl_ble_gatt_security_keys_cb(rsi_bt_event_le_security_keys_t *keys)
{
    // Kept for the bond: the peer's IRK is only reported here
    memcpy(&g_gatt.security_keys, keys, sizeof(g_gatt.security_keys));
}

// ==================== HAL worker thread ====================

/// @brief Ask a new client to secure the link: it starts pairing (or
///        re-encrypts, when it kept the bond)
static void sl_ble_gatt_security_request(void){
    int32_t status = rsi_ble_smp_pair_request(g_gatt.peer_addr, SL_BLE_GATT_SMP_IO_CAP_NO_INPUT_NO_OUTPUT, 0);

    if (status != RSI_SUCCESS) {
        LOG_DRV_WARN("BLE smp security request failed: %ld\r\n", (long)status);
    }
}

static void sl_ble_gatt_handle_connect(void)
{
    if (g_gatt.connect_status != RSI_SUCCESS) {
        LOG_DRV_WARN("BLE connect failed: status=0x%X\r\n", g_gatt.connect_status);
        return;
    }

    memcpy(g_gatt.peer_addr, g_gatt.connect_addr, RSI_DEV_ADDR_LEN);
    g_gatt.connected   = true;
    g_gatt.advertising = false;   // connectable advertising ends with the connection
    g_gatt.encrypted   = false;
    g_gatt.peer_bonded = (g_gatt.cbs && g_gatt.cbs->is_bonded) ? g_gatt.cbs->is_bonded(g_gatt.peer_addr) : false;
    g_gatt.mtu         = SL_BLE_GATT_ATT_MTU_DEFAULT;
    memset(&g_gatt.security_keys, 0, sizeof(g_gatt.security_keys));

    LOG_DRV_INFO("BLE connected: %02X:%02X:%02X:%02X:%02X:%02X%s\r\n",
                 g_gatt.peer_addr[5], g_gatt.peer_addr[4], g_gatt.peer_addr[3],
                 g_gatt.peer_addr[2], g_gatt.peer_addr[1], g_gatt.peer_addr[0],
                 g_gatt.peer_bonded ? " (bonded)" : "");

    if (g_gatt.cbs && g_gatt.cbs->on_connect) {
        g_gatt.cbs->on_connect(g_gatt.peer_addr);
    }

    // Security request policy, see the notes above
    if (!g_gatt.peer_bonded) {
        sl_ble_gatt_security_request();
    }
}

static void sl_ble_gatt_handle_encrypt(void)
{
    rsi_bt_event_encryption_enabled_t *enc = &g_gatt.encrypt;
    sl_ble_gatt_bond_t bond;
    bool has_ltk = false;

    if ((g_gatt.encrypt_status != RSI_SUCCESS) || !(enc->enabled & SL_BLE_GATT_ENCRYPT_ENABLED)) {
        LOG_DRV_WARN("BLE encryption failed: status=0x%X enabled=0x%02X\r\n", g_gatt.encrypt_status, enc->enabled);
        g_gatt.encrypted = false;
        if (g_gatt.cbs && g_gatt.cbs->on_encrypt) {
            g_gatt.cbs->on_encrypt(enc->dev_addr, false);
        }
        return;
    }
    g_gatt.encrypted = true;

    for (uint8_t i = 0; i < SL_BLE_GATT_LTK_LEN; i++) {
        if (enc->localltk[i] != 0) {
            has_ltk = true;
        }
    }
    if (has_ltk && g_gatt.cbs && g_gatt.cbs->on_bond) {
        memset(&bond, 0, sizeof(bond));
        memcpy(bond.dev_addr, enc->dev_addr, RSI_DEV_ADDR_LEN);
        bond.ediv = enc->localediv;
        memcpy(bond.rand, enc->localrand, SL_BLE_GATT_RAND_LEN);
        memcpy(bond.ltk, enc->localltk, SL_BLE_GATT_LTK_LEN);
        if (memcmp(g_gatt.security_keys.dev_addr, enc->dev_addr, RSI_DEV_ADDR_LEN) == 0) {
            memcpy(bond.peer_irk, g_gatt.security_keys.remote_irk, SL_BLE_GATT_LTK_LEN);
        }
        bond.secure_connections = (enc->sc_enable != 0);
        g_gatt.cbs->on_bond(&bond);
    }

    LOG_DRV_INFO("BLE link encrypted: sc=%u ediv=0x%04X\r\n", enc->sc_enable, enc->localediv);
    if (g_gatt.cbs && g_gatt.cbs->on_encrypt) {
        g_gatt.cbs->on_encrypt(enc->dev_addr, true);
    }
    if (!g_gatt.peer_bonded && has_ltk && g_gatt.bond_recycle_timer != NULL) {
        // Fresh bond on this link: drop the STK ceremony link soon, see
        // SL_BLE_GATT_BOND_RECYCLE_MS
        osTimerStart(g_gatt.bond_recycle_timer, SL_BLE_GATT_BOND_RECYCLE_MS);
    }
}

static void sl_ble_gatt_handle_write(void)
{
    rsi_ble_event_write_t *event = &g_gatt.write_queue[g_gatt.wq_head];
    uint16_t handle               = (uint16_t)(event->handle[0] | ((uint16_t)event->handle[1] << 8));
    bool with_response            = (event->pkt_type == RSI_BLE_WRITE_REQUEST_EVENT);
    sl_ble_gatt_char_t *ch        = sl_ble_gatt_char_by_cccd_handle(handle);

    if (ch != NULL) {
        bool enabled = (event->length >= 1) && ((event->att_value[0] & SL_BLE_GATT_CCCD_VALUE_MASK) != 0);
        LOG_DRV_INFO("BLE subscribe: char=0x%04X cccd=0x%04X %s\r\n", ch->handle, handle, enabled ? "on" : "off");
        if (g_gatt.cbs && g_gatt.cbs->on_subscribe) {
            g_gatt.cbs->on_subscribe(event->dev_addr, ch->handle, enabled);
        }
    } else {
        LOG_DRV_INFO("BLE write: handle=0x%04X type=%u len=%u\r\n", handle, event->pkt_type, event->length);
        if (g_gatt.cbs && g_gatt.cbs->on_write) {
            g_gatt.cbs->on_write(event->dev_addr, handle, event->att_value, event->length, with_response);
        }
    }
    g_gatt.wq_head = (uint8_t)((g_gatt.wq_head + 1) % SL_BLE_GATT_WRITE_QUEUE_LEN);
}

static void sl_ble_gatt_handle_read(void)
{
    rsi_ble_read_req_t *req = &g_gatt.read_req;

    LOG_DRV_INFO("BLE read: handle=0x%04X offset=%u\r\n", req->handle, req->offset);
    if (g_gatt.cbs && g_gatt.cbs->on_read) {
        g_gatt.cbs->on_read(req->dev_addr, req->handle, req->offset);
    }
}

static void sl_ble_gatt_handle_ltk_request(void)
{
    rsi_bt_event_le_ltk_request_t *req = &g_gatt.ltk_request;
    uint8_t ltk[SL_BLE_GATT_LTK_LEN];
    int32_t status;
    bool accept = false;

    memset(ltk, 0, sizeof(ltk));
    if (g_gatt.cbs && g_gatt.cbs->on_ltk_request) {
        accept = g_gatt.cbs->on_ltk_request(req->dev_addr, req->localediv, req->localrand, ltk);
    }
    LOG_DRV_INFO("BLE ltk request: ediv=0x%04X %s\r\n", req->localediv, accept ? "accepted" : "rejected");
    status = rsi_ble_ltk_req_reply(req->dev_addr, accept ? SL_BLE_GATT_LTK_REPLY_UNAUTH : 0, ltk);
    if (status != RSI_SUCCESS) {
        LOG_DRV_ERROR("BLE ltk reply failed: %ld\r\n", (long)status);
    }
}

static void sl_ble_gatt_bond_recycle_cb(void *argument)
{
    (void)argument;
    osEventFlagsSet(g_gatt.events, SL_BLE_GATT_EVT_BOND_RECYCLE);
}

static void sl_ble_gatt_worker(void *argument)
{
    (void)argument;

    while (1) {
        uint32_t flags = osEventFlagsWait(g_gatt.events, SL_BLE_GATT_EVT_ALL, osFlagsWaitAny, osWaitForever);

        // Protocol order when several events pile up
        if (flags & SL_BLE_GATT_EVT_CONNECT) {
            sl_ble_gatt_handle_connect();
        }
        if (flags & SL_BLE_GATT_EVT_SMP_RESPONSE) {
            int32_t status = rsi_ble_smp_pair_response(g_gatt.pair_addr, SL_BLE_GATT_SMP_IO_CAP_NO_INPUT_NO_OUTPUT, 0);
            if (status != RSI_SUCCESS) {
                LOG_DRV_ERROR("BLE smp pair response failed: %ld\r\n", (long)status);
            }
        }
        if (flags & SL_BLE_GATT_EVT_LTK_REQUEST) {
            sl_ble_gatt_handle_ltk_request();
        }
        if (flags & SL_BLE_GATT_EVT_ENCRYPT) {
            sl_ble_gatt_handle_encrypt();
        }
        if (flags & SL_BLE_GATT_EVT_WRITE) {
            while (g_gatt.wq_head != g_gatt.wq_tail) {
                sl_ble_gatt_handle_write();
            }
        }
        if (flags & SL_BLE_GATT_EVT_READ) {
            sl_ble_gatt_handle_read();
        }
        if (flags & SL_BLE_GATT_EVT_BOND_RECYCLE) {
            if (g_gatt.connected) {
                LOG_DRV_INFO("BLE dropping the pairing link so the client reconnects on a fresh one\r\n");
                sl_ble_gatt_disconnect();
            }
        }
        if (flags & SL_BLE_GATT_EVT_SMP_FAILED) {
            LOG_DRV_WARN("BLE smp failed: status=0x%X\r\n", g_gatt.smp_fail_status);
            if (g_gatt.cbs && g_gatt.cbs->on_encrypt) {
                g_gatt.cbs->on_encrypt(g_gatt.peer_addr, false);
            }
        }
        if (flags & SL_BLE_GATT_EVT_DISCONNECT) {
            LOG_DRV_INFO("BLE disconnected: reason=0x%X\r\n", g_gatt.disconnect_reason);
            if (g_gatt.bond_recycle_timer != NULL) {
                osTimerStop(g_gatt.bond_recycle_timer);   // the ceremony link is already gone
            }
            g_gatt.connected = false;
            g_gatt.encrypted = false;
            g_gatt.mtu       = SL_BLE_GATT_ATT_MTU_DEFAULT;
            g_gatt.wq_head = g_gatt.wq_tail;    // drop writes parked from the lost link
            if (g_gatt.cbs && g_gatt.cbs->on_disconnect) {
                g_gatt.cbs->on_disconnect(g_gatt.disconnect_addr, g_gatt.disconnect_reason);
            }
        }
    }
}

// ==================== Public API ====================

int32_t sl_ble_gatt_init(const sl_ble_gatt_cbs_t *cbs)
{
    if (cbs == NULL) {
        return RSI_ERROR_INVALID_PARAM;
    }
    g_gatt.cbs = cbs;

    if (g_gatt.events == NULL) {
        g_gatt.events = osEventFlagsNew(NULL);
        if (g_gatt.events == NULL) {
            return RSI_ERROR_IN_OS_OPERATION;
        }
    }

    rsi_ble_gap_register_callbacks(
        NULL,                           // advertise report (central role)
        sl_ble_gatt_connect_cb,
        sl_ble_gatt_disconnect_cb,
        NULL, NULL, NULL,               // le ping timeout, phy update, data length update
        sl_ble_gatt_enhanced_connect_cb,
        NULL, NULL, NULL);

    // Read request, write event, MTU and indication confirmation are only
    // registered by the legacy monolithic function.
    rsi_ble_gatt_register_callbacks(
        NULL, NULL, NULL, NULL, NULL, NULL, NULL,   // client-side responses
        sl_ble_gatt_write_cb,                        // gatt write event
        NULL, NULL,                                  // prepare / execute write
        sl_ble_gatt_read_req_cb,
        sl_ble_gatt_mtu_cb,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        sl_ble_gatt_indicate_confirm_cb,
        NULL);

    rsi_ble_smp_register_callbacks(
        sl_ble_gatt_smp_request_cb,
        sl_ble_gatt_smp_response_cb,
        NULL,                           // smp passkey
        sl_ble_gatt_smp_failed_cb,
        sl_ble_gatt_encrypt_started_cb,
        NULL,                           // smp passkey display
        NULL,                           // sc passkey
        sl_ble_gatt_ltk_request_cb,
        sl_ble_gatt_security_keys_cb,
        NULL,                           // cli smp response
        NULL);                          // sc method

    if (g_gatt.worker == NULL) {
        g_gatt.worker = osThreadNew(sl_ble_gatt_worker, NULL, &g_gatt_worker_attr);
        if (g_gatt.worker == NULL) {
            return RSI_ERROR_IN_OS_OPERATION;
        }
    }
    if (g_gatt.bond_recycle_timer == NULL) {
        g_gatt.bond_recycle_timer = osTimerNew(sl_ble_gatt_bond_recycle_cb, osTimerOnce, NULL, NULL);
    }
    return RSI_SUCCESS;
}

static int32_t sl_ble_gatt_add_attribute(const rsi_ble_resp_add_serv_t *serv, uint16_t handle, const uuid_t *att_uuid, uint8_t property,
                                         uint16_t config_bitmap, const uint8_t *data, uint16_t data_len)
{
    rsi_ble_req_add_att_t attribute;

    memset(&attribute, 0, sizeof(attribute));
    attribute.serv_handler  = serv->serv_handler;
    attribute.handle        = handle;
    attribute.config_bitmap = config_bitmap;
    attribute.att_uuid      = *att_uuid;
    attribute.property      = property;
    attribute.data_len      = data_len;
    if (data != NULL && data_len > 0) {
        memcpy(attribute.data, data, (data_len > sizeof(attribute.data)) ? sizeof(attribute.data) : data_len);
    }
    return rsi_ble_add_attribute(&attribute);
}

static void sl_ble_gatt_uuid16(uuid_t *uuid, uint16_t val16)
{
    memset(uuid, 0, sizeof(*uuid));
    uuid->size      = 2;
    uuid->val.val16 = val16;
}

static void sl_ble_gatt_uuid128(const uint8_t uuid128_le[SL_BLE_GATT_UUID128_LEN], uuid_t *uuid)
{
    uuid128_t *value = &uuid->val.val128;

    memset(uuid, 0, sizeof(*uuid));
    uuid->size = SL_BLE_GATT_UUID128_LEN;
    // The NWP does not take the 16 wire-order bytes as they are: it emits
    // data1/data2/data3 as stored, then data4 as [1][0] [3][2] [7][6][5][4]
    // (read off the air with NWP firmware 2.17.6). Feed it so that the UUID
    // goes out in BLE little-endian order, which is what uuid128_le holds.
    memcpy(value, uuid128_le, 8);
    value->data4[0] = uuid128_le[9];
    value->data4[1] = uuid128_le[8];
    value->data4[2] = uuid128_le[11];
    value->data4[3] = uuid128_le[10];
    value->data4[4] = uuid128_le[15];
    value->data4[5] = uuid128_le[14];
    value->data4[6] = uuid128_le[13];
    value->data4[7] = uuid128_le[12];
}

/// @brief Characteristic declaration (UUID 0x2803). WiseConnect keeps one
///        handle per attribute and leaves the declaration to the application.
///        Its value is [properties][pad][value handle LE][characteristic UUID]
///        in the NWP's uuid_t field layout (20 bytes for a 128-bit UUID): the
///        UUID goes through the same conversion as the service UUID, sent as
///        raw wire-order bytes the NWP scrambles its last 8 bytes (read off the
///        air with NWP firmware 2.17.6). A 3-byte header makes the client's
///        discovery fail on the whole service.
static int32_t sl_ble_gatt_add_declaration(uint16_t handle, uint8_t property, uint16_t value_handle,
                                           const uint8_t uuid128_le[SL_BLE_GATT_UUID128_LEN])
{
    uint8_t data[4 + SL_BLE_GATT_UUID128_LEN];
    uuid_t uuid;

    data[0] = property;
    data[1] = 0;
    data[2] = (uint8_t)(value_handle & 0xFF);
    data[3] = (uint8_t)(value_handle >> 8);
    sl_ble_gatt_uuid128(uuid128_le, &uuid);
    memcpy(&data[4], &uuid.val.val128, SL_BLE_GATT_UUID128_LEN);
    sl_ble_gatt_uuid16(&uuid, RSI_BLE_CHAR_SERV_UUID);
    // Kept by the firmware: it is what the client reads during discovery
    return sl_ble_gatt_add_attribute(&g_gatt.serv, handle, &uuid, SL_BLE_GATT_PROP_READ, 0, data, (uint16_t)sizeof(data));
}

int32_t sl_ble_gatt_profile_create(const sl_ble_gatt_profile_t *profile)
{
    int32_t status;
    uuid_t uuid;
    uint16_t handle;

    if (profile == NULL || profile->uuid128 == NULL || profile->chars == NULL || profile->char_count == 0) {
        return RSI_ERROR_INVALID_PARAM;
    }

    g_gatt.profile_valid = false;
    for (uint8_t i = 0; i < profile->char_count; i++) {
        profile->chars[i].handle      = 0;
        profile->chars[i].cccd_handle = 0;
    }

    // Just Works with bonding: the peer keeps our LTK, we keep it too (through
    // on_bond) so a reconnect can be encrypted again.
    rsi_ble_set_smp_pairing_capabilty_data_t smp_caps;
    memset(&smp_caps, 0, sizeof(smp_caps));
    smp_caps.io_capability         = SL_BLE_GATT_SMP_IO_CAP_NO_INPUT_NO_OUTPUT;
    smp_caps.auth_req              = SL_BLE_GATT_SMP_AUTHREQ_BONDING;
    smp_caps.enc_key_size          = SL_BLE_GATT_SMP_KEY_SIZE;
    smp_caps.ini_key_distribution  = SL_BLE_GATT_SMP_KEY_DIST_ENC_ID;
    smp_caps.rsp_key_distribution = SL_BLE_GATT_SMP_KEY_DIST_ENC_ID;
    status = rsi_ble_set_smp_pairing_cap_data(&smp_caps);
    if (status != RSI_SUCCESS) {
        LOG_DRV_ERROR("BLE set smp capabilities failed: %ld\r\n", (long)status);
        return status;
    }

    memset(&g_gatt.serv, 0, sizeof(g_gatt.serv));
    sl_ble_gatt_uuid128(profile->uuid128, &uuid);
    status = rsi_ble_add_service(uuid, &g_gatt.serv);
    if (status != RSI_SUCCESS) {
        LOG_DRV_ERROR("BLE add service failed: %ld\r\n", (long)status);
        return status;
    }

    // One handle per attribute, in this order: declaration, value, and for a
    // notifying/indicating characteristic its CCCD. The handles are the real
    // ATT handles: they come back in the write/read events.
    handle = (uint16_t)(g_gatt.serv.start_handle + 1);
    for (uint8_t i = 0; i < profile->char_count; i++) {
        sl_ble_gatt_char_t *ch = &profile->chars[i];

        status = sl_ble_gatt_add_declaration(handle, ch->property, (uint16_t)(handle + 1), ch->uuid128);
        if (status != RSI_SUCCESS) {
            LOG_DRV_ERROR("BLE add declaration %u failed: %ld\r\n", i, (long)status);
            return status;
        }
        handle++;

        sl_ble_gatt_uuid128(ch->uuid128, &uuid);
        status = sl_ble_gatt_add_attribute(&g_gatt.serv, handle, &uuid, ch->property,
                                           ch->encrypted ? SL_BLE_GATT_SEC_ENCRYPTED : SL_BLE_GATT_SEC_OPEN, NULL, 0);
        if (status != RSI_SUCCESS) {
            LOG_DRV_ERROR("BLE add characteristic %u failed: %ld\r\n", i, (long)status);
            return status;
        }
        ch->handle = handle;
        handle++;

        if (!(ch->property & (SL_BLE_GATT_PROP_NOTIFY | SL_BLE_GATT_PROP_INDICATE))) {
            continue;
        }
        // The CCCD carries no secret and clients subscribe right after the
        // META read, while the link is not necessarily encrypted yet: with
        // config 0 the NWP would inherit the characteristic's security
        // requirement and reject the write itself (Insufficient
        // Authentication), so it needs its own open declaration. The firmware
        // keeps the 2-byte value and still reports the write.
        sl_ble_gatt_uuid16(&uuid, RSI_BLE_CLIENT_CHAR_UUID);
        status = sl_ble_gatt_add_attribute(&g_gatt.serv, handle, &uuid, SL_BLE_GATT_PROP_READ | SL_BLE_GATT_PROP_WRITE,
                                           SEC_MODE_1_LEVEL_1 | ON_LE_LINK_ONLY, NULL, 2);
        if (status != RSI_SUCCESS) {
            LOG_DRV_ERROR("BLE add cccd %u failed: %ld\r\n", i, (long)status);
            return status;
        }
        ch->cccd_handle = handle;
        handle++;
    }

    g_gatt.profile       = *profile;
    g_gatt.profile_valid = true;
    LOG_DRV_INFO("BLE profile created: start_handle=0x%04X chars=%u\r\n", g_gatt.serv.start_handle, profile->char_count);
    for (uint8_t i = 0; i < profile->char_count; i++) {
        LOG_DRV_INFO("  char %u: handle=0x%04X cccd=0x%04X prop=0x%02X %s\r\n", i, profile->chars[i].handle,
                     profile->chars[i].cccd_handle, profile->chars[i].property,
                     profile->chars[i].encrypted ? "encrypted" : "open");
    }
    return RSI_SUCCESS;
}

void sl_ble_gatt_profile_invalidate(void)
{
    g_gatt.profile_valid = false;
    g_gatt.connected     = false;
    g_gatt.encrypted     = false;
    g_gatt.advertising   = false;
}

bool sl_ble_gatt_profile_is_valid(void)
{
    return g_gatt.profile_valid;
}

int32_t sl_ble_gatt_adv_data_set(const uint8_t *adv, uint8_t adv_len,
                                 const uint8_t *scan_rsp, uint8_t scan_rsp_len)
{
    int32_t status;

    if (adv == NULL || adv_len == 0 || adv_len > SL_BLE_GATT_ADV_DATA_MAX ||
        scan_rsp_len > SL_BLE_GATT_ADV_DATA_MAX || (scan_rsp_len > 0 && scan_rsp == NULL)) {
        return RSI_ERROR_INVALID_PARAM;
    }

    status = rsi_ble_set_advertise_data(adv, adv_len);
    if (status != RSI_SUCCESS) {
        LOG_DRV_ERROR("BLE set advertise data failed: %ld\r\n", (long)status);
        return status;
    }
    if (scan_rsp_len > 0) {
        status = rsi_ble_set_scan_response_data(scan_rsp, scan_rsp_len);
        if (status != RSI_SUCCESS) {
            LOG_DRV_ERROR("BLE set scan response data failed: %ld\r\n", (long)status);
            return status;
        }
    }
    return RSI_SUCCESS;
}

int32_t sl_ble_gatt_adv_start(void)
{
    rsi_ble_req_adv_t adv;
    int32_t status;

    if (!g_gatt.profile_valid) {
        return RSI_ERROR_COMMAND_GIVEN_IN_WRONG_STATE;
    }
    if (g_gatt.advertising) {
        return RSI_SUCCESS;
    }

    memset(&adv, 0, sizeof(adv));
    adv.status           = RSI_BLE_START_ADV;
    adv.adv_type         = UNDIR_CONN;
    adv.filter_type      = ALLOW_SCAN_REQ_ANY_CONN_REQ_ANY;
    adv.own_addr_type    = LE_PUBLIC_ADDRESS;
    adv.adv_int_min      = SL_BLE_GATT_ADV_INT_MIN;
    adv.adv_int_max      = SL_BLE_GATT_ADV_INT_MAX;
    adv.adv_channel_map  = SL_BLE_GATT_ADV_CHANNEL_MAP;

    status = rsi_ble_start_advertising_with_values(&adv);
    if (status != RSI_SUCCESS) {
        LOG_DRV_ERROR("BLE start advertising failed: %ld\r\n", (long)status);
        return status;
    }
    g_gatt.advertising = true;
    LOG_DRV_INFO("BLE advertising started\r\n");
    return RSI_SUCCESS;
}

int32_t sl_ble_gatt_adv_stop(void)
{
    int32_t status;

    if (!g_gatt.advertising) {
        return RSI_SUCCESS;
    }
    status = rsi_ble_stop_advertising();
    if (status != RSI_SUCCESS) {
        LOG_DRV_ERROR("BLE stop advertising failed: %ld\r\n", (long)status);
        return status;
    }
    g_gatt.advertising = false;
    LOG_DRV_INFO("BLE advertising stopped\r\n");
    return RSI_SUCCESS;
}

bool sl_ble_gatt_is_advertising(void)
{
    return g_gatt.advertising;
}

bool sl_ble_gatt_is_connected(void)
{
    return g_gatt.connected;
}

bool sl_ble_gatt_is_encrypted(void)
{
    return g_gatt.encrypted;
}

void sl_ble_gatt_peer_addr(uint8_t dev_addr[RSI_DEV_ADDR_LEN])
{
    memcpy(dev_addr, g_gatt.peer_addr, RSI_DEV_ADDR_LEN);
}

uint16_t sl_ble_gatt_mtu(void)
{
    return g_gatt.mtu;
}

int32_t sl_ble_gatt_indicate(uint16_t handle, const uint8_t *data, uint16_t len)
{
    if (!g_gatt.connected) {
        return RSI_ERROR_NOT_IN_CONNECTED_STATE;
    }
    if (data == NULL || len == 0 || len > (uint16_t)(g_gatt.mtu - 3)) {
        return RSI_ERROR_INVALID_PARAM;
    }
    // The sync variant blocks until the client confirmed the indication and
    // answers with a plain sync response. The async rsi_ble_indicate_value
    // instead leaves the SDK's per-peer cmd_in_use flag set until the
    // GATT_INDICATE_CONFIRMATION event arrives - which this NWP firmware
    // (2.17.6) never sends, so every later indication fails with
    // RSI_ERROR_BLE_ATT_CMD_IN_PROGRESS (-39) forever.
    return rsi_ble_indicate_value_sync(g_gatt.peer_addr, handle, len, data);
}

int32_t sl_ble_gatt_read_response(const uint8_t dev_addr[RSI_DEV_ADDR_LEN], uint16_t handle, uint16_t offset,
                                  const uint8_t *data, uint16_t len)
{
    return rsi_ble_gatt_read_response((uint8_t *)dev_addr, 0, handle, offset, len, data);
}

int32_t sl_ble_gatt_read_error(const uint8_t dev_addr[RSI_DEV_ADDR_LEN], uint16_t handle, uint8_t err)
{
    return rsi_ble_att_error_response((uint8_t *)dev_addr, handle, RSI_BLE_ATT_READ_REQUEST, err);
}

int32_t sl_ble_gatt_write_response(const uint8_t dev_addr[RSI_DEV_ADDR_LEN])
{
    return rsi_ble_gatt_write_response((uint8_t *)dev_addr, 0);
}

int32_t sl_ble_gatt_disconnect(void)
{
    int32_t status;

    if (!g_gatt.connected) {
        return RSI_SUCCESS;
    }
    status = rsi_ble_disconnect((const int8_t *)g_gatt.peer_addr);
    if (status != RSI_SUCCESS) {
        LOG_DRV_ERROR("BLE disconnect failed: %ld\r\n", (long)status);
    }
    return status;
}
#endif
