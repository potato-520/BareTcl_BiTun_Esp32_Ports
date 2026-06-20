# BareTcl & BiTun ESP32 Port & Integration Project

This repository provides a native C port and integration of the **BareTcl** script interpreter and the **BiTun** secure network proxy tunnel on the ESP32 platform, built using the ESP-IDF framework.

With this project, you can run an ultra-lightweight, interactive Tcl command-line shell directly on your ESP32 chip. It enables hardware GPIO controls, network diagnostics (Ping/Ipconfig), non-volatile system configuration storage (NVS), and dynamic lifecycle management of the secure BiTun SOCKS5 proxy tunnel (powered by KCP multiplexing and ChaCha20-Poly1305 AEAD encryption).

---

## 1. Key Features

*   **Lightweight Tcl Engine (BareTcl)**:
    *   Zero external `malloc`/`free` allocations. The runtime is fully bounded within a static memory pool (Arena), making it exceptionally robust for resource-constrained MCU environments.
    *   Provides an interactive console shell supporting ANSI color terminal codes and non-blocking, character-level terminal inputs.
    *   Registers custom hardware commands (GPIO mode configuration, digital read/write), task sleep delays (`sleep`), logger verbosity controls (`log`), network interfaces status (`ipconfig`), and ICMP diagnostics (`ping`).
*   **Non-Volatile Key-Value Storage (NVS Wrapper)**:
    *   Wraps the ESP32 hardware NVS (Non-Volatile Storage) capabilities into straightforward Tcl commands: `nvs_set` and `nvs_get`.
    *   Persists core connectivity configurations (remote servers, ports, pre-shared key) across system reboots.
*   **Secure Network Tunneling (BiTun Port)**:
    *   Ports the BiTun secure UDP/KCP SOCKS5 proxy tunnel, featuring a robust ESP32/FreeRTOS implementation of the OS Abstraction Layer (OSAL).
    *   Uses **ChaCha20-Poly1305 AEAD** for in-place decryption and authenticating signature checks, providing highly robust packet-loss resilience and anti-tampering protection.
    *   Optimizes RAM usage and scheduler overhead through a global single-threaded asynchronous DNS resolver and eventfd-driven wakeups.
*   **Dynamic Tcl-Based Lifecycle Management**:
    *   Manages SOCKS5 proxy execution dynamically via `bitun_start` and `bitun_stop` Tcl console commands.
    *   Retrieves the latest server credentials and keys directly from NVS on startup, allowing remote endpoint adjustments without compiling or re-flashing the firmware.

---

## 2. Project Directory Structure

```text
├── assets/
│   ├── BareTcl/          # Submodule pointing to the upstream BareTcl repository
│   └── BiTun/            # Submodule pointing to the upstream BiTun repository
├── components/
│   └── bitun_wrapper/
│       ├── CMakeLists.txt
│       └── bitun_osal_esp32.c # Complete ESP32/FreeRTOS OSAL adapter for BiTun
├── main/
│   ├── CMakeLists.txt
│   ├── main.c            # App entrypoint, custom Tcl command registrations, and bootstrap logic
│   ├── console_html.c    # Compiled bytecode array for the web-based console UI
│   └── esp32_lib.c       # Tcl bootstrap application scripts packed as a C byte array
├── CMakeLists.txt
├── build.sh              # Unified script to package Tcl code and compile the ESP32 firmware
├── LICENSE               # Apache 2.0 Open-Source License
└── README.md             # Project documentation (Simplified Chinese)
```

---

## 3. Quick Start

### 3.1 Prerequisites
*   ESP-IDF v5.0 or higher (We recommend using **ESP-IDF v5.3**).
*   CMake and Ninja build tools.
*   Python 3.x.

### 3.2 Building and Flashing
We provide a unified shell script `build.sh` that compiles the Tcl bootstrap scripts into C byte arrays and builds the ESP-IDF project:

```bash
# Grant execution permissions
chmod +x build.sh

# Run the build script
./build.sh
```

