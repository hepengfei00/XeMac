# Realtek 蓝牙适配

目标 USB ID 为 `0BDA:8922`，运行固件为 `rtl8922au_fw.bin`。固件加载驱动基于 thegwchr/RealtekBluetoothFirmware 的提交 `22e501a8a0a7aba8bb4e594bc6f40b1a67391af9`；BlueToolFixup 基于 acidanthera/BrcmPatchRAM `2.7.2`。

## 已验证组件组合

| 组件 | 版本 | 用途 |
| --- | --- | --- |
| RealtekBluetoothFirmware | 1.0.2d1 | 适配目标控制器名称及固件加载 |
| BlueToolFixup | 2.7.2d2 | USB-board 选择、EMP2 与扫描 718 的兼容处理 |

## 修改过程

1. USB 已能枚举，加入 Realtek 固件加载驱动及官方 BlueToolFixup 后，正确模块加载、固件上传成功，但控制器仍为 Off。
2. 仅针对 0BDA:8922 保留原 USB 名称，避免错误的控制器识别路径。名称修正本身没有完成最终蓝牙功能验收。
3. 检查 MacPro7,1 的控制器选择，加入官方 `-btlfxboardid` 的 USB-board 兼容参数，并核对内部控制器信息字段格式。早期 13 字节试验值被更正为目标要求的 14 字节。
4. 观察到目标控制器不支持的 EMP2 命令本地拒绝，针对相同系统构建和设备加入有显式启动开关的修正。d1 后日志出现 Stack initialization complete，但扫描继续报 718。
5. d2 增加扫描 reset/stop 回调错误处理的限定修正。后续启动中控制器为 On，配对与音箱连接可用，并保存了配套配置。

源码与补丁分别保存在 `archive/bluetooth/native-fw1/source`、`identity-fix`、`hci-compat` 和 `scan-compat`。BlueToolFixup EMP2 补丁应用于 2.7.2，scan718 补丁在 EMP2 补丁后叠加。运行与保存结果见 [功能摘要](../archive/bluetooth/native-fw1/scan-compat/postboot-user-accepted.json) 和 [配置检查](../archive/bluetooth/native-fw1/accepted-save/save-result.json)。

## 约束与风险

兼容修正针对 **macOS 26.7.2 / 25G332 / x86_64、Realtek 0BDA:8922** 的已观察路径。BlueToolFixup 按唯一目标字节模式和分支检查执行运行时兼容，不应把已保存偏移移植到其他系统构建。

扫描修正抑制了特定回调错误分支，也可能遮蔽相同回调中的其他异常。后续应验证实际扫描、配对、音频质量及长时间会话，而不是只依据控制器开关为 On。

已有只读质量检查包括音箱 A2DP / AVRCP / ACL 连接、双声道 44.1 kHz，以及短时 bluetoothd CPU/进程状态。未覆盖完整吞吐、无线重传、实际音频延迟、长播、麦克风、睡眠唤醒或所有蓝牙外设。

固件应从上游与 linux-firmware 对应来源取得并保留固件许可。公开仓库保留固件文件的来源和哈希，发布二进制时仍需核对源代码、固件和许可告知是否配齐。
