# 源码、补丁与构建依赖

## 交付范围与环境

仓库提供固定基点、全部已归档的补丁、关键修改后源码和验证记录。完整上游源码、固件、SDK、Mesa/LLVM 构建输出、辅助内核缓存和本机完整 EFI 需分别准备。`archive/graphics/source` 只含选定文件，Wi‑Fi 完整源码从固定上游取得。

以下命令用于准备独立源码目录。请先进入本仓库，再设置 `REPO`；后续示例使用该变量定位补丁。构建需要 Xcode Command Line Tools、匹配的 SDK 和各上游文档列出的依赖。图形构建目标为 x86_64，Apple Silicon 宿主也须检查产物架构与目标 KPI。

```sh
REPO="$(git rev-parse --show-toplevel)"
mkdir -p "$REPO/build-work"
cd "$REPO/build-work"
```

`build-work/` 已由 `.gitignore` 排除。

## 图形源码基点

| 树 | 提交 |
| --- | --- |
| ArrowWhatever 参考 | 555fbb2f3c29a1a1b2c5a298ee2107222d28baf4 |
| NvGfx `xe` core/display/accel/AGDC | 5a19a73f36249efb99c44c3f9cb34603ef0c0eb8 |
| NvGfx XeMTL plugin | 497d521e925bd760c8cde5bbc17d9bf96d2ee2b4 |
| Mesa | bffd91947efc1932da99aca69fc540d6d4201b0c |
| libdrm | b97cbde15c5c3abfe44d78e8f57139e50f612fec |
| FreeBSD | 6b38db44f0ea36e8336758a460615e9c52def28c |
| drm-kmod | 原记录为 drm-kmod-c16de0f，短提交标识 |

历史 NvGfx/XeGfx 仓库可能重命名或跳转，应先确认目标仓库确实包含所列提交。不要在不同源码版本上直接强行应用补丁。

### 分组件应用

ArrowWhatever 0001–0037 含多个独立源码树。core 和显示树、plugin、Mesa、libdrm 有不同基点，各自按 [上游补丁说明](../archive/graphics/reference/patches/README.md) 的对应链重放。文件中的路径为历史参考，应替换为本机目录。

core/display/accel/AGDC/tools 应用上游 29 个补丁，顺序为 `0001 0002 0003 0004 0008 0009 0010 0011 0012 0013 0014 0015 0016 0017 0018 0019 0020 0021 0022 0023 0024 0025 0027 0028 0029 0030 0034 0035 0036`。

plugin 应用 `0005 0006 0007 0026 0031 0037`。Mesa、libdrm 先应用 NvGfx 提供的 Darwin/Xe 移植补丁，再分别应用 0033 和 0032。

本地补丁按以下源码树分组，并在各组内保持编号顺序：

| 目标源码树 | 本地补丁 | 应用目录 |
| --- | --- | --- |
| core / display / KPI | `0038 → 0039 → 0040 → 0041 → 0043 → 0045` | core 基点源码根目录 |
| XeMTL plugin | `0042 → 0044` | plugin 基点源码根目录，包含 `plugin/` 子目录 |
| WTLArrowEarlyClaim | `0046` | 包含 `early-claim/` 的配套源码根目录 |

先读取补丁头和 `--- / +++` 路径，再在目标树执行 `git apply --check` 与应用。`0040` 修改核心及 Display 的启用条件，`0046` 修改独立 EarlyClaim 组件，二者应用位置不同。路径或上下文不匹配时，应重新核对基点与前置补丁。

已有记录覆盖 0038 重放及后续组件构建。当前公开快照尚未重新完成从空环境开始的图形整栈构建；其复现状态与实机结果分别列于 [验证记录](VALIDATION.md)。

### 依赖与运行库

最终需要一致的 XeGfx、Display、Accel、AGDC、EarlyClaim、XeMTL、Mesa ANV、libdrm、translator、AIR/LLVM 及其他链接依赖。运行库缺失曾造成 Metal 探针失败或桌面无法接管。构建成功后须检查 `file`、`otool -L`、Bundle 信息、KPI 导入、Mach-O UUID、签名与 SHA256，再在匹配系统构建的隔离环境生成 AuxKC。

