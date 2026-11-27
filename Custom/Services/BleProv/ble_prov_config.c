#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include "debug.h"
#include "ble_prov_config.h"
#include "communication_service.h"
#include "netif_manager.h"
#include "mm_halow_netif.h"
#include "mqtt_service.h"
#include "upload_coordinator.h"
#include "json_config_mgr.h"
#include "buffer_mgr.h"
#include "version.h"
#include "sl_rsi_ble_ctprov.h"
#include "common_utils.h"

#define LOG_TAG "[BLE_PROV] "

// Sections of the last accepted config.set; the same set is applied afterwards
#define SEC_WIFI      (1u << 0)
#define SEC_CELLULAR  (1u << 1)
#define SEC_HALOW     (1u << 2)
#define SEC_COMM      (1u << 3)
#define SEC_MQTT      (1u << 4)
#define SEC_WEBHOOK   (1u << 5)
#define SEC_ETHERNET  (1u << 6)

#define BLE_PROV_SCAN_PAGE_SIZE       24
#define BLE_PROV_SCAN_SNAPSHOT_MAX    MAX_SCAN_RESULTS
#define BLE_PROV_ANY_BSSID_TEXT       "00:00:00:00:00:00"
#define BLE_PROV_MQTT_CLIENT_ID_AUTO  "AICAM-000000"        // sentinel: derived from the device MAC

// Ethernet/PoE IP mode: the W5500 netif keeps its configuration in RAM with
// compiled-in defaults, so the provisioning owns the persistence (NVS) and
// re-applies it on boot and on config.set.
#define BLE_PROV_ETH_NVS_KEY          "bleprov_eth"
#define BLE_PROV_ETH_MAGIC            0x45544831             // "ETH1"
typedef struct {
    uint32_t magic;
    uint8_t static_ip;                // 0: DHCP (the W5500 default), 1: static
    uint8_t ip[4];
    uint8_t gw[4];
    uint8_t mask[4];
} ble_prov_eth_store_t;

typedef struct {
    uint32_t sections;
    char wifi_ssid[NETIF_SSID_VALUE_SIZE];
    char wifi_password[NETIF_PW_VALUE_SIZE];
    char wifi_bssid[18];
    cellular_connection_settings_t cellular;
    char halow_ssid[32];
    char halow_password[64];
    char halow_country_code[NETIF_HALOW_COUNTRY_CODE_LEN];
    communication_type_t comm_type;
    struct {
        bool static_ip;
        char ip[16];
        char gateway[16];
        char netmask[16];
    } ethernet;
    struct {
        bool enable;
        char host[128];
        uint16_t port;
        char client_id[64];
        char username[64];
        char password[128];
        bool tls;
        char topic[MAX_TOPIC_LENGTH];
        uint8_t qos;
    } mqtt;
    struct {
        bool enable;
        char url[WEBHOOK_URL_MAX_LEN];
        char secret[WEBHOOK_SECRET_MAX_LEN];
        bool has_secret;
    } webhook;
} ble_prov_plan_t;

typedef struct {
    char ssid[32];
    int32_t rssi;
    uint32_t channel;
} ble_prov_scan_item_t;

static ble_prov_plan_t g_plan IN_PSRAM;
static ble_prov_scan_item_t g_scan_items[BLE_PROV_SCAN_SNAPSHOT_MAX] IN_PSRAM;
static uint32_t g_scan_count;
static volatile bool g_applying;

// ==================== helpers ====================

static void scan_snapshot_build(const network_scan_result_t *results, uint32_t count);

static ct_prov_err_t fail(ct_prov_result_t *result, ct_prov_err_t code, const char *fmt, ...)
{
    va_list args;

    result->code = code;
    va_start(args, fmt);
    vsnprintf(result->message, sizeof(result->message), fmt, args);
    va_end(args);
    return code;
}

/// @brief Reject fields the device does not implement
static ct_prov_err_t check_fields(const cJSON *section, const char *name, const char *const *allowed, ct_prov_result_t *result)
{
    const cJSON *field = NULL;

    cJSON_ArrayForEach(field, section) {
        size_t i;
        for (i = 0; allowed[i] != NULL && strcmp(allowed[i], field->string) != 0; i++) {
            // walk the allowed list
        }
        if (allowed[i] == NULL) {
            return fail(result, CTPROV_ERR_FIELD_NOT_SUPPORTED, "%s.%s is not supported", name, field->string);
        }
    }
    return CTPROV_OK;
}

static ct_prov_err_t get_string(const cJSON *section, const char *field, const char *name,
                                char *out, size_t out_cap, bool required, size_t max_len, ct_prov_result_t *result)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(section, field);
    size_t len;

    out[0] = '\0';
    if (item == NULL || cJSON_IsNull(item)) {
        if (required) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s is required", name, field);
        }
        return CTPROV_OK;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s must be a string", name, field);
    }
    len = strlen(item->valuestring);
    if (len >= out_cap || len > max_len) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s is too long (max %u bytes)", name, field, (unsigned)max_len);
    }
    if (required && len == 0) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s must not be empty", name, field);
    }
    strcpy(out, item->valuestring);
    return CTPROV_OK;
}

/// @brief Parse "a.b.c.d" into 4 bytes; NULL/empty text is accepted as zeros
static bool ipv4_text_to_bytes(const char *text, uint8_t out[4])
{
    unsigned int value[4];
    char tail;

    memset(out, 0, 4);
    if (text == NULL || text[0] == '\0') {
        return true;
    }
    // the trailing %c makes trailing garbage a parse count of 5, not 4
    if (sscanf(text, "%u.%u.%u.%u%c", &value[0], &value[1], &value[2], &value[3], &tail) != 4) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (value[i] > 255) {
            return false;
        }
        out[i] = (uint8_t)value[i];
    }
    return true;
}

static bool is_ipv4_text(const char *text)
{
    uint8_t bytes[4];

    if (text == NULL || text[0] == '\0') {
        return false;
    }
    return ipv4_text_to_bytes(text, bytes);
}

static ct_prov_err_t get_bool(const cJSON *section, const char *field, const char *name, bool required, bool *out,
                              ct_prov_result_t *result)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(section, field);

    if (item == NULL || cJSON_IsNull(item)) {
        if (required) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s is required", name, field);
        }
        return CTPROV_OK;
    }
    if (!cJSON_IsBool(item)) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s must be a boolean", name, field);
    }
    *out = cJSON_IsTrue(item);
    return CTPROV_OK;
}

