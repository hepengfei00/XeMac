#!/bin/bash
set -euo pipefail
printf '这会准备下一次开机的 4K 核显重试，不会立即重启。\n请在本窗口输入这台黑苹果的登录密码。\n'
sudo /bin/bash '/Library/Application Support/WTLArrowTiming.li8Pgu/BootPolicy.1/manager.sh' manual
printf '\n请查看本目录的“状态.txt”。保存工作后再正常重启。\n'
read -r -p '按回车关闭窗口。' ignored
