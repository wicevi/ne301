# NE301 Vendor Debug Guide — SiWN917 Wi-Fi NCP

This guide explains how to debug the Wi-Fi subsystem on the NE301 camera
board: how to power it up and reach the device, how to read our logs (and
which parts come from the WiseConnect SDK), which serial-console commands
control the Wi-Fi module, and how to replace/upgrade the NWP firmware
(`.rps`).

- Source repository: <https://github.com/wicevi/ne301> (branch
  **`wifi-debug-dev`**)
- Host MCU: STM32N6570 (Cortex-M55), firmware written in C, built with
  `make`
- Wireless chip on the board: **SiWN917** (NCP variant, see §3.1)
- Host-side SDK: WiseConnect **4.2.0**, vendored under
  `Custom/Common/Lib/SiliconLabs_SDK_4_2_0/`
- NWP firmware shipped on this branch:
  `Custom/Common/Lib/SiliconLabs_SDK_4_2_0/firmware/SiWG917-B.2.17.5.0.0.42.rps`
  — the device reports its firmware version as **2.17.5.42**

> **Naming convention.** The chip soldered on this board is the **SiWN917**,
> the network co-processor (NCP) variant of the SiWG917 family: it has no
> Cortex-M4 application core and is fully controlled by our STM32 host over
> SPI, running the entire Wi-Fi/BT/network stack in its internal network
> processor (**NWP**). Silicon Labs ships the wireless firmware images for
> the whole family under the family name, which is why the file is called
> `SiWG917-B.*.rps` even though the populated chip is the SiWN917. In this
> guide, **NWP firmware** always means the wireless firmware running on the
> SiWN917, and **host firmware** means the NE301 application running on the
> STM32.

An annotated photo of the board (connectors, jumper wires, DIP switch) is
shipped together with this document; the pin descriptions below match that
photo. Where the photo says "burning", this guide says **flashing /
programming** — same thing.

---

## 1. Board hardware quick reference

| Item | Detail |
|---|---|
| Power | USB Type-C port (bottom-left of the photo) |
| Host console UART | 4-pin connector, pins **GND, TX, RX, 3V3 from left to right**. **921600 8N1**. Do NOT connect 3V3 — power the board from Type-C and wire only GND/TX/RX. |
| Host firmware programming port | 4-pin connector, pins **GND, SWCLK, SWDIO, 3V3 from top to bottom** (SWD of the STM32 host). Use the ST-LINK V2 we ship with the board. Do NOT connect 3V3. |
| Programming-mode DIP switch | Slide the DIP switch down, then press the system reset button → the host enters programming mode (labelled "burning mode" in the photo). After flashing, slide it back and reset → normal running mode. |
| System reset button | Next to the Type-C connector. |
| **SiWN917 debug UART1** (for you) | Three jumper wires soldered to the NCP: **yellow = UART1 RX (A20 / GPO8)**, **green = UART1 TX (A21 / GPO9)**, **black = GND**. **460800 8N1.** Connect your USB-UART adapter: adapter TXD → yellow, adapter RXD → green, GND → black. |

The host console (921600) carries the NE301 application logs **and** the
host-side WiseConnect SDK traces. The SiWN917 UART1 (460800) carries the
NWP **firmware-side** debug prints and RAM-dump binary streams — this is
the port where you can capture the SiWN917 firmware's own output.

| Port | Baud rate | Format | Content |
|---|---|---|---|
| Host console (4-pin) | **921600** | 8N1, ASCII | Application logs + host-side SDK traces + CLI |
| SiWN917 UART1 (jumper wires) | **460800** | 8N1; ASCII prints + raw binary dumps | NWP firmware debug prints, RAM dumps |

---

## 2. First connection: the device's Wi-Fi AP and web UI

The board is a standalone device — you do not need a build environment to
start testing. Power it from the Type-C port and wait ~30 s for boot; the
LED goes solid once the system (and the AP) is up.

**On a normal power-up the device starts its own Wi-Fi Soft-AP
automatically** (open network, no Wi-Fi password):

1. The AP name (**SSID**) is printed on the **label stuck on the device** —
   look for something like `NE301_XXYYZZ` (the suffix is derived from the
   device MAC, so every unit has its own name).
