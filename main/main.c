#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_system.h"
#include <fcntl.h>
#include "driver/uart_vfs.h"
#include <termios.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "ping/ping_sock.h"
#include <netdb.h>
#include <sys/socket.h>

// Ping 结果统计结构体
typedef struct {
    SemaphoreHandle_t sem;
    volatile int success_cnt;
    volatile int total_cnt;
    volatile int last_rtt;
} ping_result_t;

// -------------------------------------------------------------
// BareTcl 引擎整合定义与钩子
// -------------------------------------------------------------
// TCL_YIELD_HOOK: 状态机执行周期中调用的看门狗让出钩子。
// 设定每 2000 次 Tcl 状态机迭代（以防长耗时/无限循环脚本独占 CPU），
// 强制调用 vTaskDelay(1) 挂起当前任务 1 个 Tick 周期，
// 从而让出 CPU 给低优先级的 IDLE 任务去喂看门狗（TWDT），彻底解决 WDT 饥饿复位警告。
#define TCL_YIELD_HOOK() do { \
    static uint32_t __yield_cnt = 0; \
    if (++__yield_cnt >= 2000) { \
        __yield_cnt = 0; \
        vTaskDelay(1); \
    } \
} while(0)

// 引入 BareTcl 核心源码与扩展模块
#include "../../src/tcl_core.c"       // Tcl 核心解释器与内存管理核心
#include "../../src/extcmd.c"         // Tcl 核心扩展命令实现
#include "../../src/baretcl_shell.c"  // Tcl 交互式命令行行编辑器实现
#include "esp32_lib.c"                // 由 esp32_lib.tcl 自动编译转换而成的 ESP32 自定义库字节数组
#include "console_html.c"             // 由 console.html 自动编译转换而成的控制台 HTML 网页字节数组
#include "freertos/queue.h"

static QueueHandle_t input_queue = NULL;
static int ws_active_fd = -1;
static httpd_handle_t ws_server_handle = NULL;
static SemaphoreHandle_t ws_mutex = NULL;

static void ws_send_text(const char *text, size_t len) {
    if (ws_mutex == NULL) return;
    if (xSemaphoreTake(ws_mutex, portMAX_DELAY) == pdTRUE) {
        if (ws_server_handle != NULL && ws_active_fd != -1) {
            httpd_ws_frame_t ws_pkt = {
                .payload = (uint8_t *)text,
                .len = len,
                .type = HTTPD_WS_TYPE_TEXT
            };
            httpd_ws_send_frame_async(ws_server_handle, ws_active_fd, &ws_pkt);
        }
        xSemaphoreGive(ws_mutex);
    }
}

// GPIO 控制 Tcl 扩展命令函数的 C 前置声明
tcl_i32 tcl_cmd_gpio_mode(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);
tcl_i32 tcl_cmd_digital_write(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);
tcl_i32 tcl_cmd_digital_read(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);
tcl_i32 tcl_cmd_tcl_shell_ansi(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);
tcl_i32 tcl_cmd_log(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);
tcl_i32 tcl_cmd_ipconfig(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);
tcl_i32 tcl_cmd_ping(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);
tcl_i32 tcl_cmd_sleep(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values);

// -------------------------------------------------------------
// 物理内存池（Arena）布局定义
// -------------------------------------------------------------
// 定义 Tcl 运行时所需的静态物理内存池，大小为 48KB。
// BareTcl 所有对象的分配与垃圾回收（GC）均限制在此空间内，零外部 malloc/free。
#define TCL_ARENA_SIZE (48 * 1024)
static char tcl_arena[TCL_ARENA_SIZE];

// Tcl 硬件抽象层输出重定向函数
// 当 BareTcl 需要向控制台打印输出（如 puts）时，底层直接调用此 C 接口。
void tcl_hal_puts(const tcl_u8 *s) {
    printf("%s", (const char *)s); // 重定向到标准输出
    fflush(stdout);                // 强制刷空 stdout 缓冲区确保即时输出
    ws_send_text((const char *)s, strlen((const char *)s));
}

// -------------------------------------------------------------
// 硬件接口引脚映射与多任务共享状态
// -------------------------------------------------------------
static SemaphoreHandle_t relayMutex = NULL; // 互斥锁，用于跨任务互斥安全地访问继电器状态
volatile bool tcl_task_running = false;     // Tcl 任务正在运行的标志位
volatile int tcl_task_create_res = -99;     // Tcl 任务创建的返回值记录
volatile bool enable_log_print = false;     // 指示是否开启详细的调试日志打印
volatile bool log_explicitly_set = false;   // 标记用户是否显式指定了 log 的开关状态