/// Integers must not be expressed as floats (section 6)
static ct_prov_err_t get_int(const cJSON *section, const char *field, const char *name, bool required, long min, long max,
                             long *out, ct_prov_result_t *result)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(section, field);
    double number;

    if (item == NULL || cJSON_IsNull(item)) {
        if (required) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s is required", name, field);
        }
        return CTPROV_OK;
    }
    if (!cJSON_IsNumber(item)) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s must be an integer", name, field);
    }
    number = cJSON_GetNumberValue(item);
    if (number != (double)(long)number) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s must not be a float", name, field);
    }
    if (number < (double)min || number > (double)max) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "%s.%s must be %ld..%ld", name, field, min, max);
    }
    *out = (long)number;
    return CTPROV_OK;
}

static bool is_bssid_text(const char *text)
{
    for (int group = 0; group < 6; group++) {
        if (!isxdigit((unsigned char)text[group * 3]) || !isxdigit((unsigned char)text[group * 3 + 1])) {
            return false;
        }
        if (group < 5 && text[group * 3 + 2] != ':') {
            return false;
        }
    }
    return text[17] == '\0';
}

static void uppercase(char *text)
{
    for (; *text != '\0'; text++) {
        *text = (char)toupper((unsigned char)*text);
    }
}

// ==================== device identity ====================

void ble_prov_config_device_name(char *out, size_t out_cap)
{
    device_info_config_t *info = (device_info_config_t *)buffer_calloc(1, sizeof(*info));
    const char *name = "";
    const char *mac = "";

    out[0] = '\0';
    if (info != NULL) {
        if (json_config_get_device_info_config(info) == AICAM_OK) {
            name = info->device_name;
            mac  = info->mac_address;
        }
        buffer_free(info);
    }
    if (sl_ble_ctprov_name_is_valid(name)) {
        strncpy(out, name, out_cap - 1);
        out[out_cap - 1] = '\0';
        return;
    }
    // A renamed device keeps its name elsewhere; CT-Prov needs "<MODEL>-<MAC suffix>"
    if (strlen(mac) == 17) {
        snprintf(out, out_cap, "%s-%c%c%c%c%c%c", BLE_PROV_MODEL_NAME, toupper((unsigned char)mac[9]),
                 toupper((unsigned char)mac[10]), toupper((unsigned char)mac[12]), toupper((unsigned char)mac[13]),
                 toupper((unsigned char)mac[15]), toupper((unsigned char)mac[16]));
    } else {
        snprintf(out, out_cap, "%s-000000", BLE_PROV_MODEL_NAME);
    }
}

void ble_prov_config_device_info(cJSON *data, ct_prov_result_t *result)
{
    device_info_config_t *info = (device_info_config_t *)buffer_calloc(1, sizeof(*info));
    char name[24];
    const char *serial = "";
    cJSON *power;
    bool battery = false;
    bool poe = false;

    ble_prov_config_device_name(name, sizeof(name));
    if (info != NULL) {
        if (json_config_get_device_info_config(info) == AICAM_OK) {
            serial  = info->serial_number;
            battery = (strcmp(info->power_supply_type, "battery") == 0);
        }
    }
    cJSON_AddStringToObject(data, "device_name", name);
    cJSON_AddStringToObject(data, "serial_number", serial);
    cJSON_AddStringToObject(data, "model", BLE_PROV_MODEL_NAME);
    cJSON_AddStringToObject(data, "firmware", FW_VERSION_STRING);
    buffer_free(info);

    poe   = communication_is_type_connected(COMM_TYPE_POE);
    power = cJSON_AddArrayToObject(data, "power");
    if (battery) {
        cJSON_AddItemToArray(power, cJSON_CreateString("battery"));
    } else if (poe) {
        cJSON_AddItemToArray(power, cJSON_CreateString("poe"));
    } else {
        cJSON_AddItemToArray(power, cJSON_CreateString("external"));
    }
    cJSON_AddNumberToObject(data, "protocol_version", BLE_PROV_PROTOCOL_VERSION);
    (void)result;
}

// ==================== capabilities ====================

static cJSON *add_section(cJSON *sections, const char *name, const char *kind, bool configurable, bool scannable)
{
    cJSON *section = cJSON_CreateObject();

    cJSON_AddStringToObject(section, "name", name);
    cJSON_AddStringToObject(section, "kind", kind);
    cJSON_AddBoolToObject(section, "configurable", configurable);
    if (scannable) {
        cJSON_AddBoolToObject(section, "scannable", true);
    }
    cJSON_AddItemToArray(sections, section);
    return cJSON_AddArrayToObject(section, "fields");
}

static void add_fields(cJSON *fields, ...)
{
    va_list args;
    const char *field;

    va_start(args, fields);
    while ((field = va_arg(args, const char *)) != NULL) {
        cJSON_AddItemToArray(fields, cJSON_CreateString(field));
    }
    va_end(args);
}

void ble_prov_config_capabilities(cJSON *data, ct_prov_result_t *result)
{
    cJSON *sections = cJSON_AddArrayToObject(data, "sections");
    cJSON *current = cJSON_AddObjectToObject(data, "current");
    const char *network = NULL;
    communication_type_t type = communication_get_current_type();

    add_fields(add_section(sections, "wifi", "network", true, true), "ssid", "password", "bssid", NULL);
#if NETIF_4G_CAT1_IS_ENABLE
    add_fields(add_section(sections, "cellular", "network", true, false), "apn", "username", "password", "pin", "auth",
               "roaming", NULL);
#endif
#if NETIF_WIFI_HALOW_IS_ENABLE
    add_fields(add_section(sections, "halow", "network", true, false), "ssid", "password", "country", NULL);
#endif
#if NETIF_ETH_WAN_IS_ENABLE
    add_fields(add_section(sections, "ethernet", "network", true, false), "ip_mode", "ip", "gateway", "netmask", NULL);
#endif
    add_fields(add_section(sections, "comm", "selector", true, false), "type", NULL);
    add_fields(add_section(sections, "mqtt", "service", true, false), "enable", "host", "port", "client_id", "username",
               "password", "tls", "topic", "qos", NULL);
    add_fields(add_section(sections, "webhook", "service", true, false), "enable", "url", "secret", NULL);

    switch (type) {
        case COMM_TYPE_WIFI:     network = "wifi"; break;
        case COMM_TYPE_CELLULAR: network = "cellular"; break;
        case COMM_TYPE_HALOW:    network = "halow"; break;
        case COMM_TYPE_POE:      network = "ethernet"; break;
        default:                 break;
    }
    if (network != NULL) {
        cJSON_AddStringToObject(current, "network", network);
    } else {
        cJSON_AddNullToObject(current, "network");
    }
    (void)result;
}

// ==================== scan ====================

void ble_prov_config_scan_reset(void)
{
    g_scan_count = 0;
}