2. On your PC, connect to that SSID from the Wi-Fi list.
3. Open **`http://192.168.10.10`** in a browser.
4. Log in to the web UI: user **`admin`**, default password **`hicamthink`**
   (unless it was changed on this unit). Note these are two independent
   things: the **Wi-Fi network itself has no password**; only the web UI
   asks for the login above.

From this web UI you can upgrade the SiWN917 firmware over the air
(§6.1) — no disassembly or cables required beyond Wi-Fi.

If the AP does not appear (note: some low-power wake-up boots intentionally
skip the AP to start faster — a cold power-up always starts it), attach the
host console (§1) and check the boot log; `ifconfig` lists the interfaces
and their state.

---

## 3. Firmware / program structure (the parts relevant to you)

### 3.1 SiWN917 (NCP) vs SiWG917 (SoC)

The populated chip is the **SiWN917 NCP**: a network co-processor with no
user-application core. The Wi-Fi, Bluetooth and offloaded network protocol
stacks run inside its internal NWP, and our STM32 host drives it through a
serial host interface (SPI in our design) using the WiseConnect host APIs.
The **SiWG917 SoC** variant — with the Cortex-M4 application core, more
flash/PSRAM options and the ML accelerator — is the same family and uses
the same `.rps` firmware images, but it is **not** what is mounted here.

### 3.2 Repository layout

```
ne301/                              (branch wifi-debug-dev)
├── Makefile                        top-level build ("make app", "make pkg-wifi", ...)
├── Appli/                          main host application (STM32N6570)
├── FSBL/                           secure first-stage bootloader
├── Web/                            web-UI assets served by the device
├── Custom/Hal/wifi.c               Wi-Fi boot modes: normal / firmware-update / RF test
├── Custom/Hal/Network/netif_manager/
│   ├── sl_net_netif.c              ALL WiseConnect integration: device init config,
│   │                               STA/AP profiles, scan, TWT, recovery, NWP debug
│   ├── netif_manager.c             "ifconfig ..." CLI command implementations
│   └── wifi_ap_config.md           the exact sl_wifi_device_configuration_t and
│                                   sl_net_wifi_ap_profile_t we run (repro reference)
└── Custom/Common/Lib/SiliconLabs_SDK_4_2_0/
    ├── firmware/*.rps              NWP firmware image (SiWG917-B.2.17.5.0.0.42.rps)
    ├── silabs_sdk.mk               build integration + debug-log compile switches
    └── port/, wsd/                 SDK sources (porting layer + WiseConnect components)
```

How the NWP gets its firmware: the SiWN917 keeps its running firmware in
its **own internal flash** and boots from it at every power-up
(`LOAD_NWP_FW` boot option) — **a normal boot never transfers firmware over
SPI**. The OTA-headered image in host flash at `0x71A00000` is only a
**staging copy**: it is written into the SiWN917 exclusively when an update
is explicitly triggered — via the web OTA flow (§6.1, update source = host
flash) or the `wifiup` command (§6.2, which prefers the SD-card file and
falls back to the host-flash copy).

One automatic path exists, for factory-fresh modules only: an NVS counter
records how many times the Wi-Fi firmware has been updated. If that counter
shows the module has (successfully) never been programmed **and** a Wi-Fi
operation comes back with the chip's "no valid firmware present" status
(`SL_STATUS_VALID_FIRMWARE_NOT_PRESENT`), the device enters update mode by
itself — it reboots immediately and pushes the firmware (SD-card file
first, host-flash copy as fallback). The counter also caps this at 3
attempts so a persistent failure cannot reboot-loop. Once the module has
been programmed, this automatic path never fires again; every later update
is one of the explicit methods in §6.

---

## 4. Host console and CLI

Open the 4-pin console connector at **921600 8N1**. There is no login; the
prompt is `AICAM>`. Type `help` (or `?`) to list commands.

> Notation: where a command below is written `wl|ap|hw`, the vertical bars
> mean "type exactly one of the listed names" (e.g. `ifconfig wl scan`).
> Interface names: `wl` = Wi-Fi STA, `ap` = Wi-Fi AP, `hw` = HaLow,
> `4g` = cellular, `ue` = USB ECM, `wn` = Ethernet.