实际使用的上游构建脚本和依赖声明见固定版本 ArrowWhatever/NvGfx。各衍生组件的许可证与固件条款应随构建记录保存。

## Wi‑Fi

```sh
cd "$REPO/build-work"
git clone https://github.com/souissiak1-maker/AirportRTW.git AirportRTW
cd AirportRTW
git checkout --detach 763d0dfcf4871c892bd00fe3dad865e81a53be4d
git apply --check "$REPO/archive/wifi/rounds/d1/0001-release-stage-diagnostics.patch"
git apply "$REPO/archive/wifi/rounds/d1/0001-release-stage-diagnostics.patch"
git apply --check "$REPO/archive/wifi/rounds/d3/0001-native-irq-and-completion-lock.patch"
git apply "$REPO/archive/wifi/rounds/d3/0001-native-irq-and-completion-lock.patch"
```

上面生成 d3 的功能修改基底。Bundle 版本、生成的固件源、SDK 与构建参数仍需按上游说明和归档 Makefile/打包记录核对。该补丁链已在保存的上游源文件上重新重放通过。

研究 d4 时在 d3 后应用 `archive/wifi/rounds/d4/0001-native-rx-ownership-serialization-batching.patch`。研究 d5 时再应用 `archive/wifi/candidates/ba-d5/0001-passive-ba-negotiation-diagnostics.patch`。backend-d2 是独立实验，避免串入日常 d3 链。

源码以 AirportRTW 原许可为准，固件与生成文件从其上游取得。公开仓库的补丁足以重放已保存的修改，构建前仍应复核固件和 SDK 是否为原版本。

## 蓝牙

```sh
cd "$REPO/build-work"
git clone https://github.com/thegwchr/RealtekBluetoothFirmware.git RealtekBluetoothFirmware
cd RealtekBluetoothFirmware
git checkout --detach 22e501a8a0a7aba8bb4e594bc6f40b1a67391af9
git apply --check "$REPO/archive/bluetooth/native-fw1/identity-fix/0001-preserve-rtl8922a-usb-name.patch"
git apply "$REPO/archive/bluetooth/native-fw1/identity-fix/0001-preserve-rtl8922a-usb-name.patch"
```

BlueToolFixup 从 BrcmPatchRAM 2.7.2 构建。EMP2 补丁的路径带 `BrcmPatchRAM/` 前缀，扫描补丁的路径直接为 `BlueToolFixup.cpp`，因此应用目录不同。按以下顺序分别检查和应用，所有目标模式匹配及范围限制须保留。

```sh
cd "$REPO/build-work"
git clone https://github.com/acidanthera/BrcmPatchRAM.git BrcmPatchRAM-source
cd BrcmPatchRAM-source
git checkout --detach 2.7.2
git apply --check "$REPO/archive/bluetooth/native-fw1/hci-compat/0001-rtl8922a-25g332-emp2-only.patch"
git apply "$REPO/archive/bluetooth/native-fw1/hci-compat/0001-rtl8922a-25g332-emp2-only.patch"
cd BrcmPatchRAM
git apply --check "$REPO/archive/bluetooth/native-fw1/scan-compat/0002-rtl8922a-scan718-final.patch"
git apply "$REPO/archive/bluetooth/native-fw1/scan-compat/0002-rtl8922a-scan718-final.patch"
```

本次在保存的原始源文件上重新检查了 Realtek 名称、EMP2、扫描三项补丁并逐次应用通过，见 [蓝牙补丁重放](../evidence/bluetooth-patch-replay.json)。

## 温度

SMCProcessor 使用 VirtualSMC 1.3.7 官方原版，无本地传感器二进制修改。归档保存 1.3.7 / 1.3.8 源码对照，用于解释兼容性。部署时放在 VirtualSMC 后，记录官方包摘要与目标 EFI 差异。

## 工具与部署参考

`tools/verify-repository.py` 为公开仓库完整性检查，`tools/read-only-status.sh` 为本机只读信息概要。`archive` 的部署参考脚本含示例机器常量，移植时须重新核对目标路径、配置条件和配套文件。

编译检查、宿主 sanitizer、当前进程热加载、一次真实重启和长期稳定是不同验收层级，结果分别写入记录。