/// @brief Join Wi-Fi, retrying with the device's own AP out of the way.
///        Rejoining while the AP is up can pin the radio to the AP's
///        channel: the join scan then misses the router (0x10019, seen when
///        the STA was up on the same network). With the AP down the join
///        scans freely; the AP comes back on the STA's channel afterwards.
static aicam_result_t wifi_join(const char *ssid, const char *bssid)
{
    aicam_result_t ret = communication_apply_wifi_credentials(ssid, bssid);

    if (ret != AICAM_OK) {
        LOG_SVC_WARN(LOG_TAG "wifi join failed (%d), retrying without the AP\r\n", (int)ret);
        communication_stop_interface(NETIF_NAME_WIFI_AP);
        ret = communication_apply_wifi_credentials(ssid, bssid);
        communication_start_interface(NETIF_NAME_WIFI_AP);
    }
    return ret;
}

/// @brief Bring the STA back with the stored credentials. The scan's
///        restore uses the plain netif up, which skips the AP/STA channel
///        alignment and can leave the client link down; the join path used
///        by config.set's apply handles the channels and is proven.
static void scan_rejoin_sta(void)
{
    netif_config_t config;

    if (communication_is_interface_connected(NETIF_NAME_WIFI_STA)) {
        return;
    }
    if (communication_get_interface_config(NETIF_NAME_WIFI_STA, &config) != AICAM_OK ||
        config.wireless_cfg.ssid[0] == '\0') {
        return;   // nothing configured to rejoin
    }
    LOG_SVC_INFO(LOG_TAG "STA link down after the scan, rejoining %s\r\n", config.wireless_cfg.ssid);
    if (wifi_join(config.wireless_cfg.ssid, NULL) != AICAM_OK) {
        LOG_SVC_WARN(LOG_TAG "STA rejoin after scan failed\r\n");
    }
}

void ble_prov_config_set_applying(bool applying)
{
    g_applying = applying;
}

/// @brief One entry per SSID (strongest AP), strongest first
static void scan_snapshot_build(const network_scan_result_t *results, uint32_t count)
{
    g_scan_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        ble_prov_scan_item_t *slot = NULL;

        if (results[i].ssid[0] == '\0') {
            continue;   // hidden network: nothing to select
        }
        for (uint32_t j = 0; j < g_scan_count; j++) {
            if (strcmp(g_scan_items[j].ssid, results[i].ssid) == 0) {
                slot = &g_scan_items[j];
                break;
            }
        }
        if (slot != NULL) {
            if (slot->rssi < results[i].rssi) {
                slot->rssi    = results[i].rssi;
                slot->channel = results[i].channel;
            }
            continue;
        }
        if (g_scan_count >= BLE_PROV_SCAN_SNAPSHOT_MAX) {
            continue;
        }
        slot = &g_scan_items[g_scan_count++];
        memset(slot, 0, sizeof(*slot));
        strncpy(slot->ssid, results[i].ssid, sizeof(slot->ssid) - 1);
        slot->rssi    = results[i].rssi;
        slot->channel = results[i].channel;
    }
    for (uint32_t i = 1; i < g_scan_count; i++) {          // insertion sort, strongest first
        ble_prov_scan_item_t item = g_scan_items[i];
        uint32_t j = i;
        while (j > 0 && g_scan_items[j - 1].rssi < item.rssi) {
            g_scan_items[j] = g_scan_items[j - 1];
            j--;
        }
        g_scan_items[j] = item;
    }
}

void ble_prov_config_scan(const char *section, const char *cursor, cJSON *data, ct_prov_result_t *result)
{
    network_scan_result_t *results;
    uint32_t count = 0;
    uint32_t start = 0;
    uint32_t end;
    cJSON *items;

    if (strcmp(section, "wifi") != 0) {
        fail(result, CTPROV_ERR_INVALID_ARGUMENT, "section '%s' is not scannable", section);
        return;
    }
    if (g_applying) {
        fail(result, CTPROV_ERR_BUSY, "network configuration is in progress");
        return;
    }

    if (cursor != NULL && cursor[0] != '\0') {
        char *tail;
        unsigned long value;

        value = strtoul(cursor, &tail, 10);
        if (*tail != '\0' || value >= g_scan_count) {
            fail(result, CTPROV_ERR_INVALID_ARGUMENT, "invalid cursor");
            return;
        }
        start = (uint32_t)value;
    } else {
        aicam_result_t ret;

        g_scan_count = 0;
        results = (network_scan_result_t *)buffer_calloc(MAX_SCAN_RESULTS, sizeof(*results));
        if (results == NULL) {
            fail(result, CTPROV_ERR_INTERNAL_ERROR, "out of memory");
            return;
        }
        // Synchronous: pauses the AP and STA links for the ~10 s channel
        // sweep, then restores them (the client's scan timeout is 20 s).
        ret = communication_start_network_scan(NULL);
        if (ret == AICAM_OK) {
            communication_get_scan_results(results, MAX_SCAN_RESULTS, &count);
            scan_snapshot_build(results, count);
        }
        buffer_free(results);
        if (ret != AICAM_OK) {
            fail(result, CTPROV_ERR_INTERNAL_ERROR, "network scan failed (%d)", (int)ret);
            return;
        }
        // The sweep's restore can leave the STA link down (channel
        // alignment): bring it back before the client configures Wi-Fi
        scan_rejoin_sta();
    }

    end = start + BLE_PROV_SCAN_PAGE_SIZE;
    if (end > g_scan_count) {
        end = g_scan_count;
    }
    cJSON_AddStringToObject(data, "section", "wifi");
    items = cJSON_AddArrayToObject(data, "items");
    for (uint32_t i = start; i < end; i++) {
        cJSON *item = cJSON_CreateObject();

        cJSON_AddStringToObject(item, "network_id", g_scan_items[i].ssid);
        cJSON_AddStringToObject(item, "display_name", g_scan_items[i].ssid);
        cJSON_AddNumberToObject(item, "signal", g_scan_items[i].rssi);
        cJSON_AddNumberToObject(item, "channel", g_scan_items[i].channel);
        cJSON_AddItemToArray(items, item);
    }
    if (end < g_scan_count) {
        char next[12];

        snprintf(next, sizeof(next), "%lu", (unsigned long)end);
        cJSON_AddBoolToObject(data, "more", true);
        cJSON_AddStringToObject(data, "cursor", next);
    } else {
        cJSON_AddBoolToObject(data, "more", false);
        cJSON_AddStringToObject(data, "cursor", "");
    }
}

// ==================== config.set: validation ====================

static ct_prov_err_t validate_wifi(const cJSON *section, ct_prov_result_t *result)
{
    static const char *const allowed[] = { "ssid", "password", "bssid", NULL };
    ct_prov_err_t err;

    err = check_fields(section, "wifi", allowed, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "ssid", "wifi", g_plan.wifi_ssid, sizeof(g_plan.wifi_ssid), true, 31, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "password", "wifi", g_plan.wifi_password, sizeof(g_plan.wifi_password), false, 63, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "bssid", "wifi", g_plan.wifi_bssid, sizeof(g_plan.wifi_bssid), false, 17, result);
    if (err != CTPROV_OK) return err;

    if (g_plan.wifi_password[0] != '\0' && strlen(g_plan.wifi_password) < 8) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "wifi.password must be 8..63 bytes");
    }
    if (g_plan.wifi_bssid[0] != '\0') {
        uppercase(g_plan.wifi_bssid);
        if (!is_bssid_text(g_plan.wifi_bssid)) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "wifi.bssid must be XX:XX:XX:XX:XX:XX");
        }
    }
    g_plan.sections |= SEC_WIFI;
    return CTPROV_OK;
}