| Command | Purpose |
|---|---|
| `ifconfig` | List all network interfaces + DNS/SNTP |
| `ifconfig <if> init\|up\|down\|deinit` | Bring the interface up/down |
| `ifconfig <if> info` | Interface state, MAC, IP, SSID, channel, firmware version |
| `ifconfig wl cfg <ssid> [password]` | Connect STA (WPA/WPA2-mixed; omit password = open) |
| `ifconfig ap cfg <ssid>` | Configure the Soft-AP SSID (AP currently runs open mode) |
| `ifconfig wl\|ap\|hw scan` / `scan_result` / `scan_update` | Scan and read results |
| `ifconfig wl\|ap fbcast [0\|1]` | Broadcast filtering |
| `ifconfig wl\|ap lpwr [1 [profile] [dtim_skip] [monitor_ms] [beacon_miss]]` | Power-save profile |
| `ifconfig wl\|ap twt [1 [tx_kbps] [tx_lat_ms] [rx_lat_ms]]` | TWT negotiation (blocks up to 5 s for the result) |
| `ifconfig wl\|ap rmode <0\|1>` | Remote wake-up mode |
| `ifconfig wl\|ap nwp_dump [start] [len]` | **NWP RAM dump** — see §5.2 |
| `ifconfig wl\|ap nwp_pc [with_regs]` | **NWP thread PC/R0–R15 readout** — see §5.2 |
| `ifconfig wl\|ap error_test [dma_once\|dma_ff\|dma_off\|sdk_qkill\|sdk_qclear]` | Fault-injection hooks we use to test error paths (do not use unless asked) |
| `wifiup` | Reboot into Wi-Fi firmware-update mode (also the way to leave RF-test mode). Without an SD update file it re-flashes the NWP from the host-flash copy and returns to normal mode. |
| `wifitest` | Reboot into Wi-Fi RF test mode (enables `wifi_set_antenna`, `wifi_transmit_test_start/stop`) |
| `wifi_ap_clients` | List associated AP clients + DHCP leases |
| `wifispi <hex> [count]` | Raw SPI transfer to the NCP (bus-level debugging) |
| `loglevel <debug\|info\|warn\|error> [file]` | Change host console/file log level |
| `sysinfo`, `meminfo`, `tasks`, `reset` | General diagnostics |

---

## 5. Reading the logs — which output is whose?

### 5.1 Host console (921600)

Line format: `[YYYY-MM-DD HH:MM:SS] [MODULE] [LEVEL] message`

| Module tag | Origin |
|---|---|
| `[SLSDK]` | **Host-side WiseConnect SDK internal traces** — every SDK API call, return value, command-engine/event-engine activity, emitted by the SDK's own `SL_DEBUG_LOG_V2` macros through our redirect. These messages are generated by the SDK itself. |
| `[NETIF]` | Our netif layer's **call tracing of SDK APIs**: `sl_wifi_disconnect(SL_WIFI_CLIENT_INTERFACE) -> 0x0` (function + arguments + `sl_status_t` return) and `CB: ...` records whenever an SDK callback fires into our code. Useful to correlate "what we called" with "what the SDK did". |
| `[DRIVER] [HAL] [CORE] [SERVICE] [TASK] [APP] [SYSTEM]` | NE301 application logs (not SDK related). |

The device also keeps a rotated system log file (`aicam.log` on its internal
LittleFS) — SDK traces (`SLSDK`/`NETIF`) are deliberately **console-only**
and never enter that file.

Compile-time switches (all in `Custom/Common/Lib/SiliconLabs_SDK_4_2_0/silabs_sdk.mk`,
all currently **enabled** on this branch):

| Macro | Effect |
|---|---|
| `SL_SDK_DEBUG_LOG_ENABLE` | Arms the host-side redirect of SDK traces to the console (`[SLSDK]` lines) |
| `SLI_HOST_CURRENT_DBGLOG_LEVEL=SLI_HOST_DBGLOG_LVL_DEBUG` | SDK compile-time verbosity gate — without it only WARN/ERROR survive |
| `SL_NET_ENABLE_NWP_DEBUG_PRINTS` | Enables the NWP debug-print feature bit + `sl_si91x_debug_log()` + the `nwp_dump`/`nwp_pc` commands |

On the runtime side, after every successful `sl_wifi_init()` the code calls:

