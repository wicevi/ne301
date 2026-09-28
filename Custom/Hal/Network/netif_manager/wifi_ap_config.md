# NE301 Wi-Fi AP Configuration (Si917 / WiseConnect 4.1.1)

This is the Wi-Fi configuration used by the NE301 firmware (Silicon Labs
SiWG917 NCP, WiseConnect SDK 4.1.1, external host over SPI). It is provided
as-is so the same setup can be reproduced on another platform. The radio
runs in **concurrent STA + AP mode, 2.4 GHz only**.

---

## 1. Device configuration — `sl_wifi_device_configuration_t`

Applied once per NWP boot via `sl_net_init()` / `sl_wifi_init()`.

```c
sl_wifi_device_configuration_t device_configuration = {
  .boot_option  = LOAD_NWP_FW,                 /* load NWP firmware from host over SPI */
  .mac_address  = NULL,                        /* factory-burned MAC is injected here before
                                                  the first init (FW uses it as the STA address
                                                  and derives the concurrent AP as STA + 1)   */
  .band         = SL_WIFI_BAND_2_4GHZ,
  .region_code  = SL_WIFI_REGION_US,           /* default; may be changed before init      */

  .boot_config = {
    .oper_mode                = SL_SI91X_CONCURRENT_MODE,   /* STA + AP concurrent        */
    .coex_mode                = SL_SI91X_WLAN_ONLY_MODE,    /* BLE not enabled            */

    .feature_bit_map          = SL_SI91X_FEAT_AGGREGATION,

    .tcp_ip_feature_bit_map   = ( SL_SI91X_TCP_IP_FEAT_DHCPV4_CLIENT
                                | SL_SI91X_TCP_IP_FEAT_ICMP
                                | SL_SI91X_TCP_IP_FEAT_SSL
                                | SL_SI91X_TCP_IP_FEAT_DNS_CLIENT
                                | SL_SI91X_TCP_IP_FEAT_EXTENSION_VALID ),
                                /* DHCPV4_SERVER is NOT set: the host runs the DHCP server */

    .custom_feature_bit_map   = SL_SI91X_CUSTOM_FEAT_EXTENTION_VALID,

    .ext_custom_feature_bit_map = ( SL_SI91X_EXT_FEAT_XTAL_CLK        /* external 32 kHz XTAL  */
                                  | MEMORY_CONFIG                     /* BIT(20)|BIT(21)       */
                                  | SL_SI91X_EXT_FEAT_FRONT_END_SWITCH_PINS_ULP_GPIO_4_5_0
                                  | SL_SI91X_EXT_FEAT_UART_SEL_FOR_DEBUG_PRINTS ),
                                  /* debug prints ON: NWP traces stream out of the NWP UART    */

    .bt_feature_bit_map       = 0,
    .ext_tcp_ip_feature_bit_map = ( SL_SI91X_EXT_TCP_IP_DUAL_MODE_ENABLE
                                  | SL_SI91X_EXT_EMB_MQTT_ENABLE
                                  | SL_SI91X_EXT_TCP_IP_WINDOW_SCALING
                                  | SL_SI91X_EXT_TCP_IP_TOTAL_SELECTS(10)
                                  | SL_SI91X_CONFIG_FEAT_EXTENTION_VALID ),
    .ble_feature_bit_map      = 0,
    .ble_ext_feature_bit_map  = 0,
    .config_feature_bit_map   = 0,
  },
};
```

---

## 2. AP profile — `sl_net_wifi_ap_profile_t`

Applied at every AP start via `sl_net_set_profile()` + `sl_wifi_start_ap()`.

```c
sl_net_wifi_ap_profile_t wifi_ap_profile = {
  .config = {
    .ssid.value           = "",                          /* empty: auto-generated at AP start
                                                             as "NE301_XXYYZZ" from the device MAC */
    .ssid.length          = 0,
    .channel.channel      = SL_WIFI_AUTO_CHANNEL,        /* AP follows the STA channel (concurrent) */
    .channel.band         = SL_WIFI_AUTO_BAND,
    .channel.bandwidth    = SL_WIFI_AUTO_BANDWIDTH,
    .security             = SL_WIFI_OPEN,                /* open network (WPA/WPA2-mixed is used
                                                             instead when a >8-char PSK is provided) */
    .encryption           = 0,
    .options              = 0,
    .credential_id        = SL_WIFI_NO_CREDENTIAL_ID,    /* required for SL_WIFI_OPEN;
                                                             SL_NET_DEFAULT_WIFI_AP_CREDENTIAL_ID
                                                             when secured                              */
    .keepalive_type       = SL_SI91X_AP_NULL_BASED_KEEP_ALIVE,
    .beacon_interval      = 100,                         /* TU (1 TU = 1024 us) */
    .client_idle_timeout  = 0xFF,                        /* max */
    .dtim_beacon_count    = 3,
    .maximum_clients      = 3,
    .beacon_stop          = 0,
    .is_11n_enabled       = 0,
  },

  .ip = {
    .mode      = SL_IP_MANAGEMENT_STATIC_IP,
    .type      = SL_IPV4,
    .host_name = NULL,
    .ip = {
      .v4.ip_address.value = 0x0A0AA8C0,   /* 192.168.10.10 */
      .v4.gateway.value    = 0x0A0AA8C0,   /* 192.168.10.10 */
      .v4.netmask.value    = 0x00FFFFFF,   /* 255.255.255.0 */
    },
  },
};
```

Credential used when the AP is secured:

```c
sl_net_wifi_psk_credential_entry_t wifi_ap_credential = {
  .type        = SL_NET_WIFI_PSK,
  .data_length = strlen(psk),      /* must be > 8 to enable the secured mode */
  .data        = psk,
};
/* set via: sl_net_set_credential(SL_NET_DEFAULT_WIFI_AP_CREDENTIAL_ID,
                                   SL_NET_WIFI_PSK, data, length); */
```

---

## 3. Notes for reproduction

- **Enabling SDK debug prints takes two steps**: the boot-config bit above only routes the NWP firmware prints to the NWP UART pins; the verbosity must also be turned on right after `sl_wifi_init()` succeeds:

  ```c
  sl_si91x_assertion_t assertion = {
    .assert_type  = SL_SI91X_ASSERTION_TYPE_ALL,   /* 15: all cores            */
    .assert_level = SL_SI91X_ASSERTION_LEVEL_MAX,  /* 15: all print statements */
  };
  sl_si91x_debug_log(&assertion);
  ```

- The AP address is always static; the DHCP server for AP clients runs on the host (not the NWP firmware).
- In concurrent mode the AP channel must match the STA channel; keep `SL_WIFI_AUTO_CHANNEL` and set the channel from `sl_wifi_get_channel()` of the STA link when it is connected.
- The `ip.v4.*.value` fields are stored in little-endian byte order (the hex literals above are the byte-swapped dotted-quad values shown in the comments).