static ct_prov_err_t validate_cellular(const cJSON *section, ct_prov_result_t *result)
{
    static const char *const allowed[] = { "apn", "username", "password", "pin", "auth", "roaming", NULL };
    cellular_connection_settings_t *settings = &g_plan.cellular;
    char auth[16];
    bool roaming = false;
    ct_prov_err_t err;

    memset(settings, 0, sizeof(*settings));
    settings->authentication = CELLULAR_AUTH_NONE;
    settings->enable_roaming = AICAM_FALSE;

    err = check_fields(section, "cellular", allowed, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "apn", "cellular", settings->apn, sizeof(settings->apn), false, sizeof(settings->apn) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "username", "cellular", settings->username, sizeof(settings->username), false,
                     sizeof(settings->username) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "password", "cellular", settings->password, sizeof(settings->password), false,
                     sizeof(settings->password) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "pin", "cellular", settings->pin_code, sizeof(settings->pin_code), false, 8, result);
    if (err != CTPROV_OK) return err;
    auth[0] = '\0';
    err = get_string(section, "auth", "cellular", auth, sizeof(auth), false, sizeof(auth) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_bool(section, "roaming", "cellular", false, &roaming, result);
    if (err != CTPROV_OK) return err;
    settings->enable_roaming = roaming ? AICAM_TRUE : AICAM_FALSE;

    for (const char *p = settings->pin_code; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "cellular.pin must be 4..8 digits");
        }
    }
    if (settings->pin_code[0] != '\0' && strlen(settings->pin_code) < 4) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "cellular.pin must be 4..8 digits");
    }
    if (strcmp(auth, "") == 0 || strcmp(auth, "none") == 0) {
        settings->authentication = CELLULAR_AUTH_NONE;
    } else if (strcmp(auth, "pap") == 0) {
        settings->authentication = CELLULAR_AUTH_PAP;
    } else if (strcmp(auth, "chap") == 0) {
        settings->authentication = CELLULAR_AUTH_CHAP;
    } else if (strcmp(auth, "pap_or_chap") == 0) {
        settings->authentication = CELLULAR_AUTH_PAP_CHAP;
    } else {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "cellular.auth must be none, pap, chap or pap_or_chap");
    }
    g_plan.sections |= SEC_CELLULAR;
    return CTPROV_OK;
}

static ct_prov_err_t validate_halow(const cJSON *section, ct_prov_result_t *result)
{
    static const char *const allowed[] = { "ssid", "password", "country", NULL };
    ct_prov_err_t err;

    err = check_fields(section, "halow", allowed, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "ssid", "halow", g_plan.halow_ssid, sizeof(g_plan.halow_ssid), true, 31, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "password", "halow", g_plan.halow_password, sizeof(g_plan.halow_password), false, 63, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "country", "halow", g_plan.halow_country_code, sizeof(g_plan.halow_country_code), false, 2,
                     result);
    if (err != CTPROV_OK) return err;

    if (g_plan.halow_password[0] != '\0' && strlen(g_plan.halow_password) < 8) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "halow.password must be 8..63 bytes");
    }
    if (g_plan.halow_country_code[0] != '\0') {
        uppercase(g_plan.halow_country_code);
        if (!isupper((unsigned char)g_plan.halow_country_code[0]) || !isupper((unsigned char)g_plan.halow_country_code[1]) ||
            !mm_halow_regdomain_is_supported(g_plan.halow_country_code)) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "halow.country is not a supported region");
        }
    }
    g_plan.sections |= SEC_HALOW;
    return CTPROV_OK;
}

static ct_prov_err_t validate_ethernet(const cJSON *section, ct_prov_result_t *result)
{
    static const char *const allowed[] = { "ip_mode", "ip", "gateway", "netmask", NULL };
    char mode[8];
    ct_prov_err_t err;

    err = check_fields(section, "ethernet", allowed, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "ip_mode", "ethernet", mode, sizeof(mode), true, sizeof(mode) - 1, result);
    if (err != CTPROV_OK) return err;

    g_plan.ethernet.static_ip = false;
    g_plan.ethernet.ip[0]     = '\0';
    g_plan.ethernet.gateway[0] = '\0';
    g_plan.ethernet.netmask[0] = '\0';
    if (strcmp(mode, "dhcp") == 0) {
        // nothing else allowed to be set: an address next to DHCP is a client bug
        if (cJSON_GetObjectItemCaseSensitive(section, "ip") != NULL ||
            cJSON_GetObjectItemCaseSensitive(section, "gateway") != NULL ||
            cJSON_GetObjectItemCaseSensitive(section, "netmask") != NULL) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "ethernet addresses are only valid with ip_mode=static");
        }
    } else if (strcmp(mode, "static") == 0) {
        g_plan.ethernet.static_ip = true;
        err = get_string(section, "ip", "ethernet", g_plan.ethernet.ip, sizeof(g_plan.ethernet.ip), true, 15, result);
        if (err != CTPROV_OK) return err;
        err = get_string(section, "gateway", "ethernet", g_plan.ethernet.gateway, sizeof(g_plan.ethernet.gateway), true, 15, result);
        if (err != CTPROV_OK) return err;
        err = get_string(section, "netmask", "ethernet", g_plan.ethernet.netmask, sizeof(g_plan.ethernet.netmask), true, 15, result);
        if (err != CTPROV_OK) return err;
        if (!is_ipv4_text(g_plan.ethernet.ip) || !is_ipv4_text(g_plan.ethernet.gateway) ||
            !is_ipv4_text(g_plan.ethernet.netmask)) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "ethernet ip/gateway/netmask must be dotted IPv4 addresses");
        }
    } else {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "ethernet.ip_mode must be 'dhcp' or 'static'");
    }
    g_plan.sections |= SEC_ETHERNET;
    return CTPROV_OK;
}

static ct_prov_err_t validate_comm(const cJSON *section, ct_prov_result_t *result)
{
    static const char *const allowed[] = { "type", NULL };
    char type[16];
    ct_prov_err_t err;

    err = check_fields(section, "comm", allowed, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "type", "comm", type, sizeof(type), true, sizeof(type) - 1, result);
    if (err != CTPROV_OK) return err;

    g_plan.comm_type = communication_type_from_string(type);
    if (g_plan.comm_type == COMM_TYPE_NONE) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "comm.type '%s' is not a declared network", type);
    }
    g_plan.sections |= SEC_COMM;
    return CTPROV_OK;
}