```c
sl_si91x_assertion_t assertion = {
  .assert_type  = SL_SI91X_ASSERTION_TYPE_ALL,   /* 15: all cores (per SDK header)            */
  .assert_level = SL_SI91X_ASSERTION_LEVEL_MAX,  /* 15: all print statements                  */
};
sl_si91x_debug_log(&assertion);
```

(`SL_SI91X_ASSERTION_TYPE_ALL` is documented in the SDK header
`sl_si91x_constants.h` as "enables assertion for all cores".)

This is what turns on the **NWP firmware-side prints** (next section).

### 5.2 SiWN917 UART1 (460800)

Two kinds of output appear on the yellow/green/black jumper wires:

1. **NWP firmware debug prints** (ASCII text). These are the SiWN917
   firmware's own traces — your side's output — enabled by the
   `sl_si91x_debug_log()` call above. Capture with any terminal at
   460800 8N1.
2. **RAM-dump / thread-PC payloads** (raw binary, 460800 8N1):
   - `ifconfig wl nwp_dump [start] [len]` — dumps NWP RAM. Default: the
     full **672 KiB** starting at offset 0. `start` accepts either an
     offset from the NWP RAM base, or an absolute address in the SiWx917
     global memory map from `0x22000000` through `0x220A8000` (672 KiB
     window). The console only prints the trigger/status lines; the
     payload itself **streams out of the NWP UART as binary** — capture it
     with a HEX-capturing tool (e.g. Docklight).
   - `ifconfig wl nwp_pc [1]` — reads the PCs of NWP firmware threads 0–3
     (add argument `1` to also read R0–R15, R15 = SP). Each value is a
     4-byte little-endian word on the NWP UART; the console prints which
     address each chunk belongs to.

Typical crash-analysis flow: reproduce the issue → `ifconfig wl nwp_pc 1`
(the console maps each 4-byte word to a thread/register) →
`ifconfig wl nwp_dump` (full RAM for symbol resolution against the
firmware image/symbols you have for the `.rps` you flashed).

### 5.3 Firmware version formats

The same image is referred to by different strings in different places:

- **`.rps` file name** (6 segments): `SiWG917-B.2.17.5.0.0.42.rps`
- **4-segment version** (`2.17.5.42`): what the web UI shows and what
  `EXPECTED_WIFI_VERSION_STRING` in `Custom/Common/Inc/version.h` expects.
  Derived as `major.minor.patch.(security×100 + build)` from the file-name
  segments (`…5.0.0.42` → `2.17.5.42`).
- Where to read it:
  - **Web UI / `GET /api/v1/device/version`**: field `wifi` = version of
    the image staged in host flash, `wifi_running` = version running on the
    SiWN917, plus `expected_wifi`. (`/api/v1/device/version-check` compares
    them.)
  - **`ifconfig wl info`**: the `fw_version` field prints the raw SDK
    version structure (`chip.rom.major.minor.security.patch.customer.build`)
    — same data, different layout; don't be surprised that it looks
    different from the web UI.

---

## 6. Replacing / upgrading the NWP firmware (`.rps`)

There are three methods, listed from most to least convenient. All of them
end the same way: the host transfers the image to the SiWN917 over SPI
(`sl_si91x_fwup_start`) and the module then resumes normal operation — no
dedicated NWP programmer is needed.

### 6.1 Web OTA (recommended)

**Step 1 — package your new `.rps` into an OTA file.**

The packaging script selects the firmware to ship by parsing the **version
number from the `.rps` file name** (`SiWG917-B.<6-dot-separated
numbers>.rps`) and picking the **highest version found** in any
`Custom/Common/Lib/SiliconLabs_SDK*/firmware/` directory. Therefore:

1. **Delete every old `SiWG917-B.*.rps`** from
   `Custom/Common/Lib/SiliconLabs_SDK_4_2_0/firmware/` (there is normally
   one file, `SiWG917-B.2.17.5.0.0.42.rps`). If an old file with a higher
   version number stays behind, the script may select that old file
   **without any warning** and your test firmware never gets packed — test
   builds often carry unusual version numbers, so this step matters.
