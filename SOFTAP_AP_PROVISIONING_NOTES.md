# ATK-DLRK3506B SoftAP 配网适配注意点与问题优化清单

> 平台：ATK-DLRK3506B（RK3506 + RTL8733BU）  
> 模块：TuyaOS Wi-Fi TKL / SoftAP 配网  
> 验证版本：应用约 `1.0.72`（SoftAP → STA → GOT_IP → 云激活可用）  
> 主要改动文件：
> - `vendor/.../tuyaos_adapter/src/tkl_wifi.c`
> - `vendor/.../tuyaos_adapter/src/tkl_wired.c`
> - `apps/tuyaos_demo_wukong_ai/src/tuya_app_main.c`（及重置配网入口）
> - `buildroot/.../fs-overlay/etc/init.d/S98wukong_ai`

本文整理 SoftAP 配网适配过程中需注意的点，以及已落地的问题与优化项。

---

## 一、环境与架构注意点

1. **SoftAP 与 STA 必须分网卡**  
   SoftAP 固定跑在 `wlan1`，STA 固定跑在 `wlan0`，与板级 `wifi_apmode.sh` 一致。不要把 SoftAP 开在 `wlan0`：单口 AP→STA 切换在 RTL8733BU 上极易异常。

2. **SoftAP 依赖 hostapd + dnsmasq**  
   AP 侧用 hostapd；DHCP/网关由 dnsmasq 提供。SoftAP 网关地址按 SDK/默认使用 `192.168.176.1`，STA 侧不得把该地址误判为已获取路由器 IP。

3. **必须关掉 connman（或同类网络管家）**  
   `connmand` 会与 hostapd / wpa_supplicant 抢占 `wlan0`/`wlan1`，导致 SoftAP 起不来或 STA 连错网。TKL 启动配网路径前应 `killall connmand`。

4. **本板 BLE 广播常失败，优先 SoftAP-only**  
   BLE adv 常见 `ret:-4`。应用侧配网/重置统一使用 `WF_START_AP_ONLY`，不要依赖 EZ/BLE 并发作为主路径。

5. **有线口必须报 DOWN（纯 Wi-Fi 产品）**  
   `tkl_wired.c` 必须返回 `TKL_WIRED_LINK_DOWN`。若 stub 成假 LINK_UP，激活流程会误走有线链路，Wi-Fi GOT_IP 后云侧仍异常。

6. **STA 拿地址用 udhcpc，不要用 `-n` 一次性退出模式**  
   SoftAP→STA 切换瞬间关联尚未完成时，`udhcpc -n` 会试一次就退出，此后永远拿不到路由 IP。应后台常驻重试（如 `-b -t 20`）。

7. **禁止 SoftAP 结束后 `iw del wlan1`**  
   在 RTL8733BU 上删除并发 AP 虚接口会挂死驱动/命令，导致后续 STA 超时。正确做法：`wlan1` down + 杀 hostapd/dnsmasq，保留接口。

8. **SoftAP→STA 切换要“慢启动、少命令”**  
   切 STA 后先 settle（约 2s），再按板级 `wifi-connect.sh` 风格：写一份 `/tmp/tkl_wpa_supplicant.conf`，只拉起一次 wpa。避免大量 `wpa_cli` / `scan freq`（切换后易空扫且单次超时约 2s，叠满 SDK 约 20s 窗口）。

9. **解析 wpa 状态字段必须“行首精确匹配”**  
   `tkl_kv()` 不能用裸 `strstr(text, "ssid=")`：会命中 `bssid=` 里的子串 `ssid=`，把 BSSID 当成 SSID，导致链路已 `COMPLETED` 却永远不报 `WSS_GOT_IP`（1.0.72 关键缺陷）。

10. **GOT_IP 必须按目标 SSID / 非 SoftAP IP 过滤**  
    切换后可能误连开放热点（如 `Tuya-Guest`）或残留 SoftAP 地址。应：空 conf 清旧关联、只 enable 目标网络、IP 非 SoftAP 地址、SSID 匹配期望值后再上报 GOT_IP。

---

## 二、适配过程中出现的问题与已做优化（按序号）

1. **问题：SoftAP 起在错误接口 / 与 STA 冲突**  
   **优化：** AP=`wlan1`，STA=`wlan0`；确保存在 concurrent vif 后再起 hostapd。

2. **问题：connman 抢占无线口，AP/STA 不稳定**  
   **优化：** 配网相关路径主动杀掉 `connmand`，由 TKL 独占无线管理。

3. **问题：BLE 配网不可用，重置后进不了可用配网态**  
   **优化：** `tuya_iot_wf_soc_dev_init` / `tuya_iot_wf_gw_fast_unactive` 等入口改为 `WF_START_AP_ONLY`。

4. **问题：有线 stub 假 LINK_UP，激活跑偏**  
   **优化：** `tkl_wired_get_status` / status_cb 固定 `LINK_DOWN`，纯 Wi-Fi 设备不伪造有线就绪。