static ct_prov_err_t validate_mqtt(const cJSON *section, ct_prov_result_t *result)
{
    static const char *const allowed[] = { "enable", "host", "port", "client_id", "username", "password", "tls", "topic",
                                           "qos", NULL
                                         };
    mqtt_service_config_t *stored;
    ct_prov_err_t err;
    long port = 0;
    long qos = 0;

    err = check_fields(section, "mqtt", allowed, result);
    if (err != CTPROV_OK) return err;
    err = get_bool(section, "enable", "mqtt", true, &g_plan.mqtt.enable, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "host", "mqtt", g_plan.mqtt.host, sizeof(g_plan.mqtt.host), true, sizeof(g_plan.mqtt.host) - 1,
                     result);
    if (err != CTPROV_OK) return err;
    err = get_int(section, "port", "mqtt", true, 1, 65535, &port, result);
    if (err != CTPROV_OK) return err;
    g_plan.mqtt.port = (uint16_t)port;
    err = get_string(section, "client_id", "mqtt", g_plan.mqtt.client_id, sizeof(g_plan.mqtt.client_id), false,
                     sizeof(g_plan.mqtt.client_id) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "username", "mqtt", g_plan.mqtt.username, sizeof(g_plan.mqtt.username), false,
                     sizeof(g_plan.mqtt.username) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "password", "mqtt", g_plan.mqtt.password, sizeof(g_plan.mqtt.password), false,
                     sizeof(g_plan.mqtt.password) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_bool(section, "tls", "mqtt", false, &g_plan.mqtt.tls, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "topic", "mqtt", g_plan.mqtt.topic, sizeof(g_plan.mqtt.topic), false,
                     sizeof(g_plan.mqtt.topic) - 1, result);
    if (err != CTPROV_OK) return err;
    err = get_int(section, "qos", "mqtt", false, 0, 2, &qos, result);
    if (err != CTPROV_OK) return err;
    g_plan.mqtt.qos = (uint8_t)qos;

    if (strchr(g_plan.mqtt.topic, '+') != NULL || strchr(g_plan.mqtt.topic, '#') != NULL) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "mqtt.topic must not contain wildcards");
    }
    if (g_plan.mqtt.tls) {
        // TLS is the presence of a CA certificate, which is uploaded on the web UI
        stored = (mqtt_service_config_t *)buffer_calloc(1, sizeof(*stored));
        bool has_ca = false;
        if (stored != NULL) {
            has_ca = (json_config_get_mqtt_service_config(stored) == AICAM_OK) && (stored->base_config.ca_cert_path[0] != '\0');
            buffer_free(stored);
        }
        if (!has_ca) {
            return fail(result, CTPROV_ERR_INVALID_ARGUMENT,
                        "mqtt.tls needs a CA certificate on the device (web UI) or a built-in one");
        }
    }
    g_plan.sections |= SEC_MQTT;
    return CTPROV_OK;
}

static ct_prov_err_t validate_webhook(const cJSON *section, ct_prov_result_t *result)
{
    static const char *const allowed[] = { "enable", "url", "secret", NULL };
    ct_prov_err_t err;

    err = check_fields(section, "webhook", allowed, result);
    if (err != CTPROV_OK) return err;
    err = get_bool(section, "enable", "webhook", true, &g_plan.webhook.enable, result);
    if (err != CTPROV_OK) return err;
    err = get_string(section, "url", "webhook", g_plan.webhook.url, sizeof(g_plan.webhook.url), true,
                     sizeof(g_plan.webhook.url) - 1, result);
    if (err != CTPROV_OK) return err;
    g_plan.webhook.has_secret = false;
    err = get_string(section, "secret", "webhook", g_plan.webhook.secret, sizeof(g_plan.webhook.secret), false,
                     sizeof(g_plan.webhook.secret) - 1, result);
    if (err != CTPROV_OK) return err;

    if (strncmp(g_plan.webhook.url, "http://", 7) != 0 && strncmp(g_plan.webhook.url, "https://", 8) != 0) {
        return fail(result, CTPROV_ERR_INVALID_ARGUMENT, "webhook.url must be an http:// or https:// URI");
    }
    g_plan.webhook.has_secret = (g_plan.webhook.secret[0] != '\0');
    g_plan.sections |= SEC_WEBHOOK;
    return CTPROV_OK;
}

// ==================== config.set: persistence ====================

/// @brief Push an IP mode into the W5500 netif (down -> config -> up if it was up)
static aicam_result_t eth_apply_config(bool static_ip, const uint8_t ip[4], const uint8_t gw[4], const uint8_t mask[4])
{
    netif_config_t *cfg = (netif_config_t *)buffer_calloc(1, sizeof(*cfg));
    aicam_result_t ret;

    if (cfg == NULL) {
        return AICAM_ERROR_NO_MEMORY;
    }
    ret = nm_get_netif_cfg(NETIF_NAME_ETH_WAN, cfg);
    if (ret == AICAM_OK) {
        cfg->ip_mode = static_ip ? NETIF_IP_MODE_STATIC : NETIF_IP_MODE_DHCP;
        if (static_ip) {
            memcpy(cfg->ip_addr, ip, 4);
            memcpy(cfg->gw, gw, 4);
            memcpy(cfg->netmask, mask, 4);
        }
        ret = nm_set_netif_cfg(NETIF_NAME_ETH_WAN, cfg);
    }
    buffer_free(cfg);
    return ret;
}

static aicam_result_t persist_ethernet(void)
{
    ble_prov_eth_store_t store;
    aicam_result_t ret;

    memset(&store, 0, sizeof(store));
    store.magic     = BLE_PROV_ETH_MAGIC;
    store.static_ip = g_plan.ethernet.static_ip ? 1 : 0;
    ipv4_text_to_bytes(g_plan.ethernet.ip, store.ip);
    ipv4_text_to_bytes(g_plan.ethernet.gateway, store.gw);
    ipv4_text_to_bytes(g_plan.ethernet.netmask, store.mask);
    // storage_nvs_write() returns the number of bytes written, a negative
    // errno on failure
    ret = storage_nvs_write(NVS_USER, BLE_PROV_ETH_NVS_KEY, &store, sizeof(store));
    return (ret < 0) ? ret : AICAM_OK;
}

