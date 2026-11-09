# 图形栈与桌面 DisplayPort 适配

## 目标与显示链路

目标为 Arrow Lake Xe-LPG `8086:7D67`，通过主板 DisplayPort 输出至单台 SST 显示器。已验证路径为 DP-H、AUX `0x80`、TC3、pipe A；输出 3840×2160 @ 60 Hz，逻辑分辨率 1920×1080，2× HiDPI。

适配覆盖原生模式提交、Metal 渲染和 WindowServer 桌面接管。各层职责如下：

| 层级 | 组件 | 适配职责 |
| --- | --- | --- |
| GPU 与显示核心 | XeGfx / i915 / xkpi | GPU 初始化、AUX、模式设置与 scanout |
| 显示适配 | XeGfxDisplay / XeGfxAGDC | framebuffer、模式表、HiDPI 与当前时序发布 |
| 渲染与合成 | XeGfxAccel / XeMTL / Mesa ANV / libdrm | 加速器接口、Metal 转译、纹理与共享缓冲区 |
| 组件协作 | WTLArrowEarlyClaim | 设备声明与核心加载条件配合 |

内核组件与用户态库需要配套。成功读取 EDID、通过模式检查、运行 Metal 探针和完成桌面接管分别对应不同阶段，验证时应逐项记录。

## 源码基点与组件版本

核心、Display、Accel 和 AGDC 基于历史 NvGfx `xe` 提交 `5a19a73f36249efb99c44c3f9cb34603ef0c0eb8`。XeMTL plugin 使用独立基点 `497d521e925bd760c8cde5bbc17d9bf96d2ee2b4`。在各自基点应用 ArrowWhatever 参考补丁后，再叠加本地修改。

| 组件 | 已验证版本 |
| --- | --- |
| XeGfx | `0.0.44` |
| XeGfxDisplay | `0.5.45` |
| XeGfxAccel | `0.1` |
| XeGfxAGDC | `0.1` |
| WTLArrowEarlyClaim | `1.2` |
| XeMTLDriver | `0.35.44` |

Mesa、libdrm、FreeBSD 和 drm-kmod 的基点及移植依赖见 [构建说明](BUILD.md)。复现时应保存自身构建的版本、架构、Mach-O UUID 和 SHA256，并与实际加载组件对应；AuxKC 须在匹配的目标系统中重新生成。

## 本地补丁索引

| 编号 | 目标源码树 | 修改内容 |
| --- | --- | --- |
| `0038` | core / display / AGDC | 选择外接 DP 与 EDID 模式，按实际模式分配 framebuffer，补充 HiDPI |
| `0039` | core 构建工具 | 跨宿主 KPI 源码与构建检查适配 |
| `0040` | core / display | 核心与显示组件启用条件 |
| `0041` | core / i915 | display-v14 F/G/H 映射，支持本板 DP-H / AUX `0x80` |
| `0042` | plugin | 在 WindowServer 路径开放 framebuffer fetch |
| `0043` | core | 接受已由 EarlyClaim 消耗的有效一次性令牌 |
| `0044` | plugin | 跟踪缓冲区实际内存类型，修正 RGBA16Float 纹理别名 |
| `0045` | display | 向 AGDC 发布当前 KAPI `NVRMTiming` |
| `0046` | EarlyClaim | EarlyClaim 状态诊断 |

本地补丁位于 [archive/graphics/patches](../archive/graphics/patches)，上游 `0001` 至 `0037` 位于 [参考补丁目录](../archive/graphics/reference/patches)。另附 FreeBSD Mach-O / read-frequently 兼容补丁。

`archive/graphics/source` 保存关键修改后源码，供代码审阅和差异核对。完整构建需要固定版本的上游源码及依赖；分组件补丁顺序见 [构建说明](BUILD.md)。

## 关键修改

### 外接 DP 与时序

本平台使用外接 DP。移植时移除对特定笔记本内屏时序的依赖，根据 VBT、AUX 与 EDID 确定输出路径；`0041` 补齐 display-v14 F/G/H 映射后，目标端口可读取完整 EDID 并通过 4K60 模式检查。

`0045` 将当前原生模式的 `NVRMTiming` 发布至 AGDC，使显示上报与实际提交的时序对应。HiDPI 验收同时检查物理像素、逻辑尺寸、刷新率和 HiDPI 标志。

### Metal 与共享缓冲区

Metal 计算或绘制通过后，桌面仍可能在合成路径失败。`0042` 处理 WindowServer 使用的 framebuffer fetch，`0044` 保存真实缓冲区内存类型，修正 RGBA16Float 纹理别名。已保存测试覆盖池化与独立分配、GPU clear 及 CPU 可见性。

运行时还依赖 AIR/LLVM、Mesa ANV、libdrm 与 translator。缺少运行库曾导致探针失败或桌面无法接管，部署时须检查动态链接和目标架构。

## 验证与未完成事项

当前已通过原生 4K60、2× HiDPI、Metal 计算/绘制/纹理探针与 WindowServer 合成路径检查。功能摘要见 [显示与 Metal 验证](../archive/graphics/evidence/display-audit.json)。控制中心与 Dock 的 Liquid Glass 效果见 [实机展示](../README.zh-CN.md#实机展示)。

登录阶段短暂黑块、首次图标加载延迟、部分 App 兼容性、视频硬件解码和长期稳定性仍需继续处理。其他输出接口、多显示器、HDR、睡眠唤醒尚未完成验证，详见 [已知问题](KNOWN-ISSUES.md)。
