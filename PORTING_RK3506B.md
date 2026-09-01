# Porting tuyaos_demo_wukong_ai to ATK-DLRK3506B — build record

The RK3506B TuyaOS AI SDK (`...arm-none-linux-gnueabihf_RK3506B`) ships 32-bit
armhf `libtuyaos.a` + `libopus.so`, so the app builds and links natively for
RK3506B (Cortex-A7 / ARMv7-A hard-float). This file records the porting work.

## Status

- ✅ Builds & links → `output/tuyaos_demo_wukong_ai_1.0.55/tuyaos_demo_wukong_ai`
  (ELF 32-bit ARM EABI5, ~1.8 MB). Wi-Fi/BT/flash TKL provided; cloud auth
  embedded. Staged into the ATK rootfs + autostart.
- ⚠️ Runtime bring-up (Wi-Fi scan/connect, cloud handshake) needs on-device
  tuning — see "Runtime bring-up".

## Changes made in this app tree

| What | Where |
|------|-------|
| New RK3506B board | `src/boards/RK3506B_BOARD/tuya_device_board.[ch]` |
| RK3506B appconfig | `build/appconfig/RK3506B_BOARD` (+ copied to `build/tuya_app.config`) |
| Board wired into build | `local.mk`, `CMakeLists.txt` (`CONFIG_RK3506B_BOARD`) |
| Resolved scode strings (Kconfig `default X if Y` isn't derived by conf2h.py) | `build/appconfig/RK3506B_BOARD`, `build/tuya_app.config` |
| Guard `tkl_wakeup.h` (deep-sleep, absent on RK3506B) under `ENABLE_LOW_POWER` | `src/tuya_ai_toy.c` |
| Cloud credentials (PID/UUID/AUTHKEY, software auth) | `src/tuya_app_main.c` |

## Changes in the SDK/platform (outside this app dir)

| What | Where |
|------|-------|
| Wi-Fi service flags re-enabled (were on by default) | `include/base/include/tuya_iot_config.h` |
| Platform feature flags: `CONFIG_ENABLE_WIFI=y`, `CONFIG_ENABLE_BLUETOOTH=y` | `vendor/<toolchain>/tuyaos/tuyaos_kernel.config` |

## TKL adaptation added to the vendor adapter

The shipped vendor `tuyaos_adapter/src/` only implements 17 TKL modules
(audio, fs, gpio, media, memory, mutex, ota, output, queue, semaphore, sleep,
system, thread, uart, video_enc, video_in, **wired**) — Wi-Fi/BT/flash were
headers-only. Added in `vendor/<toolchain>/tuyaos/tuyaos_adapter/src/`:

| File | Implements | Notes |
|------|-----------|-------|
| `tkl_wifi.c` | 29 Wi-Fi TKL fns | STA scan/connect/disconnect/status/ip/mac/rssi/bssid/channel/work-mode via **wpa_cli**; AP via **hostapd**; sniffer/mgnt/fast-connect/lp/ioctl → stubs. Event monitor thread translates wpa_state → `WIFI_EVENT_CB`. |
| `tkl_bluetooth.c` | 43 BLE TKL fns | Functional **stubs** (BLE not needed — device uses software auth). Real BLE later if provisioning-over-BLE is required. |
| `tkl_flash.c` | 6 flash TKL fns | **File-backed** flash (`/userdata/tuya_flash.bin`, 4 MiB); KV/MF regions map to file offsets. |
| `tkl_rk3506b_stubs.c` | `TKL_WIFI` feature marker, `set_uart`, `audio_dump_write`, 3× `tuya_ble_*` | Link-resolution stubs for framework/app symbols with no impl on this platform. |

## Credentials

Set in `src/tuya_app_main.c` (software authorization path): `PID`, `UUID` and
`AUTHKEY` are `#define`d near the top of the file.

The values are per-device licences from the Tuya IoT platform and are **not**
recorded here — this repo is public. Get your own PID/UUID/AUTHKEY from the
platform and fill them in locally, or comment the `UUID`/`AUTHKEY` defines out
and burn the licence with the MF production tool instead (that path is what
`__soc_device_init()` takes when the two defines are absent).

## Build

```
cd ../../..   # software/TuyaOS
sh ./build_app.sh apps/tuyaos_demo_wukong_ai tuyaos_demo_wukong_ai 1.0.55
```
Platform (toolchain + internal headers + adapter) is downloaded once into
`vendor/` on first build (~373 MB). Output:
`apps/tuyaos_demo_wukong_ai/output/tuyaos_demo_wukong_ai_1.0.55/tuyaos_demo_wukong_ai`.

## Stage into the ATK image + autostart

```
cp <output>/tuyaos_demo_wukong_ai \
   <atk>/buildroot/board/alientek/atk-dlrk3506/fs-overlay/usr/bin/
```
Autostart: `<atk>/buildroot/board/alientek/atk-dlrk3506/fs-overlay/etc/init.d/S98wukong_ai`
(launches on boot from writable `/userdata/tuya_wukong`; log `/var/log/wukong_ai.log`).
Then rebuild the image (`./build.sh`) and flash.

## Runtime bring-up (on-device, TODO)

These need the physical board + RTL8733BU; tune here:

1. **Wi-Fi interface name**: `tkl_wifi.c` assumes `wlan0`. Verify with `ip link`
   and adjust `TKL_WLAN_IFNAME` if different.
2. **wpa_supplicant**: ensure it runs with a ctrl socket the `wpa_cli` calls can
   reach (`/etc/wpa_supplicant.conf`, `-D nl80211`). The buildroot has connman +
   wpa_supplicant; either let connman manage it or run wpa_supplicant directly.
3. **Provisioning**: with BLE stubbed, the device provisions via **Wi-Fi AP mode**
   (Tuya app) — confirm `tkl_wifi_start_ap`/hostapd brings up the AP and the app's
   netcfg flow passes SSID/PSK to `tkl_wifi_station_connect`. For lab testing you
   can pre-set SSID/PSK and call connect directly.
4. **Flash**: `tkl_flash.c` is file-backed; if KV layout misbehaves, fill
   `tkl_flash_get_one_type_info` capacity fields from your partition layout
   (or point it at an MTD/UBI volume).
5. **Ethernet fallback**: the vendor `tkl_wired.c` is a stub (hardcoded IP); if
   you use Ethernet, fill in the real ioctl impl (a `#if 0` reference is in the
   file) for `tkl_wired_get_ip` / link-status detection.

## Files touched (full list)

App: `src/boards/RK3506B_BOARD/*`, `build/appconfig/RK3506B_BOARD`,
`build/tuya_app.config`, `local.mk`, `CMakeLists.txt`, `src/tuya_ai_toy.c`,
`src/tuya_app_main.c`.
Platform: `include/base/include/tuya_iot_config.h`,
`vendor/<tc>/tuyaos/tuyaos_kernel.config`,
`vendor/<tc>/tuyaos/tuyaos_adapter/src/tkl_wifi.c`,
`.../tkl_bluetooth.c`, `.../tkl_flash.c`, `.../tkl_rk3506b_stubs.c`.