// 硬件板卡引脚定义：4路输入按键引脚、4路低电平触发继电器输出引脚
const gpio_num_t buttonPins[4] = {GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_6, GPIO_NUM_8};
const gpio_num_t relayPins[4] = {GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_7};

static uint64_t relayOnMillis[4] = {0}; // 记录每个继电器最近一次被开启（拉低电平）的时间戳（毫秒）
static volatile bool relayOn[4] = {false}; // 标记每个继电器当前是否处于激活（开启）状态

// -------------------------------------------------------------
// Tcl 硬件扩展命令接口实现（供 Tcl 脚本直接调用控制 GPIO）
// -------------------------------------------------------------

// Tcl 指令: gpio_mode <pin> <mode>
// 设置 GPIO 的物理模式。mode 参数: 0=输入模式, 1=输出模式, 2=带上拉输入模式。
tcl_i32 tcl_cmd_gpio_mode(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    if (arg_count < 3) return TCL_ERROR; // 参数检查
    int pin = atoi((const char *)TO_PTR(context, arg_values[1]));
    int mode = atoi((const char *)TO_PTR(context, arg_values[2]));
    
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << pin),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    if (mode == 0) { // INPUT
        io_conf.mode = GPIO_MODE_INPUT;
    } else if (mode == 1) { // OUTPUT
        io_conf.mode = GPIO_MODE_OUTPUT;
    } else if (mode == 2) { // INPUT_PULLUP
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    } else {
        return TCL_ERROR;
    }
    gpio_config(&io_conf); // 配置 GPIO 寄存器
    return TCL_OK;
}

// Tcl 指令: digital_write <pin> <level>
// 输出指定 GPIO 引脚的物理电平。level 参数: 0=低电平, 1=高电平。
// 当对继电器所映射的引脚写入 0（低电平有效激活）时，自动更新激活定时器用于延时自锁。
tcl_i32 tcl_cmd_digital_write(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    if (arg_count < 3) return TCL_ERROR;
    int pin = atoi((const char *)TO_PTR(context, arg_values[1]));
    int val = atoi((const char *)TO_PTR(context, arg_values[2]));
    
    gpio_set_level(pin, val); // 写入硬件物理电平
    
    // 如果修改了继电器引脚，在互斥锁保护下同步更新它的激活状态与开启毫秒时间戳
    if (xSemaphoreTake(relayMutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < 4; i++) {
            if (relayPins[i] == pin) {
                if (val == 0) { // 低电平代表开启继电器
                    relayOn[i] = true;
                    relayOnMillis[i] = esp_timer_get_time() / 1000;
                } else {
                    relayOn[i] = false;
                }
            }
        }
        xSemaphoreGive(relayMutex);
    }
    return TCL_OK;
}

// Tcl 指令: digital_read <pin>
// 读取指定 GPIO 引脚的物理输入电平值，返回 0 或 1 并赋予 Tcl 解释器的 result 寄存器。
tcl_i32 tcl_cmd_digital_read(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    if (arg_count < 2) return TCL_ERROR;
    int pin = atoi((const char *)TO_PTR(context, arg_values[1]));
    int val = gpio_get_level(pin); // 读取硬件引脚物理输入
    
    // 在静态 Arena 中分配 12 字节存储结果字符串，并写入解释器 result 寄存器返回给 Tcl 空间
    tcl_u32 res_offset = tcl_alc_p(context, 12);
    if (res_offset != TCL_NULL) {
        itoa(val, (char *)TO_PTR(context, res_offset), 10);
        context->result = res_offset;
    }
    return TCL_OK;
}

// Tcl 指令: tcl_shell_ansi <0|1>
// 动态切换交互式控制台的 ANSI 转义着色支持（0 为关闭，1 为开启复位/擦除/提示符高亮）。
tcl_i32 tcl_cmd_tcl_shell_ansi(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    if (arg_count < 2) return TCL_ERROR;
    int val = atoi((const char *)TO_PTR(context, arg_values[1]));
    baretcl_use_ansi = val;
    return TCL_OK;
}

// Ping 成功接收 Echo 响应回调
static void ping_on_success(esp_ping_handle_t hdl, void *args) {
    ping_result_t *res = (ping_result_t *)args;
    uint32_t elapsed_time, recv_len;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    res->success_cnt++;
    res->last_rtt = elapsed_time;
    char out_buf[128];
    snprintf(out_buf, sizeof(out_buf), "%ld bytes from " IPSTR ": icmp_seq=%d time=%ld ms\r\n", 
             (long)recv_len, IP2STR(&target_addr.u_addr.ip4), res->total_cnt, (long)elapsed_time);
    tcl_hal_puts((const tcl_u8 *)out_buf);
    res->total_cnt++;
}

