# Realtek Wi‑Fi 适配

目标为 PCI `10EC:8922`，使用 AirportRTW V2.3.1 的原生 IO80211 / Skywalk 控制路径。固定上游提交为 `763d0dfcf4871c892bd00fe3dad865e81a53be4d`。

## 版本与实验分支

| 轮次 | 内容 | 实机与最终用途 |
| --- | --- | --- |
| 上游 V2.3.1 | 保存官方来源、源码包及二进制身份 | 原始基点 |
| d1 | Release 轻量阶段日志，不开启全局 DEBUG/AWDL | 定位 HAL enable / 原生 POWER 等待 |
| backend-d2 | 隔离后端上电与 IRQ 的独立实验 | 证明部分后端路径，未证明扫描或联网 |
| native-d3 | completion 超时解锁 + IRQ/C2H 独立 workloop | 扫描、关联、DHCP、绑定接口 HTTPS 通过；最终日常使用版本 |
| native-d4 | 512 帧 / 4 MiB 有界 RX 队列、每 action 最多 64 帧、native gate 序列化 | 标准接收架构试验，下载重传未解决，已回退 |
| BA-d5 | 解密后 BA/AMPDU 协商阶段的被动诊断 | 实机已测，发现 PMF/Action/ADDBA 线索，未作为当前使用版本 |

backend-d2 是独立诊断分支。主线补丁关系为 d1 → d3 → d4 → d5，当前使用 d3；后两项保留为实验实现。

## d3 处理的问题

completion 等待的超时分支曾在重新加锁后直接返回，遗留非递归锁。修正后各个返回路径都释放锁。

另一个相互等待来自原生 POWER 请求占有协议 gate，同时固件启动又等待由同一个 gate 约束的 IRQ/C2H 应答。把 IRQ/C2H 移至独立 workloop 后，上电期间的固件回复不再依赖该协议队列。

这两项一起部署后完成了联网验收。尚无实机逐项 A/B，不能把原始故障完全归因于其中一项。

## 接收队列与协议诊断

d4 的离线 ASan/UBSan/TSan 与缓冲区所有权测试通过。已保存的实机记录中，测试期间未出现新增队列满丢包、分配失败或 gate 违规，但启动阶段存在满队列丢包，下载方向仍有重传。接收架构调整尚未解决吞吐问题。

d5 在协议验证及解密后增加协商阶段诊断，抓到 PMF 已协商、管理帧保护位未启用、受保护 Action 丢弃及 ADDBA 超时线索。这些记录指导后续调查，完整 PMF/BA 修复仍未实施。测试中另有黑屏与图形恢复事件，确切因果没有被完整证明。

## 补丁链与验证范围

当前保留版本为 **d3**，按 d1 → d3 顺序应用补丁。d4/d5 可在独立源码目录继续叠加。实际依赖、固件、SDK、Bundle 版本及生成文件由对应构建流程提供。

本次整理在保存的上游源码包上重新检查了 d1 → d3 → d4 → d5 的逐次 `git apply --check` 和应用，结果见 [补丁重放证据](../evidence/wifi-patch-replay.json)。该检查覆盖补丁与源文件的匹配，未重新构建或运行无线驱动。

## 资料入口

- [上游来源](../archive/wifi/origin.json)
- [d1 诊断](../archive/wifi/rounds/d1/diagnosis.json)
- [d3 实机摘要](../archive/wifi/rounds/d3/runtime-summary.json)
- [d4 候选和队列参数](../archive/wifi/rounds/d4/candidate.json)
- [d5 被动诊断补丁](../archive/wifi/candidates/ba-d5/0001-passive-ba-negotiation-diagnostics.patch)
- [时序就绪等待修正](../archive/wifi/restore-d3/0001-wait-for-valid-framebuffer-timing-before-userland-switch.patch)

还需验证下载吞吐、重传、PMF/BA 兼容、漫游、长时间稳定、睡眠唤醒及 Wi‑Fi/蓝牙共存。AWDL、AirDrop 等功能需独立验收。
