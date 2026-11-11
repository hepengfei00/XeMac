# 适配变更记录

本记录按组件整理本地修改与验证结果。固定基点见 [构建说明](docs/BUILD.md)，功能限制见 [已知问题](docs/KNOWN-ISSUES.md)。

## 2026-11-11 · 适配快照 a2bf4d4

- 修复接管未完成、没有最终健康提交后，后续启动一直停留基础显示的流程缺口：持久化启动阶段，每次严格健康成功后提供一次恢复额度，重试失败暂停，准备中断最多续接一次，不自动重启机器。
- 增加可见状态与手动重试入口；WindowServer 切换记录开始、心跳与最终结果，已通过实时健康检查时可跳过切换。
- 采用当前启动 UUID、随机 nonce 和生成时间校验实时显示，替代旧缓存文件时间验收；提交失败只回退启动配置和缓存，保留图形资源。
- 保留现有图形、Wi-Fi、蓝牙及温度驱动；`RebaseRegions=true`、5 秒菜单和默认 Tahoe 保持。
- 一次新实机启动独立确认物理 3840×2160@60、逻辑 1920×1080、2× HiDPI、stage7/accel2、持续送帧和成功提交；56 项保护文件未变。偶发接管中断的底层原因尚未根治，未宣称长期稳定。
- 发布参考源码、实时显示查询源码、隔离状态测试及去除机器身份的验证摘要。来源与文件哈希见 [发布清单](evidence/adaptation-a2bf4d4.json)，流程与复用条件见 [启动恢复说明](archive/boot/recovery-20261111/README.md)。

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
