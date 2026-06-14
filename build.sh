#!/bin/bash
set -e

# 1. 获取当前脚本所在目录与父目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# 2. 判断当前运行上下文是 BiTun 还是 BareTcl
# 如果父目录的 src 下没有 tcl_core.c，说明当前在 BiTun 目录下作为子模块运行
if [ ! -f "${PARENT_DIR}/src/tcl_core.c" ]; then
    echo "=== [BiTun Context] Redirecting build to BareTcl ==="
    
    # 确定 BareTcl 的目标目录（应当与 BiTun 平级）
    BARETCL_DIR="$(cd "${SCRIPT_DIR}/../../" && pwd)/BareTcl"
    
    if [ ! -d "${BARETCL_DIR}" ]; then
        echo "BareTcl repo not found at ${BARETCL_DIR}. Cloning from GitHub..."
        git clone https://github.com/potato-520/BareTcl.git "${BARETCL_DIR}"
    fi
    
    # 将 BiTun 的源码同步到 BareTcl 的 ESP-IDF 组件目录下
    # 这样 BareTcl 编译时就可以自动识别并链接 BiTun
    echo "=== Syncing BiTun sources to BareTcl components ==="
    BITUN_COMP_DIR="${BARETCL_DIR}/ESP32_ports/components/bitun"
    mkdir -p "${BITUN_COMP_DIR}"
    
    # 拷贝 BiTun 源码文件到组件中
    cp -r "${PARENT_DIR}/src"/* "${BITUN_COMP_DIR}/"
    
    # 为 BiTun 组件生成 CMakeLists.txt，使其符合 ESP-IDF 规范
    cat << 'EOF' > "${BITUN_COMP_DIR}/CMakeLists.txt"
# BiTun Component for ESP-IDF
idf_component_register(SRCS "tunnel.c" "socks5.c" "encrypt.c" "ikcp.c"
                    INCLUDE_DIRS "."
                    REQUIRES mbedtls)
EOF

    # 切换到 BareTcl 的 ESP32_ports 目录，调用其 build.sh 进行真实编译
    echo "=== Invoking build.sh in BareTcl ==="
    cd "${BARETCL_DIR}/ESP32_ports"
    ./build.sh
    exit 0
fi

# =============================================================
# 以下为 BareTcl 原生上下文编译逻辑
# =============================================================
echo "=== [BareTcl Context] Starting Build ==="

# 载入 ESP-IDF 环境变量
if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
    . "$HOME/esp/esp-idf/export.sh"
else
    echo "Error: Cannot find ESP-IDF export.sh at $HOME/esp/esp-idf/export.sh"
    exit 1
fi

# 确保在当前脚本所在目录
cd "${SCRIPT_DIR}"

# 编译 ESP32 自定义 Tcl 库为 C 文件，以及生成标准自举库
echo "=== Generating standard Tcl library ==="
python3 ../tools/tcl2c.py ../src/tcllib.tcl ../src/tcllib.c

echo "=== Generating ESP32 Tcl library ==="
python3 tcl2c_esp32.py esp32_lib.tcl main/esp32_lib.c esp32_bootstrap
python3 tcl2c_esp32.py console.html main/console_html.c console_html

# 执行编译
echo "=== Starting ESP-IDF Build ==="
idf.py build
echo "=== Build Completed Successfully ==="