// Ping 超时未响应回调
static void ping_on_timeout(esp_ping_handle_t hdl, void *args) {
    ping_result_t *res = (ping_result_t *)args;
    ip_addr_t target_addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));
    char out_buf[128];
    snprintf(out_buf, sizeof(out_buf), "From " IPSTR " icmp_seq=%d timeout\r\n", 
             IP2STR(&target_addr.u_addr.ip4), res->total_cnt);
    tcl_hal_puts((const tcl_u8 *)out_buf);
    res->total_cnt++;
}

// Ping 会话结束回调（释放信号量激活等待任务）
static void ping_on_end(esp_ping_handle_t hdl, void *args) {
    ping_result_t *res = (ping_result_t *)args;
    xSemaphoreGive(res->sem);
}

// Tcl 指令: ipconfig
// 获取当前开发板的 Wi-Fi 连接状态，如果已连接则输出 SSID、RSSI、IP 地址、子网掩码和网关。
tcl_i32 tcl_cmd_ipconfig(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    char buf[256];
    wifi_ap_record_t ap_info;
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    
    // 使用正确的 esp_wifi_sta_get_ap_info
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, sizeof(buf), 
                 "Status: Connected\n"
                 "SSID: %s\n"
                 "RSSI: %d dBm\n"
                 "IP Address: " IPSTR "\n"
                 "Netmask: " IPSTR "\n"
                 "Gateway: " IPSTR,
                 ap_info.ssid, ap_info.rssi,
                 IP2STR(&ip_info.ip),
                 IP2STR(&ip_info.netmask),
                 IP2STR(&ip_info.gw));
    } else {
        snprintf(buf, sizeof(buf), "Status: Disconnected");
    }
    
    tcl_u32 len = strlen(buf) + 1;
    tcl_u32 res_offset = tcl_alc_p(context, len);
    if (res_offset != TCL_NULL) {
        char *ptr = (char *)TO_PTR(context, res_offset);
        if (ptr) {
            strcpy(ptr, buf);
        }
        context->result = res_offset;
    }
    return TCL_OK;
}

// Tcl 指令: ping <host>
// 向指定的主机或 IP 发送 ICMP Echo 请求以测试网络连通性。
tcl_i32 tcl_cmd_ping(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    if (arg_count < 2) return TCL_ERROR;
    const char *host = (const char *)TO_PTR(context, arg_values[1]);
    
    // 解析域名或 IP 地址
    struct addrinfo hints;
    struct addrinfo *res_addr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_RAW;
    
    int err = getaddrinfo(host, NULL, &hints, &res_addr);
    if (err != 0 || res_addr == NULL) {
        tcl_u32 res_offset = tcl_alc_p(context, 64);
        if (res_offset != TCL_NULL) {
            char *ptr = (char *)TO_PTR(context, res_offset);
            if (ptr) {
                snprintf(ptr, 64, "Ping: unknown host %s", host);
            }
            context->result = res_offset;
        }
        return TCL_OK;
    }
    
    struct sockaddr_in *saddr = (struct sockaddr_in *)res_addr->ai_addr;
    ip_addr_t target_addr;
    target_addr.u_addr.ip4.addr = saddr->sin_addr.s_addr;
    target_addr.type = IPADDR_TYPE_V4;
    freeaddrinfo(res_addr);
    
    char out_buf[128];
    snprintf(out_buf, sizeof(out_buf), "PING %s (" IPSTR ")\r\n", host, IP2STR(&target_addr.u_addr.ip4));
    tcl_hal_puts((const tcl_u8 *)out_buf);

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 4; // 默认 ping 4 次
    ping_config.task_stack_size = 4096; // 增加栈空间防止在回调中调用 printf 引起栈溢出 (Stack protection fault)

    ping_result_t result;
    result.sem = xSemaphoreCreateBinary();
    result.success_cnt = 0;
    result.total_cnt = 0;
    result.last_rtt = 0;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
        .cb_args = &result
    };

    esp_ping_handle_t ping_handle;
    if (esp_ping_new_session(&ping_config, &cbs, &ping_handle) == ESP_OK) {
        esp_ping_start(ping_handle);
        xSemaphoreTake(result.sem, portMAX_DELAY); // 挂起 Tcl 任务直到 Ping 结束
        esp_ping_delete_session(ping_handle);
    }
    vSemaphoreDelete(result.sem);
    
    // 将 ping 结果统计信息输出给 Tcl result 寄存器
    char buf[128];
    snprintf(buf, sizeof(buf), "Packets: Sent = %d, Received = %d, Lost = %d", 
             result.total_cnt, result.success_cnt, result.total_cnt - result.success_cnt);
    
    tcl_u32 len = strlen(buf) + 1;
    tcl_u32 res_offset = tcl_alc_p(context, len);
    if (res_offset != TCL_NULL) {
        char *ptr = (char *)TO_PTR(context, res_offset);
        if (ptr) {
            strcpy(ptr, buf);
        }
        context->result = res_offset;
    }
    return TCL_OK;
}

