# 第三方版权与许可

## Required Notice

Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com).

Metal 插件及相关图形补丁继承 NullMoth / WTL / ArrowWhatever 工作，适用 PolyForm Noncommercial License 1.0.0 的部分继续受其条款约束。原文保存在 [许可文件](LICENSES/POLYFORM-NONCOMMERCIAL-1.0.0.txt)，ArrowWhatever 原告知保存在 [NOTICE](LICENSES/ArrowWhatever-NOTICE.md)。

## 目录与许可范围

| 范围 | 许可与来源 |
| --- | --- |
| 根目录及 docs 的新编写文档，tools 的新仓库工具 | MIT，见 LICENSES/MIT.txt |
| archive/graphics/reference 的上游补丁 | 沿用 ArrowWhatever / 对应上游文件许可；涉及 plugin 的部分含 PolyForm Noncommercial |
| archive/graphics/source/plugin 及其本地修改 | PolyForm Noncommercial 1.0.0，保留 NullMoth 告知 |
| archive/graphics 的 i915 / shim / xkpi 文件 | 逐文件保留 MIT、BSD 或其他上游许可，以源码头及来源为准 |
| archive/wifi 的 AirportRTW 衍生补丁与试验源码 | GPL-2.0 范围按原上游文件及依赖确定，原全文已附 |
| archive/bluetooth 的 RealtekBluetoothFirmware 代码 | GPL-2.0-or-later，原版权告知已附 |
| archive/bluetooth 的 BrcmPatchRAM / BlueToolFixup 衍生源码与补丁 | GPL-2.0，BrcmPatchRAM 原许可已附 |
| archive/temperature 的 SMCProcessor 对照源码 | VirtualSMC 原许可及文件版权，原许可全文已附 |
| archive/boot/rebase-usb 的 OpenCore 宿主模型 | OpenCore BSD 许可及其中保留的原作者告知 |
| 固件和编译器运行库依赖 | 从其上游取得，分别遵守固件及依赖许可 |

目录级概括用于定位，文件中已有的版权、SPDX 和许可条款继续适用。不同组件的聚合不改变它们各自的授权范围。

## 改动日期与作者记录

本地修改时间为 2026-11-07 至 2026-11-09。Wi-Fi 修改涉及 completion、IRQ/workloop、RX 队列与 BA 诊断；蓝牙修改涉及目标 USB 名称、EMP2 和扫描回调兼容。具体改动见 CHANGELOG 与组件文档。上游文件保留原版权、作者与许可告知。

## 进一步分发

公开仓库附带图形组件的非商业限制。需要商业使用、将多个组件合并发行或分发驱动二进制时，应逐组件核对授权与完整源码、构建和告知要求。独立新文档的 MIT 许可仅覆盖该部分原创内容。