/// @brief Restore the Ethernet/PoE IP mode at boot: the W5500 netif starts
///        from its compiled-in defaults (DHCP), the stored mode is applied
///        once the service layer comes up.
void ble_prov_config_eth_boot_apply(void)
{
    ble_prov_eth_store_t *store = (ble_prov_eth_store_t *)buffer_calloc(1, sizeof(*store));
    int len;

    if (store == NULL) {
        return;
    }
    len = storage_nvs_read(NVS_USER, BLE_PROV_ETH_NVS_KEY, store, sizeof(*store));
    if (len == (int)sizeof(*store) && store->magic == BLE_PROV_ETH_MAGIC && store->static_ip) {
        LOG_SVC_INFO(LOG_TAG "restoring static IP %u.%u.%u.%u on the PoE port\r\n",
                     store->ip[0], store->ip[1], store->ip[2], store->ip[3]);
        (void)eth_apply_config(true, store->ip, store->gw, store->mask);
    }
    buffer_free(store);
}

static aicam_result_t persist_halow(void)
{
    network_service_config_t *net = (network_service_config_t *)buffer_calloc(1, sizeof(*net));
    aicam_result_t ret;

    if (net == NULL) {
        return AICAM_ERROR_NO_MEMORY;
    }
    ret = json_config_get_network_service_config(net);
    if (ret == AICAM_OK) {
        strncpy(net->halow_ssid, g_plan.halow_ssid, sizeof(net->halow_ssid) - 1);
        strncpy(net->halow_password, g_plan.halow_password, sizeof(net->halow_password) - 1);
        net->halow_security = (g_plan.halow_password[0] != '\0') ? WIRELESS_SAE : WIRELESS_OPEN;
        net->halow_bssid[0] = '\0';
        if (g_plan.halow_country_code[0] != '\0') {
            strncpy(net->halow_country_code, g_plan.halow_country_code, sizeof(net->halow_country_code) - 1);
        }
        ret = json_config_set_network_service_config(net);
    }
    buffer_free(net);
    return ret;
}

static aicam_result_t persist_mqtt(void)
{
    mqtt_service_config_t *mqtt = (mqtt_service_config_t *)buffer_calloc(1, sizeof(*mqtt));
    aicam_result_t ret;

    if (mqtt == NULL) {
        return AICAM_ERROR_NO_MEMORY;
    }
    ret = json_config_get_mqtt_service_config(mqtt);
    if (ret == AICAM_OK) {
        strncpy(mqtt->base_config.hostname, g_plan.mqtt.host, sizeof(mqtt->base_config.hostname) - 1);
        mqtt->base_config.port = g_plan.mqtt.port;
        // Empty means "the device default" for these two
        strncpy(mqtt->base_config.client_id,
                (g_plan.mqtt.client_id[0] != '\0') ? g_plan.mqtt.client_id : BLE_PROV_MQTT_CLIENT_ID_AUTO,
                sizeof(mqtt->base_config.client_id) - 1);
        strncpy(mqtt->base_config.username, g_plan.mqtt.username, sizeof(mqtt->base_config.username) - 1);
        strncpy(mqtt->base_config.password, g_plan.mqtt.password, sizeof(mqtt->base_config.password) - 1);
        mqtt->data_report_topic[0] = '\0';
        strncpy(mqtt->data_report_topic, g_plan.mqtt.topic, sizeof(mqtt->data_report_topic) - 1);
        mqtt->data_report_qos = g_plan.mqtt.qos;
        if (!g_plan.mqtt.tls) {
            mqtt->base_config.ca_cert_path[0] = '\0';
            mqtt->base_config.ca_cert_len    = 0;
            mqtt->base_config.ca_cert_data[0] = '\0';
        }
        ret = json_config_set_mqtt_service_config(mqtt);
    }
    buffer_free(mqtt);
    return ret;
}

/// @brief mqtt.enable / webhook.enable select which egress receives captures:
///        this firmware has a single upload protocol switch.
static aicam_result_t persist_upload_protocol(void)
{
    capture_upload_config_t *capture = (capture_upload_config_t *)buffer_calloc(1, sizeof(*capture));
    aicam_result_t ret;

    if (capture == NULL) {
        return AICAM_ERROR_NO_MEMORY;
    }
    json_config_capture_upload_defaults(capture);
    ret = json_config_get_capture_upload_config(capture);
    if (ret == AICAM_OK) {
        if ((g_plan.sections & SEC_MQTT) && g_plan.mqtt.enable) {
            capture->upload_protocol = UPLOAD_PROTO_MQTT;
        } else if ((g_plan.sections & SEC_WEBHOOK) && g_plan.webhook.enable) {
            capture->upload_protocol = UPLOAD_PROTO_WEBHOOK;
        } else if ((g_plan.sections & SEC_MQTT) && !g_plan.mqtt.enable) {
            capture->upload_protocol = UPLOAD_PROTO_WEBHOOK;
        }
        ret = json_config_set_capture_upload_config(capture);
        if (ret == AICAM_OK) {
            ret = upload_coordinator_reload_config();
        }
    }
    buffer_free(capture);
    return ret;
}

static aicam_result_t persist_webhook(void)
{
    webhook_config_t webhook;
    aicam_result_t ret;

    ret = json_config_get_webhook_config(&webhook);
    if (ret != AICAM_OK) {
        return ret;
    }
    webhook.enable = g_plan.webhook.enable ? AICAM_TRUE : AICAM_FALSE;
    strncpy(webhook.url, g_plan.webhook.url, sizeof(webhook.url) - 1);
    webhook.url[sizeof(webhook.url) - 1] = '\0';
    if (g_plan.webhook.has_secret) {
        strncpy(webhook.secret, g_plan.webhook.secret, sizeof(webhook.secret) - 1);
        // A secret only reaches the request when some auth mode uses it
        if (strcmp(webhook.auth_type, "none") == 0) {
            strcpy(webhook.auth_type, "bearer");
        }
    }
    return json_config_set_webhook_config(&webhook);
}

