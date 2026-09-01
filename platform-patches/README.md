# 平台补丁（内核 / buildroot）

RK3506B 的 BLE + SoftAP 配网**不只**靠这个仓库的适配层代码。真正让手机能扫到设备的
根因修复在**内核**，把开机到可扫时间砍掉 20~30s 的改动在 **buildroot**。

那两个仓库由 `repo` 管理，remote 指向厂商 SDK 上游镜像
（`http://192.168.1.71/RockchipSDK/...`），既推不上去、也不该把板级改动推进厂商镜像。
所以在这里存一份补丁快照，避免换 SDK 包时丢失——`BLE_NETCFG_NOTES.md` 注意事项 11
说的就是这个风险。

## 目录

| 路径 | 应用到 | 内容 |
|------|--------|------|
| `kernel/0001-*.patch` | `kernel-6.1/` | `HCI_QUIRK_BROKEN_EXT_ADV`、legacy 广播不超时、100/120ms 广播间隔、rtk_btusb 与 USB 改内建 |
| `buildroot/0001-*.patch` | `buildroot/` | S39/S40/S41/S97/S98 启动链、S01overlayfs 预建 dbus 目录、`etc/main.conf`、`deploy-wukong-app.sh` |
| `buildroot/0002-*.patch` | `buildroot/` | S97soft_rtc 回退时间修正到证书有效期内 |
| `buildroot/0003-*.patch` | `buildroot/` | `etc/fstab` 给 `/tmp`(16M) 和 `/var/log`(8M) 两个 tmpfs 加 `size=` 上限 |

## 应用方式

```sh
cd <SDK>/kernel-6.1
git am  <此仓库>/platform-patches/kernel/*.patch

cd <SDK>/buildroot
git am  <此仓库>/platform-patches/buildroot/*.patch
```

`git am` 失败时（换了 SDK 版本导致上下文漂移）用 `git apply --reject` 再手工收 `.rej`。

## 补丁里**没有**什么

导出时刻意排除了两个文件，它们是本仓库的构建产物，放进补丁会循环嵌套
（带上 2MB 二进制会让补丁从 36KB 涨到 1.9MB）：

- `board/alientek/atk-dlrk3506/fs-overlay/usr/bin/tuyaos_demo_wukong_ai`
- `board/alientek/atk-dlrk3506/fs-overlay/etc/wukong_ai_build`

打完补丁后用 buildroot 里的脚本重新生成这两个：

```sh
<SDK>/buildroot/board/alientek/atk-dlrk3506/deploy-wukong-app.sh
```

内核补丁里**包含** `firmware/rtl8733bu_fw` 和 `rtl8733bu_config`（约 55KB 二进制）。
这两个不是构建产物，`CONFIG_EXTRA_FIRMWARE` 要在 probe 阶段（早于 rootfs 挂载）用到它们，
必须随补丁走。

## 同步纪律

改了内核或 buildroot 后，**重新导出补丁并一并提交**，否则这里的快照会悄悄过时：

```sh
cd <SDK>/kernel-6.1 && git format-patch -1 --no-numbered \
    -o <此仓库>/platform-patches/kernel HEAD

cd <SDK>/buildroot  && git format-patch -3 --no-numbered \
    -o <此仓库>/platform-patches/buildroot HEAD \
    -- . ':(exclude)board/alientek/atk-dlrk3506/fs-overlay/usr/bin/tuyaos_demo_wukong_ai' \
         ':(exclude)board/alientek/atk-dlrk3506/fs-overlay/etc/wukong_ai_build'
```

补丁导出自这两个本地分支（提交号仅供本机对照，换机后无意义）：

- `kernel-6.1` 分支 `rk3506b-ble-netcfg`
- `buildroot` 分支 `rk3506b-ble-netcfg`