// Tcl 指令: sleep <ms>
// 挂起当前 Tcl 运行任务指定毫秒数（利用 FreeRTOS 任务延时让出 CPU，不阻塞其他系统任务及主循环）
tcl_i32 tcl_cmd_sleep(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    if (arg_count < 2) return TCL_ERROR;
    int ms = atoi((const char *)TO_PTR(context, arg_values[1]));
    if (ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    return TCL_OK;
}

// Tcl 指令: log <on|off>
// 动态开启或关闭 ESP-IDF 底层 Wi-Fi 协议栈和系统内核的高频调试日志，以防止干扰 Tcl 控制台输入。
tcl_i32 tcl_cmd_log(TclCtx *context, tcl_i32 arg_count, tcl_u32 *arg_values) {
    if (arg_count < 2) {
        // 如果没有携带参数，则返回当前 log 开关的 Tcl 状态值 ("on" 或 "off")
        tcl_u32 res_offset = tcl_alc_p(context, 16);
        if (res_offset != TCL_NULL) {
            char *dest = (char *)TO_PTR(context, res_offset);
            if (dest != NULL) {
                if (enable_log_print) {
                    dest[0] = 'o'; dest[1] = 'n'; dest[2] = '\0';
                } else {
                    dest[0] = 'o'; dest[1] = 'f'; dest[2] = 'f'; dest[3] = '\0';
                }
                context->result = res_offset;
            }
        }
        return TCL_OK;
    }
    const char *action = (const char *)TO_PTR(context, arg_values[1]);
    if (strcmp(action, "on") == 0) {
        enable_log_print = true;
        log_explicitly_set = true;
        esp_log_level_set("wifi", ESP_LOG_INFO);
        esp_log_level_set("WIFI", ESP_LOG_INFO);
        esp_log_level_set("*", ESP_LOG_INFO);
        printf("[INFO] Log printing turned ON\n");
    } else if (strcmp(action, "off") == 0) {
        enable_log_print = false;
        log_explicitly_set = true;
        esp_log_level_set("wifi", ESP_LOG_NONE);
        esp_log_level_set("WIFI", ESP_LOG_NONE);
        esp_log_level_set("*", ESP_LOG_WARN);
        printf("[INFO] Log printing turned OFF\n");
    } else {
        return TCL_ERROR;
    }
    return TCL_OK;
}

static void handle_input_char(TclCtx *ctx, TclShell *tcl_sh, uint8_t c) {
    if (shell_handle_char(tcl_sh, c, "> ") == 1) {
        int status = tcl_eval(ctx, tcl_sh->line);
        if (status == TCL_EXIT) {
            tcl_hal_puts((const tcl_u8 *)"BareTcl exit command triggered. System rebooting...\r\n");
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        } else if (status == TCL_ERROR) {
            char err_buf[512];
            snprintf(err_buf, sizeof(err_buf), "Error: %s\r\n", tcl_get_result(ctx));
            tcl_hal_puts((const tcl_u8 *)err_buf);
        } else {
            const tcl_u8 *res = tcl_get_result(ctx);
            if (res && res[0]) {
                tcl_hal_puts(res);
                tcl_hal_puts((const tcl_u8 *)"\r\n");
            }
        }
        
        memset(tcl_sh->line, 0, SHELL_MAX_LINE);
        tcl_sh->len = 0;
        tcl_sh->cursor = 0;
        
        if (baretcl_use_ansi) {
            tcl_hal_puts((const tcl_u8 *)"\x1b[0m> ");
        } else {
            tcl_hal_puts((const tcl_u8 *)"> ");
        }
    }
}

// -------------------------------------------------------------
// FreeRTOS Tcl 独立运行任务 (tcl_task)
// -------------------------------------------------------------
void tcl_task(void *pvParameters) {
    tcl_task_running = true;

    // 禁用当前 FreeRTOS 任务独占重入结构体（Reent）下的 stdin 和 stdout 流缓冲区。
    // 这是实现字符级非阻塞实时输入、即时回显的基础。
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);

    printf("[DIAG] tcl_task started!\n");
    fflush(stdout);

    // 获取内存 Arena 物理指针并执行 BareTcl 初始化
    TclCtx *ctx = (TclCtx *)tcl_arena;
    tcl_init(tcl_arena, TCL_ARENA_SIZE);
    
    // 注册核心通用库扩展 C 函数
    tcl_register_ext_cmds(ctx);
    
    // 注册 GPIO 控制与端口定制 of C 指令到 Tcl 命名空间中
    tcl_register_c_cmd((const tcl_u8 *)"gpio_mode", tcl_cmd_gpio_mode);
    tcl_register_c_cmd((const tcl_u8 *)"digital_write", tcl_cmd_digital_write);
    tcl_register_c_cmd((const tcl_u8 *)"digital_read", tcl_cmd_digital_read);
    tcl_register_c_cmd((const tcl_u8 *)"tcl_shell_ansi", tcl_cmd_tcl_shell_ansi);
    tcl_register_c_cmd((const tcl_u8 *)"log", tcl_cmd_log);
    tcl_register_c_cmd((const tcl_u8 *)"ipconfig", tcl_cmd_ipconfig);
    tcl_register_c_cmd((const tcl_u8 *)"ping", tcl_cmd_ping);
    tcl_register_c_cmd((const tcl_u8 *)"sleep", tcl_cmd_sleep);

    // 1. 核心关键步：显式加载标准 Tcl 自举脚本库 (tcllib.tcl -> tcllib.c)
    // 注册高级通用 Tcl 指令（如 for, foreach, incr, lappend, lsearch 等）
    if (tcl_load_bootstrap(ctx) != TCL_OK) {
        printf("[ERROR] Failed to load standard bootstrap library: %s\n", (const char *)tcl_get_result(ctx));
    }

    // 2. 核心关键步：评估执行端口特有的扩展自举 Tcl 脚本 (esp32_lib.tcl -> esp32_lib.c)
    // 注册包括 help, queens 在内的嵌入式特色应用与帮助菜单
    tcl_eval(ctx, (const tcl_u8 *)esp32_bootstrap);

    // 初始化交互式控制台 Shell 结构体（多端隔离）
    static TclShell uart_sh;
    static TclShell ws_sh;
    shell_init(&uart_sh);
    shell_init(&ws_sh);
    baretcl_use_ansi = 1; // 默认开启 ANSI 终端控制（支持 Putty、Miniterm 等的高级行编辑功能）

    tcl_hal_puts((const tcl_u8 *)"\r\n==============================================\r\n");
    tcl_hal_puts((const tcl_u8 *)"BareTcl Shell for ESP32 (ESP-IDF Native C)\r\n");
    tcl_hal_puts((const tcl_u8 *)"==============================================\r\n");
    tcl_hal_puts((const tcl_u8 *)"Use 'gpio_mode', 'digital_write', 'digital_read' to control HW.\r\n");
    tcl_hal_puts((const tcl_u8 *)"Standard monitor: ANSI enabled. Run 'tcl_shell_ansi 0' to disable ANSI.\r\n\x1b[0m> ");

    // 将 stdin 输入流底层的物理描述符配置为非阻塞模式（O_NONBLOCK）
    // 使得 fgetc(stdin) 能够不挂起地瞬间读入并处理当前缓冲区内所有的串口字节数据
    int fd = fileno(stdin);
    int fcntl_res = fcntl(fd, F_SETFL, O_NONBLOCK);
    printf("[DIAG] stdin fd: %d, fcntl set non-block result: %d\n", fd, fcntl_res);
    fflush(stdout);

    uint64_t last_diag_print = esp_timer_get_time() / 1000;

    // Tcl 任务主事件循环
    while (true) {
        int r;
        bool progress = false;
        
        // 1. 循环拉取当前非阻塞串口输入缓冲区中已到达的所有字符
        while ((r = fgetc(stdin)) != EOF) {
            progress = true;
            uint8_t c = (uint8_t)r;
            if (enable_log_print) {
                printf("[DIAG] Read UART char: 0x%02X (%c)\n", c, (c >= 32 && c < 127) ? c : ' ');
                fflush(stdout);
            }
            handle_input_char(ctx, &uart_sh, c);
        }

        // 2. 循环拉取 WebSocket 队列中已到达的所有输入字符
        char ws_c;
        while (input_queue != NULL && xQueueReceive(input_queue, &ws_c, 0) == pdTRUE) {
            progress = true;
            uint8_t c = (uint8_t)ws_c;
            if (enable_log_print) {
                printf("[DIAG] Read WS char: 0x%02X (%c)\n", c, (c >= 32 && c < 127) ? c : ' ');
                fflush(stdout);
            }
            handle_input_char(ctx, &ws_sh, c);
        }

        uint64_t now = esp_timer_get_time() / 1000;
        if (enable_log_print && (now - last_diag_print >= 5000)) {
            last_diag_print = now;
            printf("[DIAG] tcl_task loop alive, reading stdin & WS queue...\n");
            fflush(stdout);
        }

        // 串口和 WS 均输入空闲期间，强制让出 10ms 资源给同优先级或更高优先级任务运行
        if (!progress) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// -------------------------------------------------------------
// WebServer HTTP 静态请求路由与事件处理器
// -------------------------------------------------------------

// HTTP GET / 路由处理器：静态返回状态标识 "1" 证明网络与控制链路正常
static void ws_sess_free_ctx(void *ctx) {
    int fd = (int)(intptr_t)ctx;
    if (ws_mutex != NULL && xSemaphoreTake(ws_mutex, portMAX_DELAY) == pdTRUE) {
        if (ws_active_fd == fd) {
            ws_active_fd = -1;
            ws_server_handle = NULL;
            ESP_LOGI("WS", "Active WebSocket connection closed, fd: %d", fd);
        }
        xSemaphoreGive(ws_mutex);
    }
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        if (ws_mutex != NULL && xSemaphoreTake(ws_mutex, portMAX_DELAY) == pdTRUE) {
            ws_active_fd = httpd_req_to_sockfd(req);
            ws_server_handle = req->handle;
            httpd_sess_set_ctx(req->handle, ws_active_fd, (void *)(intptr_t)ws_active_fd, ws_sess_free_ctx);
            ESP_LOGI("WS", "Handshake done, new connection opened, fd: %d", ws_active_fd);
            xSemaphoreGive(ws_mutex);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE("WS", "httpd_ws_recv_frame failed to get length: %d", ret);
        return ret;
    }

    // 防御大包 OOM 攻击，设置最大帧限制为 1024 字节
    if (ws_pkt.len > 1024) {
        ESP_LOGE("WS", "Received WS frame size %d exceeds limit of 1024 bytes! Terminating connection.", (int)ws_pkt.len);
        return ESP_ERR_INVALID_SIZE;
    }

    if (ws_pkt.len > 0) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE("WS", "Failed to calloc memory for WS payload");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE("WS", "httpd_ws_recv_frame failed: %d", ret);
            free(buf);
            return ret;
        }

        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            for (size_t i = 0; i < ws_pkt.len; i++) {
                if (input_queue != NULL) {
                    if (xQueueSend(input_queue, &buf[i], 0) != pdTRUE) {
                        ESP_LOGW("WS", "input_queue is full, character dropped");
                    }
                }
            }
        }
        free(buf);
    }
    return ESP_OK;
}

