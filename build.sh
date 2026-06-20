#!/bin/bash
set -e

# 1. 获取当前脚本所在目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

# 2. 载入 ESP-IDF 环境变量
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    . "$HOME/esp/esp-idf/export.sh"
else
    echo "Error: Cannot find ESP-IDF export.sh at $HOME/esp/esp-idf/export.sh"
    exit 1
fi

# 3. 编译 ESP32 自定义 Tcl 库为 C 文件，以及生成 standard 自举库
echo "=== Generating standard Tcl library from submodule ==="
python3 assets/BareTcl/tools/tcl2c.py assets/BareTcl/src/tcllib.tcl assets/BareTcl/src/tcllib.c

echo "=== Generating ESP32 Tcl library ==="
python3 tools/tcl2c_esp32.py main/esp32_lib.tcl main/esp32_lib.c esp32_bootstrap
python3 tools/tcl2c_esp32.py main/console.html main/console_html.c console_html

# 4. 执行编译
echo "=== Starting ESP-IDF Build ==="
idf.py build
echo "=== Build Completed Successfully ==="
