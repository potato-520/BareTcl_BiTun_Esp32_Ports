#!/bin/bash
set -e

# 1. 载入 ESP-IDF 环境变量
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    . "$HOME/esp/esp-idf/export.sh"
else
    echo "Error: Cannot find ESP-IDF export.sh at $HOME/esp/esp-idf/export.sh"
    exit 1
fi

# 2. 进入当前脚本所在目录
cd "$(dirname "$0")"

# 3. 执行清理
echo "=== Cleaning ESP-IDF Build Artifacts ==="
idf.py fullclean
echo "=== Clean Completed Successfully ==="