// HTTP GET / 路由处理器：静态返回 console_html 网页内容
static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, console_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// HTTP GET /change_relay1 至 /change_relay4 路由处理器：
// 网络触发开启指定的继电器通道（将 GPIO 拉低），并刷新开启时间戳用于 10 秒自动延时自锁关断。
static esp_err_t relay_change_handler(httpd_req_t *req) {
    int index = -1;
    if (strstr(req->uri, "change_relay1")) index = 0;
    else if (strstr(req->uri, "change_relay2")) index = 1;
    else if (strstr(req->uri, "change_relay3")) index = 2;
    else if (strstr(req->uri, "change_relay4")) index = 3;

    if (index >= 0 && index < 4) {
        int next_val = 1; // 默认高电平（断开）
        
        // 加互斥锁更新计时器与激活标志位
        if (xSemaphoreTake(relayMutex, portMAX_DELAY) == pdTRUE) {
            if (relayOn[index]) { // 当前开启 -> 关闭
                next_val = 1;
                relayOn[index] = false;
            } else { // 当前关闭 -> 开启
                next_val = 0;
                relayOnMillis[index] = esp_timer_get_time() / 1000;
                relayOn[index] = true;
            }
            gpio_set_level(relayPins[index], next_val);
            xSemaphoreGive(relayMutex);
        }
        
        char res_buf[4];
        snprintf(res_buf, sizeof(res_buf), "%d", next_val);
        httpd_resp_send(req, res_buf, HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Relay not found");
    }
    return ESP_OK;
}

// Web Server 初始化启动与路由注册接口
httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true; // 启用最近最少使用（LRU）清理，保持资源占用紧凑

    if (httpd_start(&server, &config) == ESP_OK) {
        // 注册根路径根网页 API
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &uri_root);

        // 注册 WebSocket 路由
        httpd_uri_t uri_ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true
        };
        httpd_register_uri_handler(server, &uri_ws);

        // 注册 4 路网络继电器控制路由
        httpd_uri_t uri_relay1 = { .uri = "/change_relay1", .method = HTTP_GET, .handler = relay_change_handler };
        httpd_register_uri_handler(server, &uri_relay1);
        httpd_uri_t uri_relay2 = { .uri = "/change_relay2", .method = HTTP_GET, .handler = relay_change_handler };
        httpd_register_uri_handler(server, &uri_relay2);
        httpd_uri_t uri_relay3 = { .uri = "/change_relay3", .method = HTTP_GET, .handler = relay_change_handler };
        httpd_register_uri_handler(server, &uri_relay3);
        httpd_uri_t uri_relay4 = { .uri = "/change_relay4", .method = HTTP_GET, .handler = relay_change_handler };
        httpd_register_uri_handler(server, &uri_relay4);
    }
    return server;
}