Once the compilation succeeds, you can flash the binary to your chip (e.g., ESP32-C3) and start the console monitor:
```bash
idf.py -p <YOUR_SERIAL_PORT> flash monitor
```

---

## 4. Tcl Commands Manual

Upon entering the interactive Tcl Shell (accessible via standard UART terminal or the built-in WebSocket Web Console), you can utilize the following custom commands:

### 4.1 Hardware & System Control
*   **`gpio_mode <pin> <mode>`**: Configures the hardware pin mode.
    *   `mode`: `0` = Input, `1` = Output, `2` = Input with internal pull-up resistor.
*   **`digital_write <pin> <level>`**: Writes a logic level to an output pin. `level` must be `0` or `1`.
*   **`digital_read <pin>`**: Reads the current logic state of a pin. Returns `0` or `1`.
*   **`sleep <ms>`**: Pauses execution for the specified milliseconds (internally yields the FreeRTOS task).
*   **`log <on|off>`**: Dynamically toggles background Wi-Fi and system logs on/off to keep the Tcl console output clean and readable.

### 4.2 Network Diagnostics
*   **`ipconfig`**: Displays current Wi-Fi status, local station IP address, subnet mask, and gateway.
*   **`ping <host>`**: Sends ICMP Echo requests to a target IP or domain, printing standard Round-Trip Time (RTT) results.

### 4.3 Non-Volatile Storage (NVS)
*   **`nvs_set <key> <value>`**: Writes a persistent string key-value pair to NVS.
    *   *Note*: The `key` length must not exceed 15 bytes.
*   **`nvs_get <key> ?default?`**: Retrieves a string value from NVS.
    *   Returns the optional `default` value if the key does not exist (or an empty string `""` if omitted).

### 4.4 BiTun Tunnel Lifecycle Controls
*   **`bitun_start`**: Starts the BiTun proxy tunnel asynchronously in the background.
    *   Reads configuration keys dynamically from the NVS namespace `"storage"` (falls back to defaults if not found):
        *   `bitun_rem_ip`: Remote KCP server IP address or domain name (Default: `"127.0.0.1"`).
        *   `bitun_rem_port`: Remote KCP server listening port (Default: `9999`).
        *   `bitun_loc_port`: Local SOCKS5 proxy port (Default: `1080`).
        *   `bitun_psk`: 32-byte pre-shared key string (Default: `"00000000000000000000000000000000"`).
*   **`bitun_stop`**: Stops the background BiTun tunnel loop and cleans up all active network sockets and task allocations.

---

## 5. Usage Example

To connect to a remote server, execute the following commands in the Tcl console:

```tcl
# 1. Store the remote server configurations and pre-shared key in NVS
nvs_set bitun_rem_ip "your-server-domain.com"
nvs_set bitun_rem_port "8888"
nvs_set bitun_psk "abcdefghijklmnopqrstuvwxyz123456"

# 2. Launch the tunnel proxy
bitun_start

# 3. Local network devices can now configure the ESP32's IP:1080 as a SOCKS5 proxy for secure data transmission.

# 4. Stop the tunnel whenever necessary
bitun_stop
```

---

## 6. Web Shell Console

This project features a built-in WebSocket-based interactive Web Tcl Console, which makes cross-platform debugging and dynamic configurations incredibly easy.

*   **Default Wi-Fi Credentials**:
    *   **SSID**: `"testzzzz"`
    *   **Password**: `"11111111"`
*   **How to Access**:
    1. Once the ESP32 is successfully connected to your Wi-Fi router, check the serial monitor to retrieve the assigned LAN IP address (e.g., `192.168.1.100`).
    2. Open a web browser on any device in the same local network, and navigate to `http://<ESP32_IP>/` to open the built-in interactive console.
    3. Type any Tcl command in the input box and press Enter. The command will be sent in real-time to the ESP32 via the `/ws` WebSocket route, and execution output will be streamed back and displayed on the web page asynchronously.

---

## 7. License

This project is licensed under the **[Apache License 2.0](LICENSE)**.  
For details, please refer to the `LICENSE` file located at the root of this repository.

