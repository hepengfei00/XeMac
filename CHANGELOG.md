# 适配变更记录

本记录按组件整理本地修改与验证结果。固定基点见 [构建说明](docs/BUILD.md)，功能限制见 [已知问题](docs/KNOWN-ISSUES.md)。

## DisplayPort 与 HiDPI

- 基于 ArrowWhatever 参考补丁补充桌面主板 DP 路径。
- `0038`：依据 VBT/AUX 与 EDID 选择接口及模式，按实际几何分配 framebuffer，补充 HiDPI 模式。
- `0039`：适配跨宿主 KPI 源码与构建检查。
- `0041`：补充 display-v14 F/G/H 映射，完成目标 DP 的 EDID 读取及 4K60 模式检查。
- `0045`：向 AGDC 发布当前原生模式的 `NVRMTiming`。
- 验证物理 3840×2160 @ 60 Hz、逻辑 1920×1080 和 2× HiDPI。

## Metal 与桌面合成

- 配套 core、Display、Accel、AGDC、XeMTL、Mesa ANV、libdrm 与 translator。
- 修正模块打包并补齐 AIR/LLVM 运行库依赖。
- `0042`：在 WindowServer 路径开放 framebuffer fetch。
- `0044`：跟踪缓冲区实际内存类型，修正 RGBA16Float 纹理别名。
- 验证池化与独立分配、GPU clear、CPU 可见性、Metal 计算和绘制探针。
- 完成桌面合成与实时显示模式检查，补充 Liquid Glass 实机展示。

## Wi-Fi

- 以 AirportRTW V2.3.1 为基点，d1 补充 Release 阶段诊断。
- backend-d2 独立检查后端上电、接口与 IRQ 路径。
- d3 修复 completion 超时路径漏解锁，将 IRQ/C2H 移至独立 workloop；扫描、关联、DHCP 与 HTTPS 通过。
- d4 实现 512 帧 / 4 MiB 有界 RX 队列、批处理和所有权检查；接收队列测试通过，吞吐问题未解决。
- d5 增加 BA/AMPDU 诊断，记录 PMF、受保护 Action 与 ADDBA 超时线索。
- 当前使用 d3，d4/d5 保留为实验分支。

## 蓝牙

- RealtekBluetoothFirmware `1.0.2d1` 为目标 USB 控制器保留原名。
- 核对 USB-board 选择与控制器信息字段，修正字段长度。
- BlueToolFixup d1 增加目标构建上的 EMP2 兼容处理。
- d2 补充扫描 reset/stop 回调处理，完成控制器开启、配对与音箱连接验证。
- 兼容补丁限定于 `25G332 / x86_64 / 0BDA:8922`。

## 传感器

- 使用未修改的官方 SMCProcessor 1.3.7。
- 验证 SMC 温度键存在且读数变化；精度与持续负载对照待测。

## 构建与验证资料

- 固定上游基点，整理分组件补丁链与依赖范围。
- 收录关键源码快照、Wi-Fi 与蓝牙补丁重放结果。
- 增加功能状态、验证方法、已知问题及实机截图。
- 保留各组件版权、许可与 Required Notice。
