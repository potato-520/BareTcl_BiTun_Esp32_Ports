# BareTcl & BiTun ESP32 移植整合项目

本项目是 **BareTcl** 脚本解释器与 **BiTun** 安全网络隧道代理在 ESP32（基于 ESP-IDF 平台）上的原生 C 语言移植与整合项目。

通过本项目，您可以在 ESP32 芯片上运行一个超轻量级的 Tcl 交互式命令行环境，实现 GPIO 硬件控制、网络检测（Ping/Ipconfig），并能通过 Tcl 命令行动态读取 NVS 配置并拉起/停止安全的 BiTun SOCKS5 代理隧道（基于 KCP 和 ChaCha20-Poly1305 加密）。

---

## 1. 核心特性 (Key Features)

*   **轻量级 Tcl 引擎 (BareTcl)**：
    *   零外部 `malloc`/`free` 依赖，完全限定在固定的静态内存池（Arena）内，极度适合嵌入式资源约束环境。
    *   提供交互式控制台 Shell，支持 ANSI 彩色终端控制及字符级非阻塞输入。
    *   集成 GPIO 控制（设置模式、数字读写）、延时（sleep）、系统日志打印级别（log）、网络状态查看（ipconfig）及 ICMP Ping 命令。
*   **非易失 Key-Value 存储 (NVS 封装)**：
    *   将 ESP32 NVS（Non-Volatile Storage）读写能力直接封装为 Tcl 指令：`nvs_set` 和 `nvs_get`。
    *   支持网络配置（如远程服务器域名、连接端口、预共享密钥等）的断电持久化。
*   **安全网络隧道 (BiTun 移植)**：
    *   移植了 BiTun UDP/KCP 安全网络隧道代理，实现了 ESP32 下的完整 OS 抽象层（OSAL）。
    *   采用 **ChaCha20-Poly1305 AEAD** 对网络传输进行原位解密与签名验证，具备高抗丢包与防篡改能力。
    *   通过全局单线程 DNS 解析器 and `eventfd` 唤醒队列，保障了低 SRAM 占用和高效的 FreeRTOS 任务调度。
*   **Tcl 动态控制与配置**：
    *   可通过 Tcl 扩展命令 `bitun_start` 和 `bitun_stop` 后台动态启停安全代理。
    *   隧道启动时会自动从 NVS 中读取最新的服务器和密钥配置，无需重新烧录固件即可实现动态服务器切换。

---

## 2. 目录结构 (Project Directory Structure)

```text
├── assets/
│   ├── BareTcl/          # BareTcl 官方源仓库子模块 (assets/BareTcl)
│   └── BiTun/            # BiTun 官方源仓库子模块 (assets/BiTun)
├── components/
│   └── bitun_wrapper/
│       ├── CMakeLists.txt
│       └── bitun_osal_esp32.c # 为 BiTun 编写的完整 ESP32/FreeRTOS 适配层
├── main/
│   ├── CMakeLists.txt
│   ├── main.c            # 项目主入口、Tcl 命令定义与自举逻辑
│   ├── console_html.c    # Web 控制台前端字节数组
│   └── esp32_lib.c       # Tcl 自定义脚本打包生成的字节数组
├── CMakeLists.txt
├── build.sh              # 一键生成 Tcl 字节代码并编译固件的脚本
├── LICENSE               # Apache 2.0 开源许可协议
└── README.md             # 本说明文档 (简体中文)
```

---

## 3. 快速开始 (Quick Start)

### 3.1 环境要求
*   ESP-IDF v5.0 或以上版本（推荐使用 **ESP-IDF v5.3**）。
*   CMake 与 Ninja 构建工具。
*   Python 3.x。

### 3.2 编译与烧录
我们提供了一个自动打包 Tcl 自举脚本并编译项目的脚本 `build.sh`：

```bash
# 赋予执行权限
chmod +x build.sh

# 一键自举转换并编译项目
./build.sh
```

编译完成后，可以使用 ESP-IDF 命令进行烧录（以 ESP32-C3 芯片为例）：
```bash
idf.py -p <您的串口号> flash monitor
```

