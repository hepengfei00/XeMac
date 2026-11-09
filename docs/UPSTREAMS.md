# 上游来源、固定版本与贡献说明

## 直接参考和修改的仓库

| 项目 | 固定版本或提交 | 本仓库的使用方式 |
| --- | --- | --- |
| Mountainatic/ArrowWhatever | 555fbb2f3c29a1a1b2c5a298ee2107222d28baf4 | 保存 0001–0037 补丁，按其分组件基点重放，作为原生 KMS 与 Metal 接管参考 |
| watchthelight-labs/NvGfx，历史 xe 分支 | 5a19a73f36249efb99c44c3f9cb34603ef0c0eb8 | 核心、Display、Accel、AGDC 基底 |
| 同一历史 NvGfx plugin | 497d521e925bd760c8cde5bbc17d9bf96d2ee2b4 | XeMTL plugin 基底，与 core 基点不同 |
| souissiak1-maker/AirportRTW | V2.3.1；763d0dfcf4871c892bd00fe3dad865e81a53be4d | d1、d3、d4、d5 及 backend-d2 的来源 |
| thegwchr/RealtekBluetoothFirmware | 22e501a8a0a7aba8bb4e594bc6f40b1a67391af9 | 固件加载和 USB 名称修正 |
| acidanthera/BrcmPatchRAM | 2.7.2 | BlueToolFixup EMP2 / Scan 718 修正 |
| acidanthera/OpenCorePkg | 1.0.6 | 引导、配置 schema 与 ACPI 接口参考 |
| acidanthera/VirtualSMC | 1.3.7 | 官方 SMCProcessor 部署，1.3.8 仅用于源码对比 |
| acidanthera/Lilu | 兼容性研究参考 1.7.2；实际使用应核对自己的 EFI | 蓝牙补丁、CPU 识别、传感器基础框架 |

原始入口分别为 [ArrowWhatever](https://github.com/Mountainatic/ArrowWhatever)、[历史 NvGfx](https://github.com/watchthelight-labs/NvGfx)、[当前 WTLIntelGfx](https://github.com/watchthelight-labs/WTLIntelGfx)、[AirportRTW](https://github.com/souissiak1-maker/AirportRTW)、[RealtekBluetoothFirmware](https://github.com/thegwchr/RealtekBluetoothFirmware)、[BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM)、[OpenCorePkg](https://github.com/acidanthera/OpenCorePkg)、[VirtualSMC](https://github.com/acidanthera/VirtualSMC) 和 [Lilu](https://github.com/acidanthera/Lilu)。

WTL 的仓库名称发生过变化，历史 `XeGfx` 与 `NvGfx` 链接可能跳转。复现时检查提交对象是否存在、目录布局是否匹配，并以已保存的固定补丁头和哈希判断基点。

## 间接依赖和基础工作

| 项目 | 角色 | 记录中的版本 |
| --- | --- | --- |
| NullMoth Systems 的 macOS Metal 驱动 | WTL/NvGfx 图形体系与 Metal plugin 基础 | 按所列 core/plugin 提交继承 |
| Mesa / ANV | Intel Vulkan 与编译器 | bffd91947efc1932da99aca69fc540d6d4201b0c |
| libdrm | 用户态 DRM / Darwin 接口 | b97cbde15c5c3abfe44d78e8f57139e50f612fec |
| FreeBSD | XNU 兼容层的接口参考 | 6b38db44f0ea36e8336758a460615e9c52def28c |
| drm-kmod / Intel i915 | GPU、显示、AUX、模式设置核心 | 保存标识 drm-kmod-c16de0f |
| steelbrain/metal2vulkan | translator 血缘来源 | 随固定上游源码与许可取得 |
| linux-firmware | Intel/Realtek 固件来源 | 按各组件固定输入取得，分别核对固件许可 |
| OpenIntelWireless/IntelBluetoothFirmware | Realtek 固件驱动脚手架来源 | 随 RealtekBluetoothFirmware 原告知 |
| USBToolBox | USB 配置与映射 | 当前使用过 USBToolBox + UTBDefault；完整定制映射待验收 |
| NVIDIA open-gpu-kernel-modules | 上游 NVRM/兼容代码血缘 | 上游告知记录 r610 / 610.57.04 |

原始仓库为 [Mesa](https://gitlab.freedesktop.org/mesa/mesa)、[libdrm](https://gitlab.freedesktop.org/mesa/drm)、[FreeBSD](https://github.com/freebsd/freebsd-src)、[drm-kmod](https://github.com/freebsd/drm-kmod)、[Linux](https://github.com/torvalds/linux)、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)、[IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware)、[USBToolBox](https://github.com/USBToolBox/tool) 和 [NVIDIA 开放内核模块](https://github.com/NVIDIA/open-gpu-kernel-modules)。

## 文档与问题参考

- OpenCore 1.0.6 Configuration 文档用于核对配置字段、ACPI 接口及安全限制。 [官方文档](https://github.com/acidanthera/OpenCorePkg/blob/1.0.6/Docs/Configuration.tex)
- Dortania 用于安装和 USB 配置参考，具体参数仍需以本机与对应 OpenCore 版本为依据。 [安装指南](https://dortania.github.io/OpenCore-Install-Guide/)、[后续维护指南](https://dortania.github.io/OpenCore-Post-Install/)
- RealtekBluetoothFirmware issue #6 提供 USB 识别及同构建蓝牙兼容线索，实际效果以本机运行记录验收。 [问题页面](https://github.com/thegwchr/RealtekBluetoothFirmware/issues/6)
- WTLNvidiaGfx #10 / #6 提供系统设置图标首次加载与着色器开销的调查线索，本机根因未证实。 [图标问题](https://github.com/watchthelight-labs/WTLNvidiaGfx/issues/10)、[CPU 开销问题](https://github.com/watchthelight-labs/WTLNvidiaGfx/issues/6)

## 本地修改范围

本地工作主要是 265K 桌面外接 DP、真实时序发布、缓冲区类型跟踪、Wi-Fi 锁与队列、目标 Realtek 蓝牙兼容及功能验证工具。既有驱动、渲染框架、GPU 核心、Vulkan 实现和固件属于上述上游及其贡献者。

补丁号 0001–0037 继续注明 ArrowWhatever 来源。本地 0038–0046 与无线、蓝牙补丁按目录标明适配目的，保留上游作者、版权、许可和实际测试局限。已有补丁中的 Generated-by 标记按原样保留，各项构建与测试覆盖范围见验证记录。

## 源码版本

源码基点和补丁前置条件见补丁头与组件文档。复现时应按组件核对固定版本及其补丁链。
