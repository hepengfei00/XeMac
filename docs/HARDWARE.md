# 平台与兼容范围

## 测试平台

| 部分 | 配置 |
| --- | --- |
| 处理器 | Intel Core Ultra 7 265K，Arrow Lake 桌面平台 |
| 主板 | Gigabyte Z890M AORUS ELITE WIFI7 |
| 核显 | Intel Xe-LPG，PCI `8086:7D67` |
| 无线网络 | Realtek，PCI `10EC:8922` |
| 蓝牙 | Realtek，USB `0BDA:8922` |
| 系统 | macOS Tahoe 26.7.2，Build `25G332` |
| 引导 | OpenCore 1.0.6，SMBIOS `MacPro7,1` |

## 显示路径

已验证的连接为主板 DisplayPort 至单台显示器，使用 SST。当前实现匹配 DP-H、AUX `0x80`、TC3 与 pipe A 路径。

| 参数 | 已验证值 |
| --- | --- |
| 物理输出 | 3840×2160 @ 60 Hz |
| 逻辑分辨率 | 1920×1080 |
| 缩放 | 2× HiDPI |
| 色彩模式 | 8 bpc SDR |

端口映射与时序处理见 [图形适配](GRAPHICS.md)。HDMI、USB-C、多显示器、HDR 及其他刷新率尚未纳入验证。

## 迁移条件

处理器与主板型号相同，也需要重新核对固件提供的 VBT、ACPI 表、USB 拓扑和显示器 EDID。补丁中的设备匹配、AUX 路径与模式选择应以目标机器的实际信息为准。

蓝牙兼容补丁限定于 `25G332 / x86_64 / 0BDA:8922`。系统升级前应重新检查目标字节模式与配套组件。完整条件见 [蓝牙适配](BLUETOOTH.md)。