void ble_prov_config_set(const cJSON *sections, cJSON *data, ct_prov_result_t *result)
{
    const cJSON *section = NULL;
    static const struct {
        const char *name;
        uint32_t bit;
        ct_prov_err_t (*validate)(const cJSON *section, ct_prov_result_t *result);
    } table[] = {
        { "wifi",     SEC_WIFI,     validate_wifi     },
        { "cellular", SEC_CELLULAR, validate_cellular },
        { "halow",    SEC_HALOW,    validate_halow    },
        { "ethernet", SEC_ETHERNET, validate_ethernet },
        { "comm",     SEC_COMM,     validate_comm     },
        { "mqtt",     SEC_MQTT,     validate_mqtt     },
        { "webhook",  SEC_WEBHOOK,  validate_webhook  },
    };
    ct_prov_err_t err = CTPROV_OK;
    aicam_result_t ret = AICAM_OK;
    uint32_t present;
    cJSON *stored;

    if (g_applying) {
        fail(result, CTPROV_ERR_BUSY, "the previous configuration is still being applied");
        return;
    }

    // Pass 1: validate everything into the plan; nothing is written on any failure
    memset(&g_plan, 0, sizeof(g_plan));
    cJSON_ArrayForEach(section, sections) {
        const char *name = (section->string != NULL) ? section->string : "";
        size_t i;

        if (!cJSON_IsObject(section)) {
            fail(result, CTPROV_ERR_INVALID_ARGUMENT, "section '%s' must be an object", name);
            return;
        }
        for (i = 0; i < sizeof(table) / sizeof(table[0]) && strcmp(table[i].name, name) != 0; i++) {
            // find the section
        }
        if (i >= sizeof(table) / sizeof(table[0])) {
            err = fail(result, CTPROV_ERR_SECTION_NOT_SUPPORTED, "section '%s' is not configurable on this device", name);
            break;
        }
        if (g_plan.sections & table[i].bit) {
            err = fail(result, CTPROV_ERR_INVALID_ARGUMENT, "section '%s' is given twice", name);
            break;
        }
        err = table[i].validate(section, result);
        if (err != CTPROV_OK) {
            break;
        }
    }
    if (err == CTPROV_OK && g_plan.sections == 0) {
        err = fail(result, CTPROV_ERR_INVALID_ARGUMENT, "the request carries no config section");
    }
    if (err != CTPROV_OK) {
        g_plan.sections = 0;
        return;
    }

    // Pass 2: persist
    present = g_plan.sections;
    if (present & SEC_WIFI) {
        ret = communication_store_wifi_credentials(g_plan.wifi_ssid, g_plan.wifi_password,
                                                   (g_plan.wifi_bssid[0] != '\0') ? g_plan.wifi_bssid : NULL);
    }
    if (ret == AICAM_OK && (present & SEC_CELLULAR)) {
        ret = communication_cellular_store_settings(&g_plan.cellular);
    }
    if (ret == AICAM_OK && (present & SEC_HALOW)) {
        ret = persist_halow();
    }
    if (ret == AICAM_OK && (present & SEC_ETHERNET)) {
        ret = persist_ethernet();
    }
    if (ret == AICAM_OK && (present & SEC_COMM)) {
        ret = communication_set_preferred_type(g_plan.comm_type);
    }
    if (ret == AICAM_OK && (present & SEC_MQTT)) {
        ret = persist_mqtt();
    }
    if (ret == AICAM_OK && (present & SEC_WEBHOOK)) {
        ret = persist_webhook();
    }
    if (ret == AICAM_OK && (present & (SEC_MQTT | SEC_WEBHOOK))) {
        ret = persist_upload_protocol();
    }
    if (ret != AICAM_OK) {
        LOG_SVC_ERROR(LOG_TAG "config.set persistence failed: %d\r\n", (int)ret);
        g_plan.sections = 0;
        fail(result, CTPROV_ERR_STORAGE_FAILED, "failed to store the configuration (%d)", (int)ret);
        return;
    }
    LOG_SVC_INFO(LOG_TAG "config.set stored sections 0x%02X\r\n", (unsigned)present);

    stored = cJSON_AddArrayToObject(data, "stored");
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (present & table[i].bit) {
            cJSON_AddItemToArray(stored, cJSON_CreateString(table[i].name));
        }
    }
    cJSON_AddStringToObject(data, "apply",
                            (present & (SEC_WIFI | SEC_CELLULAR | SEC_HALOW | SEC_ETHERNET | SEC_COMM)) ? "reconnect"
                                                                                                        : "immediate");
}

// ==================== apply ====================

static void apply_cellular(void)
{
    cellular_connection_settings_t settings;

    if (!communication_cellular_is_available()) {
        LOG_SVC_INFO(LOG_TAG "cellular module not present; settings apply when it is\r\n");
        return;
    }
    if (communication_cellular_get_settings(&settings) == AICAM_OK) {
        (void)communication_cellular_set_settings(&settings);
    }
}

static void apply_halow(void)
{
    netif_config_t *cfg = (netif_config_t *)buffer_calloc(1, sizeof(*cfg));

    if (cfg == NULL) {
        return;
    }
    if (!communication_is_type_available(COMM_TYPE_HALOW) || nm_get_netif_cfg(NETIF_NAME_WIFI_HALOW, cfg) != 0) {
        LOG_SVC_INFO(LOG_TAG "HaLow module not present; settings apply when it is\r\n");
    } else {
        strncpy(cfg->wireless_cfg.ssid, g_plan.halow_ssid, sizeof(cfg->wireless_cfg.ssid) - 1);
        strncpy(cfg->wireless_cfg.pw, g_plan.halow_password, sizeof(cfg->wireless_cfg.pw) - 1);
        cfg->wireless_cfg.security = (g_plan.halow_password[0] != '\0') ? WIRELESS_SAE : WIRELESS_OPEN;
        if (g_plan.halow_country_code[0] != '\0') {
            strncpy(cfg->halow_cfg.country_code, g_plan.halow_country_code, sizeof(cfg->halow_cfg.country_code) - 1);
        }
        (void)nm_set_netif_cfg(NETIF_NAME_WIFI_HALOW, cfg);
    }
    buffer_free(cfg);
}

void ble_prov_config_apply(void)
{
    uint32_t present = g_plan.sections;
    aicam_result_t ret;

    // Network sections first, then the selector (section 7 of the config schema)
    if (present & SEC_WIFI) {
        ret = wifi_join(g_plan.wifi_ssid, (g_plan.wifi_bssid[0] != '\0') ? g_plan.wifi_bssid : NULL);
        LOG_SVC_INFO(LOG_TAG "apply wifi '%s': %d\r\n", g_plan.wifi_ssid, (int)ret);
    }
    if (present & SEC_CELLULAR) {
        apply_cellular();
    }
    if (present & SEC_HALOW) {
        apply_halow();
    }
    if (present & SEC_ETHERNET) {
        uint8_t ip[4], gw[4], mask[4];

        ipv4_text_to_bytes(g_plan.ethernet.ip, ip);
        ipv4_text_to_bytes(g_plan.ethernet.gateway, gw);
        ipv4_text_to_bytes(g_plan.ethernet.netmask, mask);
        ret = eth_apply_config(g_plan.ethernet.static_ip, ip, gw, mask);
        LOG_SVC_INFO(LOG_TAG "apply ethernet %s: %d\r\n", g_plan.ethernet.static_ip ? "static" : "dhcp", (int)ret);
    }
    if (present & SEC_MQTT) {
        ret = mqtt_service_reload_config(AICAM_TRUE);
        LOG_SVC_INFO(LOG_TAG "apply mqtt: %d\r\n", (int)ret);
    }
    if (present & SEC_COMM) {
        ret = communication_switch_type(g_plan.comm_type, NULL);
        LOG_SVC_INFO(LOG_TAG "apply comm type %s: %d\r\n", communication_type_to_string(g_plan.comm_type), (int)ret);
    }
}

// ==================== config.get ====================