// -------------------------------------------------------------
// Wi-Fi 事件回调与网络连接管理
// -------------------------------------------------------------
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); // Wi-Fi 启动后自动建立 AP 连接
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect(); // 断开后自动重试 AP 连接
        if (enable_log_print) {
            ESP_LOGI("WIFI", "Retrying AP connection...");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        if (enable_log_print) {
            ESP_LOGI("WIFI", "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        }
    }
}

// Wi-Fi 客户端模式 (STA) 冷启动初始化配置
void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    // 静态配置目标 Wi-Fi 路由的 SSID 与密钥
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "testzzzz",
            .password = "11111111",
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start(); // 激活 Wi-Fi 硬件
}

// -------------------------------------------------------------
// 板载硬件 GPIO 引脚及状态配置
// -------------------------------------------------------------
void init_gpio(void) {
    // 1. 配置 4 路按键输入引脚：输入模式，物理使能内部弱上拉，防止电平漂移
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << GPIO_NUM_10) | (1ULL << GPIO_NUM_9) | (1ULL << GPIO_NUM_6) | (1ULL << GPIO_NUM_8),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&io_conf);

    // 2. 配置 4 路继电器输出引脚：输出模式
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_4) | (1ULL << GPIO_NUM_5) | (1ULL << GPIO_NUM_7);
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    // 3. 硬件上电初始化状态：将所有低电平有效的继电器输出引脚置高（初始断开关断状态）
    // 之间加入 100ms 延迟防止继电器瞬间同时动作引发系统电涌或供电跌落重启
    for (int i = 0; i < 4; i++) {
        gpio_set_level(relayPins[i], 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// -------------------------------------------------------------
// 物理 UART0 串口输入输出驱动初始化
// -------------------------------------------------------------
void init_uart(void) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // 在物理 UART0 口上安装串口驱动（缓冲区大小配置为 2048 字节）
    uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_config);
    // 关键步：绑定 VFS 虚拟文件系统接口至当前 UART 驱动以使用标准 C 输入输出
    uart_vfs_dev_use_driver(UART_NUM_0);
}

