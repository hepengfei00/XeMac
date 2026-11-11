# 验证范围与结果

本页区分实机功能验证、补丁重放和仓库检查。实机结果来自适配阶段保存的记录；公开仓库的文件检查与补丁重放结果独立列出。

## 实机功能验证

测试条件为 [平台说明](HARDWARE.md) 中的单 DP 环境，系统 Build 为 `25G332`。

| 对象 | 检查方法 | 结果 | 覆盖限制 |
| --- | --- | --- | --- |
| GPU 初始化 | 检查实际加载模块及初始化阶段 | stage 7、accel 2 | 未完成长期负载测试 |
| 原生显示 | 读取当前物理输出、时序和刷新率 | 3840×2160 @ 60 Hz | 单 DP、单显示器 SST |
| HiDPI | 在 GUI 会话中读取物理像素、逻辑尺寸和 HiDPI 标志 | 1920×1080 逻辑尺寸、2× 缩放 | 其他缩放档位待测 |
| Metal | 计算、绘制及缓冲区纹理别名探针 | 目标探针通过 | 第三方 App 兼容性尚不完整 |
| 桌面接管 | 检查 WindowServer 替换及切换后的实时显示状态 | 完成切换并通过健康检查 | 登录阶段偶有短暂黑块 |
| Wi-Fi d3 | 扫描、关联、DHCP、绑定接口 HTTPS | 基础联网通过 | 下载吞吐与重传异常未解决 |
| 蓝牙 d2 | 控制器状态、配对、音箱连接 | 控制器 On，音频连接可建立 | 长播、麦克风与睡眠恢复待测 |
| CPU 温度 | 读取 SMC 键与变化数值 | 官方 SMCProcessor 1.3.7 可读取 | 无独立温度校准或持续负载对照 |

图形验证依次覆盖模块加载、原生模式、Metal 探针、桌面合成和 HiDPI。显示查询来自实际 GUI 会话，同时核对物理输出、逻辑尺寸和刷新率。

相关摘要：

- [显示与 Metal 验证摘要](../archive/graphics/evidence/display-audit.json)
- [Wi-Fi d3 联网结果](../archive/wifi/rounds/d3/runtime-summary.json)
- [蓝牙运行结果](../archive/bluetooth/native-fw1/scan-compat/postboot-user-accepted.json)
- [蓝牙配置保存结果](../archive/bluetooth/native-fw1/accepted-save/save-result.json)

README 的控制中心截图用于展示实机界面。输出模式、驱动状态与功能结论以对应运行查询为依据。

## 2026-11-11 启动恢复验证

适配快照 `a2bf4d4` 在一次用户手动重启后，通过独立 GUI 查询的当前启动 UUID、nonce 和生成时间检查，确认物理 3840×2160@60、逻辑 1920×1080、2× HiDPI。GPU stage7/accel2，送帧计数两秒内从 812 增至 814；健康检查与下次原生启动配置提交通过，恢复状态为 healthy，额度为 1，56 项保护文件保持一致。

新桌面切换助手记录开始、5 秒心跳与 exit0；本次没有走“已健康则跳过切换”分支。状态转换、准备事务与切换失败场景有主机模拟测试；未在正常桌面故意制造失败。一次成功启动不能证明此前偶发接管中断已根治。

见 [实机摘要](../evidence/native-boot-20261111.json)、[来源与哈希](../evidence/adaptation-a2bf4d4.json) 和 [流程及隔离测试](../archive/boot/recovery-20261111/README.md)。本次公开发布只重新运行仓库检查与隔离状态测试，没有部署或操作目标机器。

## 补丁重放

| 范围 | 方法 | 结果 |
| --- | --- | --- |
| Wi-Fi d1 → d3 → d4 → d5 | 在保存的上游源文件上逐次执行 `git apply --check` 与应用 | 通过 |
| Realtek 蓝牙名称修正 | 对固定基点源文件检查并应用 | 通过 |
| BlueToolFixup EMP2 → Scan 718 | 在对应目录顺序检查并应用 | 通过 |
| 非空补丁格式 | 使用 `git apply --numstat` 解析 | 56 项通过 |

重放记录见 [Wi-Fi](../evidence/wifi-patch-replay.json)、[蓝牙](../evidence/bluetooth-patch-replay.json) 和 [补丁格式](../evidence/patch-format.json)。另有一项空对比文件，表示对应源码未发生修改。

图形整栈的构建和运行结果沿用适配阶段记录；当前公开快照未重新完成从空环境开始的整栈构建。Wi-Fi 与蓝牙的补丁重放检查覆盖源文件匹配，驱动编译和硬件运行需按组件说明继续执行。

## 仓库检查

在仓库内运行：

```sh
python3 tools/verify-repository.py
```

检查范围包括 JSON/plist 解析、Python/shell 语法、当前文档相对链接与图片文件格式。

检查提交版本与工作区字节一致性：

```sh
python3 tools/verify-repository.py --git
```

该选项应在提交后运行。以上检查均在本地读取文件，完整构建与实机验证属于独立步骤。

## 后续验证

优先补充 Wi-Fi 吞吐与重传、登录阶段绘制异常和持续图形负载测试。睡眠唤醒、HDMI/USB-C、多显示器、DRM、视频硬件解码及更多 App 的 Metal 路径仍需单独验证。

新增结果应注明系统构建、组件版本、连接方式、复现步骤和通过条件，格式见 [贡献指南](../CONTRIBUTING.md)。