2. **Copy your new `.rps` in**, keeping the standard naming scheme
   `SiWG917-B.<version>.rps` (e.g. `SiWG917-B.2.17.6.0.0.1.rps`). The
   version in the file name becomes the OTA package version, so keep it
   accurate.
3. From the repository root run:

   ```
   make pkg-wifi
   ```

   This produces `build/ne301_Wifi_flash_v<version>_pkg.bin` — a 1 KB OTA
   header + the flash image (raw `.rps`).

**Step 2 — upload and install.**

1. Connect to the device's AP and open `http://192.168.10.10` (§2).
2. In the web UI go to *System settings → Firmware upgrade → Import →
   Advanced options → Wi-Fi upgrade* and select the
   `ne301_Wifi_flash_v<version>_pkg.bin` file.
3. Confirm the upgrade — the web UI then **reboots the device automatically**
   (the frontend calls `POST /api/v1/system/restart` by itself). The upload
   itself only stages the image and sets an update flag; the actual
   transfer happens on that reboot: the device pushes the new firmware to
   the SiWN917 (the LED blinks rapidly during the transfer, the console
   prints `wifi_update_process` … `wifi_update ok`, it takes roughly
   **1–3 minutes**), then boots normally with the new firmware.

The same steps via HTTP API instead of the web UI:

```
POST /api/v1/system/ota/upload         (multipart: the *_pkg.bin file)
POST /api/v1/system/ota/upgrade-local  body: {"firmware_type":"wifi"}
POST /api/v1/system/restart            (the web UI does this for you; raw API
                                         callers must reboot manually — the
                                         transfer only happens on the next boot)
```

### 6.2 SD card + `wifiup` (recovery path, no web UI needed)

1. Rename your **raw `.rps` file** to `siwg917` and put it in the root of a
   FAT/exFAT SD card; insert the card.
2. On the serial console run:

   ```
   wifiup
   ```

   The device reboots into update mode and applies the firmware from the
   SD card first — **the SD-card `siwg917` file takes priority**; the
   host-flash copy is only used as a fallback when the file is missing.
   Afterwards the device returns to normal mode.

### 6.3 Host-flash programming (factory / bricked-device recovery)

Flash the image into host flash at `0x71A00000` through the SWD programming
port:

```
make wifi-image        # build flash image (32-byte header + raw .rps)
make flash-wifi        # program it at 0x71A00000 via ST-LINK
```

Slide the DIP switch down + reset to enter programming mode, flash, slide
the switch back and reset. Note that this only **stages** the image in host
flash — the SiWN917 is not updated yet. With no SD card inserted, run
`wifiup` on the console: it finds no SD-card file and pushes the
host-flash copy instead (§6.2 fallback).

---

## 7. Minimal debugging recipes

**Connect to an AP and watch the SDK:**
```
ifconfig wl cfg MySSID mypassword     # [NETIF] lines show each SDK call + status
ifconfig wl info                      # state, IP, channel, firmware version
```

**Read the current NWP firmware version:**
```
ifconfig wl info        # raw SDK version string — see §5.3 for the formats
```
or check the web UI device/system-info page (`wifi_running`, §5.3) for the
4-segment form.

**Capture an NWP crash / hang:**
```
ifconfig wl nwp_pc 1                  # thread PCs (and R0-R15) — 4-byte LE words on the NWP UART
ifconfig wl nwp_dump                  # full 672 KiB RAM — binary stream on the NWP UART
```

**Check RF / association behaviour on the firmware side:** just watch the
460800 NWP UART while reproducing; all firmware debug prints are enabled by
default on this branch.

---

## 8. Notes

- The Soft-AP (`ifconfig ap ...`) currently runs in **open** mode; the
  security-enabled path is implemented in the code but disabled pending a
  product decision.
- The device runs the DHCP server for AP clients on the host (lwIP), not in
  the NWP firmware; for NWP firmware older than 2.16.5 the code
  auto-falls-back to the firmware DHCP server.
- The exact `sl_wifi_device_configuration_t` /
  `sl_net_wifi_ap_profile_t` values we run are documented (as copy-paste
  initializers) in `Custom/Hal/Network/netif_manager/wifi_ap_config.md` —
  use that file to reproduce our radio configuration on your side.
- For questions or issues, please send us: the host console log (921600),
  the NWP UART capture (460800), and the output of `ifconfig wl info`.