5. **问题：STA 关联成功但无路由 IP**  
   **优化：** 使用 `udhcpc` 后台重试；关联完成后再确保 DHCP；禁止依赖会立刻退出的 `-n`。

6. **问题：误连开放热点或残留旧网络，出现假 GOT_IP**  
   **优化：** SoftAP 前/切换时清空 wpa 网络配置；只 enable 目标 SSID；GOT_IP 过滤 SoftAP IP 与非目标 SSID。

7. **问题：SoftAP 结束后 `iw del wlan1` 导致 RTL8733BU 挂死**  
   **优化：** 删除该步骤，仅 down 接口并杀 hostapd/dnsmasq。

8. **问题：SoftAP→STA 后大量 `wpa_cli`/`scan freq`，超时且扫不到 BSS**  
   **优化：** 改为单次写 conf + 单次起 wpa；必要时再做轻量 scan/assoc kick，避免命令风暴。

9. **问题：`wpa_state=COMPLETED` 且已有 IP，但 SDK 始终收不到 GOT_IP（关键）**  
   **现象：** status 里出现 `ssid=9c:7f:81:...`（实际是 BSSID），真实 SSID（如 `FAE_TEST`）被丢掉。  
   **根因：** `strstr(..., "ssid=")` 匹配到 `bssid=`。  
   **优化：** `tkl_kv()` 仅在行首（或 `\n` 后）匹配 `key=`；SSID/wpa_state 解析全部走该接口。

10. **问题：SoftAP IP 被当成 STA 已联网**  
    **优化：** 状态机取 IP 时优先真实 iface 地址，并显式排除 SoftAP 网段/配置 IP。

11. **问题：hostapd 假成功（进程未起来）**  
    **优化：** SoftAP start 后用 `pidof hostapd` 校验；失败尽早返回，避免 SDK 以为热点已开。

12. **问题：GOT_IP 之后 SoftAP 残留占用射频/进程**  
    **优化：** 上报 GOT_IP 后快速清理 hostapd/dnsmasq 等 SoftAP 残留（保持路径短，不阻塞激活）。

13. **问题：切换瞬间旧 AP/旧 IP 与新目标网络竞态**  
    **优化：** SoftAP 前强制断 STA、清关联；handoff 时先 recover SoftAP，再按目标 conf 连接。

14. **问题：SDK 线程在 shell/wpa 卡住导致整体超时**  
    **优化：** 外部命令加超时；handoff 后 settle；避免长时间阻塞在 `wpa_cli` 轮询上。

---

## 三、联调与验收建议

1. **串口/日志关注点**  
   - SoftAP：hostapd 已起、手机能搜到热点并拿到 `192.168.176.x`。  
   - 下发路由器信息后：`wpa_state` 最终为 `COMPLETED`，`ssid=` 为目标名（不是 MAC）。  
   - `udhcpc` 拿到非 SoftAP 的局域网 IP。  
   - 上层出现 `WSS_GOT_IP` / 云激活继续，而非卡在连网。

2. **回归用例**  
   - 冷启动 → SoftAP 配网 → 目标路由（有密码）成功激活。  
   - 重置（`WF_START_AP_ONLY`）后再配一次。  
   - 环境中存在开放热点（如 Guest）时，不应误连并假 GOT_IP。  
   - 配网成功后 `wlan1`/hostapd 已清理，STA 保持在线。

3. **单仓库自包含维护**  
   现已单仓库自包含：`app/tuyaos_demo_wukong_ai/` 内含 SDK 头文件/库/toolchain/adapter，不再有外部 TuyaOS 树。`tkl_wifi.c` / `tkl_wired.c` / 应用配网入口改动直接在此仓库内编译（`./build_rk3506b.sh`）即可。

4. **不建议的操作**  
   - SoftAP 跑在 `wlan0`  
   - `iw del wlan1`  
   - 保留 connman 与 TKL 并行管网  
   - 用 `strstr("ssid=")` 解析 wpa status  
   - `udhcpc -n` 作为 SoftAP 切换后的唯一 DHCP 手段  
   - 有线 stub 报 LINK_UP

---

## 四、关键代码索引

| 项 | 位置 |
|----|------|
| SoftAP/STA 接口与 handoff | `tuyaos_adapter/src/tkl_wifi.c` |
| wpa status 行锚点解析 `tkl_kv` | 同上 |
| 有线固定 DOWN | `tuyaos_adapter/src/tkl_wired.c` |
| 配网模式 SoftAP-only | `apps/.../src/tuya_app_main.c`、`tuya_ai_toy.c`、按键/设置重置入口 |
| 开机拉起应用 | `fs-overlay/etc/init.d/S98wukong_ai` |

---

*文档对应 2026-07-22 ~ 2026-07-23 SoftAP 配网联调结论；后续若驱动/板级脚本变更，请优先复核第 1、7、8、9 条。*