void ble_prov_config_get(cJSON *data, ct_prov_result_t *result)
{
    network_service_config_t *net = (network_service_config_t *)buffer_calloc(1, sizeof(*net));
    mqtt_service_config_t *mqtt = (mqtt_service_config_t *)buffer_calloc(1, sizeof(*mqtt));
    capture_upload_config_t *capture = (capture_upload_config_t *)buffer_calloc(1, sizeof(*capture));
    webhook_config_t webhook;
    cJSON *section;
    const network_scan_result_t *newest = NULL;
    communication_type_t type;

    if (net == NULL || mqtt == NULL || capture == NULL) {
        buffer_free(net);
        buffer_free(mqtt);
        buffer_free(capture);
        fail(result, CTPROV_ERR_INTERNAL_ERROR, "out of memory");
        return;
    }
    if (json_config_get_network_service_config(net) != AICAM_OK) {
        memset(net, 0, sizeof(*net));
    }
    if (json_config_get_mqtt_service_config(mqtt) != AICAM_OK) {
        memset(mqtt, 0, sizeof(*mqtt));
    }
    json_config_capture_upload_defaults(capture);
    if (json_config_get_capture_upload_config(capture) != AICAM_OK) {
        memset(capture, 0, sizeof(*capture));
    }
    if (json_config_get_webhook_config(&webhook) != AICAM_OK) {
        memset(&webhook, 0, sizeof(webhook));
    }

    // wifi: the most recently used known network
    for (uint32_t i = 0; i < net->known_network_count && i < 16; i++) {
        if (net->known_networks[i].ssid[0] == '\0') {
            continue;
        }
        if (newest == NULL || net->known_networks[i].last_connected_time > newest->last_connected_time) {
            newest = &net->known_networks[i];
        }
    }
    if (newest != NULL) {
        section = cJSON_AddObjectToObject(data, "wifi");
        cJSON_AddStringToObject(section, "ssid", newest->ssid);
        if (strcmp(newest->bssid, BLE_PROV_ANY_BSSID_TEXT) != 0 && newest->bssid[0] != '\0') {
            cJSON_AddStringToObject(section, "bssid", newest->bssid);
        }
    }

    if (net->cellular.apn[0] != '\0') {
        const char *auth = "none";

        section = cJSON_AddObjectToObject(data, "cellular");
        cJSON_AddStringToObject(section, "apn", net->cellular.apn);
        if (net->cellular.username[0] != '\0') {
            cJSON_AddStringToObject(section, "username", net->cellular.username);
        }
        if (net->cellular.authentication == CELLULAR_AUTH_PAP) {
            auth = "pap";
        } else if (net->cellular.authentication == CELLULAR_AUTH_CHAP) {
            auth = "chap";
        } else if (net->cellular.authentication == CELLULAR_AUTH_PAP_CHAP) {
            auth = "pap_or_chap";
        }
        cJSON_AddStringToObject(section, "auth", auth);
        cJSON_AddBoolToObject(section, "roaming", net->cellular.enable_roaming ? true : false);
    }

    if (net->halow_ssid[0] != '\0') {
        section = cJSON_AddObjectToObject(data, "halow");
        cJSON_AddStringToObject(section, "ssid", net->halow_ssid);
        if (net->halow_country_code[0] != '\0') {
            cJSON_AddStringToObject(section, "country", net->halow_country_code);
        }
    }

    // ethernet: the stored provisioning state (the W5500 default is DHCP)
    {
        ble_prov_eth_store_t *store = (ble_prov_eth_store_t *)buffer_calloc(1, sizeof(*store));

        if (store != NULL) {
            int len = storage_nvs_read(NVS_USER, BLE_PROV_ETH_NVS_KEY, store, sizeof(*store));

            if (len == (int)sizeof(*store) && store->magic == BLE_PROV_ETH_MAGIC) {
                section = cJSON_AddObjectToObject(data, "ethernet");
                cJSON_AddStringToObject(section, "ip_mode", store->static_ip ? "static" : "dhcp");
                if (store->static_ip) {
                    char text[16];
                    snprintf(text, sizeof(text), "%u.%u.%u.%u", store->ip[0], store->ip[1], store->ip[2], store->ip[3]);
                    cJSON_AddStringToObject(section, "ip", text);
                    snprintf(text, sizeof(text), "%u.%u.%u.%u", store->gw[0], store->gw[1], store->gw[2], store->gw[3]);
                    cJSON_AddStringToObject(section, "gateway", text);
                    snprintf(text, sizeof(text), "%u.%u.%u.%u", store->mask[0], store->mask[1], store->mask[2], store->mask[3]);
                    cJSON_AddStringToObject(section, "netmask", text);
                }
            }
            buffer_free(store);
        }
    }

    type = communication_get_preferred_type();
    if (type == COMM_TYPE_NONE || type >= COMM_TYPE_MAX) {
        type = communication_get_current_type();
    }
    if (type != COMM_TYPE_NONE && type < COMM_TYPE_MAX) {
        const char *wire = NULL;

        section = cJSON_AddObjectToObject(data, "comm");
        switch (type) {
            case COMM_TYPE_WIFI:     wire = "wifi"; break;
            case COMM_TYPE_CELLULAR: wire = "cellular"; break;
            case COMM_TYPE_HALOW:    wire = "halow"; break;
            case COMM_TYPE_POE:      wire = "ethernet"; break;
            default: break;
        }
        if (wire != NULL) {
            cJSON_AddStringToObject(section, "type", wire);
        }
    }

    section = cJSON_AddObjectToObject(data, "mqtt");
    cJSON_AddBoolToObject(section, "enable", capture->upload_protocol == UPLOAD_PROTO_MQTT);
    cJSON_AddStringToObject(section, "host", mqtt->base_config.hostname);
    cJSON_AddNumberToObject(section, "port", mqtt->base_config.port);
    if (mqtt->base_config.client_id[0] != '\0' && strcmp(mqtt->base_config.client_id, BLE_PROV_MQTT_CLIENT_ID_AUTO) != 0) {
        cJSON_AddStringToObject(section, "client_id", mqtt->base_config.client_id);
    }
    if (mqtt->base_config.username[0] != '\0') {
        cJSON_AddStringToObject(section, "username", mqtt->base_config.username);
    }
    cJSON_AddBoolToObject(section, "tls", mqtt->base_config.ca_cert_path[0] != '\0');
    if (mqtt->data_report_topic[0] != '\0') {
        cJSON_AddStringToObject(section, "topic", mqtt->data_report_topic);
    }
    cJSON_AddNumberToObject(section, "qos", mqtt->data_report_qos);

    section = cJSON_AddObjectToObject(data, "webhook");
    cJSON_AddBoolToObject(section, "enable", webhook.enable ? true : false);
    if (webhook.url[0] != '\0') {
        cJSON_AddStringToObject(section, "url", webhook.url);
    }

    buffer_free(net);
    buffer_free(mqtt);
    buffer_free(capture);
}