// -------------------------------------------------------------
// ESP32 Native USB-Serial-JTAG 控制台虚拟文件系统绑定
// -------------------------------------------------------------
void init_usb_serial_jtag(void) {
    usb_serial_jtag_driver_config_t usb_serial_jtag_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    // 安装 USB-Serial-JTAG 驱动，利用芯片内部 FIFO 进行数据收发
    usb_serial_jtag_driver_install(&usb_serial_jtag_config);
    // 关键步：绑定 VFS 虚拟文件系统接口至当前驱动，使得芯片可通过原生 USB 直连线收发控制台输入
    usb_serial_jtag_vfs_use_driver();
}

// -------------------------------------------------------------
// 整个 ESP32 应用程序的入口 (app_main)
// -------------------------------------------------------------
void app_main(void) {
    // 启动之初，立刻屏蔽除警告和错误之外的高频日志输出，防止系统背景日志破坏 Tcl Shell 控制台的可读性
    esp_log_level_set("wifi", ESP_LOG_NONE);
    esp_log_level_set("WIFI", ESP_LOG_NONE);
    esp_log_level_set("*", ESP_LOG_WARN);

    // 初始化非易失闪存（NVS），WiFi 协议栈在闪存内部建立网络校准数据存储时必须依赖此组件
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化继电状态保护锁
    relayMutex = xSemaphoreCreateMutex();

    // 初始化 WebSocket 互斥锁
    ws_mutex = xSemaphoreCreateMutex();
    if (ws_mutex == NULL) {
        printf("[ERROR] Failed to create WebSocket mutex\n");
    }
    
    // 初始化 WebSocket 输入队列 (1024 字节)
    input_queue = xQueueCreate(1024, sizeof(char));
    if (input_queue == NULL) {
        printf("[ERROR] Failed to create WebSocket input queue\n");
    }
    
    // 初始化板卡外设及通信接口
    init_gpio();
    init_uart();
    init_usb_serial_jtag();

    // 禁用主任务下的输入输出缓冲
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);

    // 彻底改写并擦除 stdin 标准输入的 ICANON 规范行处理及 ECHO 回显标志
    // 从而使串口驱动直接将原生字符透明搬运至 `shell_handle_char` 进行实时光标移动、回退处理
    struct termios t;
    int getattr_res = tcgetattr(fileno(stdin), &t);
    printf("[DIAG] tcgetattr result: %d, errno: %d\n", getattr_res, getattr_res == 0 ? 0 : errno);
    fflush(stdout);
    if (getattr_res == 0) {
        t.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN);
        int setattr_res = tcsetattr(fileno(stdin), TCSANOW, &t);
        printf("[DIAG] tcsetattr result: %d, errno: %d\n", setattr_res, setattr_res == 0 ? 0 : errno);
        fflush(stdout);
    }

    // 激活网络与服务
    wifi_init_sta();
    start_webserver();

    // 创建独立的 FreeRTOS 任务（tcl_task），分配 8KB 的栈空间，优先级设为 5
    tcl_task_create_res = xTaskCreate(tcl_task, "tcl_task", 8192, NULL, 5, NULL);

    // 芯片硬件轮询主循环（用于按键的消抖捕获、继电器的自动安全延时自锁关断）
    bool lastButtonState[4] = { 1, 1, 1, 1 };
    uint64_t previousMillis = esp_timer_get_time() / 1000;
    uint64_t lastPrintMillis = esp_timer_get_time() / 1000;
    const uint64_t resetInterval = 1 * 60 * 60 * 1000; // 每 1 小时自动重启以防系统在极极端环境下僵死
    const uint64_t printInterval = 1000; // 日志打印频率为 1 秒
    const uint64_t relayOnDuration = 10000; // 继电器开启 10 秒后自动断开，实现物理延时关断

    while (true) {
        uint64_t currentMillis = esp_timer_get_time() / 1000;

        // 定期重启触发
        if (currentMillis - previousMillis >= resetInterval) {
            esp_restart();
        }

        // 如果开启了日志，打印运行健康状况
        if (enable_log_print && (currentMillis - lastPrintMillis >= printInterval)) {
            lastPrintMillis = currentMillis;
            printf("Running time: %lld s, task_created: %d, task_running: %d\n", 
                   currentMillis / 1000, tcl_task_create_res, tcl_task_running);
            fflush(stdout);
        }

        // 轮询 4 路板载实体按键（低电平有效）的按下状态
        for (int i = 0; i < 4; i++) {
            bool buttonState = gpio_get_level(buttonPins[i]);
            if (buttonState == 0 && lastButtonState[i] == 1) { // 边缘跳变捕获：按键被按下
                // 在互斥锁保护下翻转继电器的输出电平并启动自动关断计时
                if (xSemaphoreTake(relayMutex, portMAX_DELAY) == pdTRUE) {
                    int nextVal = 1;
                    if (relayOn[i]) { // 当前开启 -> 关闭
                        nextVal = 1;
                        relayOn[i] = false;
                    } else { // 当前关闭 -> 开启
                        nextVal = 0;
                        relayOn[i] = true;
                        relayOnMillis[i] = esp_timer_get_time() / 1000;
                    }
                    gpio_set_level(relayPins[i], nextVal);
                    xSemaphoreGive(relayMutex);
                }
            }
            lastButtonState[i] = buttonState;

            // 定时断开安全检测：如果继电器持续处于开启状态超过 10 秒，强制断开以确保物理安全
            bool shouldClose = false;
            if (xSemaphoreTake(relayMutex, portMAX_DELAY) == pdTRUE) {
                if (relayOn[i] && (currentMillis - relayOnMillis[i] >= relayOnDuration)) {
                    relayOn[i] = false;
                    shouldClose = true;
                }
                xSemaphoreGive(relayMutex);
            }
            
            if (shouldClose) {
                gpio_set_level(relayPins[i], 1); // 强制输出高电平，关闭继电器
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // 睡眠 10ms，防止该大循环占满 CPU
    }
}