---

## 4. Tcl 扩展命令手册 (Tcl Commands Manual)

进入 Tcl Shell 交互环境（支持串口终端或内置网页 WebSocket 控制台）后，可使用以下定制的扩展命令：

### 4.1 硬件与系统控制
*   **`gpio_mode <pin> <mode>`**：设置引脚模式。
    *   `mode`: `0`=输入, `1`=输出, `2`=带上拉输入。
*   **`digital_write <pin> <level>`**：写数字电平。`level` 为 `0` 或 `1`。
*   **`digital_read <pin>`**：读取引脚数字电平，返回 `0` 或 `1`。
*   **`sleep <ms>`**：延迟，单位为毫秒（内部挂起 FreeRTOS 任务以出让 CPU）。
*   **`log <on|off>`**：开启/关闭底层 Wi-Fi 和内核的冗余调试 Log 输出，防止干扰 Tcl 控制台的可读性。

### 4.2 网络检测
*   **`ipconfig`**：查看当前 Wi-Fi 连接状态、局域网 IP 地址、掩码和网关。
*   **`ping <host>`**：向指定 IP 或域名发送 ICMP 请求，并输出 RTT 时间。

### 4.3 NVS 存储配置 (断电保存)
*   **`nvs_set <key> <value>`**：向 NVS 写入持久化字符串。
    *   *注*：`key` 的最大长度不能超过 15 字节。
*   **`nvs_get <key> ?default?`**：读取 NVS 中的字符串。
    *   如果 key 不存在，返回可选参数 `default`（未填则默认为空字符串 `""`）。

### 4.4 BiTun 隧道动态控制
*   **`bitun_start`**：异步启动 BiTun 隧道。
    *   启动时会自动从 NVS 中读取以下配置，若不存在则降级为默认值：
        *   `bitun_rem_ip`：远程 KCP 服务器 IP 或域名（默认 `"127.0.0.1"`）。
        *   `bitun_rem_port`：远程 KCP 服务器端口（默认 `9999`）。
        *   `bitun_loc_port`：本地 SOCKS5 代理端口（默认 `1080`）。
        *   `bitun_psk`：32 字节预共享密钥 PSK（默认 `"00000000000000000000000000000000"`）。
*   **`bitun_stop`**：停止后台的 BiTun 隧道任务，并彻底清理占用的网络和内存资源。

---

## 5. 使用示例 (Configuration Example)

要设置远端连接目标服务器，您只需在 Tcl 环境下执行：

```tcl
# 1. 设定远端 KCP 代理服务器地址与密钥
nvs_set bitun_rem_ip "10.0.0.5"
nvs_set bitun_rem_port "8888"
nvs_set bitun_psk "abcdefghijklmnopqrstuvwxyz123456"

# 2. 启动隧道
bitun_start

# 3. 此时局域网内设备可以将 ESP32 的 IP:1080 设置为 SOCKS5 代理，实现安全数据转发。

# 4. 如果需要停止隧道
bitun_stop
```

---

## 6. Web Shell 网页控制台 (Web Shell Console)

本项目内置了一个基于 WebSocket 的网页交互式 Tcl 控制台，极大地方便了跨平台调试和动态配置。

*   **默认 Wi-Fi 连接配置**：
    *   **SSID**: `"testzzzz"`
    *   **Password**: `"11111111"`
*   **访问方式**：
    1. ESP32 成功连上您的 Wi-Fi 路由后，会在串口终端（Monitor）打印获取到的局域网 IP（例如 `192.168.1.100`）。
    2. 打开同一局域网内的浏览器，访问 `http://<ESP32_IP>/` 即可打开内置的网页交互控制台。
    3. 在网页文本框中输入 Tcl 命令并回车，命令会通过 `/ws` WebSocket 路由实时发送至 ESP32，且执行结果会即时异步推流回显在网页上。

---

## 7. 开源许可 (License)

本项目采用 **[Apache License 2.0](LICENSE)** 协议开源。
详情请参阅项目根目录下的 `LICENSE` 文件。

