/**
 * @file bitun_osal_esp32.c
 * @brief OSAL ESP32 (ESP-IDF/FreeRTOS/MbedTLS) implementation for BiTun
 */

#include "bitun_osal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_vfs_eventfd.h"
#include "mbedtls/md.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/chachapoly.h"

volatile sig_atomic_t g_should_exit = 0;

/* ========================================================================== */
/* 1. 套接字网络抽象 API                                                      */
/* ========================================================================== */

bitun_socket_t bitun_osal_socket_create(int domain, int type, int protocol) {
    return socket(domain, type, protocol);
}

int bitun_osal_socket_close(bitun_socket_t fd) {
    if (fd >= 0) {
        return close(fd);
    }
    return 0;
}

int bitun_osal_socket_bind(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen) {
    return bind(fd, addr, addrlen);
}

int bitun_osal_socket_listen(bitun_socket_t fd, int backlog) {
    return listen(fd, backlog);
}

bitun_socket_t bitun_osal_socket_accept(bitun_socket_t fd, struct sockaddr *addr, bitun_socklen_t *addrlen) {
    return accept(fd, addr, addrlen);
}

int bitun_osal_socket_connect(bitun_socket_t fd, const struct sockaddr *addr, bitun_socklen_t addrlen) {
    return connect(fd, addr, addrlen);
}

int bitun_osal_socket_send(bitun_socket_t fd, const void *buf, size_t len, int flags) {
    return send(fd, buf, len, flags);
}

int bitun_osal_socket_recv(bitun_socket_t fd, void *buf, size_t len, int flags) {
    return recv(fd, buf, len, flags);
}

int bitun_osal_socket_sendto(bitun_socket_t fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *dest_addr, bitun_socklen_t addrlen) {
    return sendto(fd, buf, len, flags, dest_addr, addrlen);
}

int bitun_osal_socket_recvfrom(bitun_socket_t fd, void *buf, size_t len, int flags,
                               struct sockaddr *src_addr, bitun_socklen_t *addrlen) {
    return recvfrom(fd, buf, len, flags, src_addr, addrlen);
}

int bitun_osal_socket_set_nonblocking(bitun_socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int bitun_osal_socket_set_reuseaddr(bitun_socket_t fd) {
    int reuse = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
}

/* ========================================================================== */
/* 2. 多路复用事件监听接口 (Poll 实现)                                         */
/* ========================================================================== */

#define MAX_POLL_FDS 32
struct bitun_osal_poll_set {
    struct pollfd fds[MAX_POLL_FDS];
    int nfds;
    pthread_mutex_t lock;
};

bitun_osal_poll_set_t *bitun_osal_poll_create(void) {
    bitun_osal_poll_set_t *set = malloc(sizeof(*set));
    if (!set) return NULL;
    set->nfds = 0;
    for (int i = 0; i < MAX_POLL_FDS; i++) {
        set->fds[i].fd = -1;
        set->fds[i].events = 0;
        set->fds[i].revents = 0;
    }
    pthread_mutex_init(&set->lock, NULL);
    return set;
}

void bitun_osal_poll_destroy(bitun_osal_poll_set_t *set) {
    if (set) {
        pthread_mutex_destroy(&set->lock);
        free(set);
    }
}

int bitun_osal_poll_add(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events) {
    if (!set || fd < 0) return -1;
    pthread_mutex_lock(&set->lock);
    
    // Check if already exists
    for (int i = 0; i < set->nfds; i++) {
        if (set->fds[i].fd == fd) {
            pthread_mutex_unlock(&set->lock);
            return -1;
        }
    }
    
    if (set->nfds >= MAX_POLL_FDS) {
        pthread_mutex_unlock(&set->lock);
        return -1;
    }
    
    int index = set->nfds;
    set->fds[index].fd = fd;
    set->fds[index].events = 0;
    if (events & BITUN_POLL_IN) set->fds[index].events |= POLLIN;
    if (events & BITUN_POLL_OUT) set->fds[index].events |= POLLOUT;
    if (events & BITUN_POLL_ERR) set->fds[index].events |= POLLERR;
    set->fds[index].revents = 0;
    set->nfds++;
    pthread_mutex_unlock(&set->lock);
    return 0;
}

int bitun_osal_poll_mod(bitun_osal_poll_set_t *set, bitun_socket_t fd, uint32_t events) {
    if (!set || fd < 0) return -1;
    pthread_mutex_lock(&set->lock);
    
    for (int i = 0; i < set->nfds; i++) {
        if (set->fds[i].fd == fd) {
            set->fds[i].events = 0;
            if (events & BITUN_POLL_IN) set->fds[i].events |= POLLIN;
            if (events & BITUN_POLL_OUT) set->fds[i].events |= POLLOUT;
            if (events & BITUN_POLL_ERR) set->fds[i].events |= POLLERR;
            pthread_mutex_unlock(&set->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&set->lock);
    return -1;
}

int bitun_osal_poll_del(bitun_osal_poll_set_t *set, bitun_socket_t fd) {
    if (!set || fd < 0) return -1;
    pthread_mutex_lock(&set->lock);
    
    for (int i = 0; i < set->nfds; i++) {
        if (set->fds[i].fd == fd) {
            for (int j = i; j < set->nfds - 1; j++) {
                set->fds[j] = set->fds[j + 1];
            }
            set->nfds--;
            set->fds[set->nfds].fd = -1;
            set->fds[set->nfds].events = 0;
            set->fds[set->nfds].revents = 0;
            pthread_mutex_unlock(&set->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&set->lock);
    return -1;
}

int bitun_osal_poll_wait(bitun_osal_poll_set_t *set, int timeout_ms, 
                         bitun_osal_event_t *events_out, int max_events) {
    if (!set || max_events <= 0) return -1;
    
    pthread_mutex_lock(&set->lock);
    int nfds = set->nfds;
    struct pollfd local_fds[MAX_POLL_FDS];
    memcpy(local_fds, set->fds, sizeof(struct pollfd) * nfds);
    pthread_mutex_unlock(&set->lock);
    
    int ret = poll(local_fds, nfds, timeout_ms);
    if (ret < 0) {
        return -1;
    }
    if (ret == 0) {
        return 0;
    }
    
    int count = 0;
    for (int i = 0; i < nfds && count < max_events; i++) {
        if (local_fds[i].revents != 0) {
            events_out[count].fd = local_fds[i].fd;
            events_out[count].events = 0;
            if (local_fds[i].revents & POLLIN) events_out[count].events |= BITUN_POLL_IN;
            if (local_fds[i].revents & POLLOUT) events_out[count].events |= BITUN_POLL_OUT;
            if (local_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) events_out[count].events |= BITUN_POLL_ERR;
            count++;
        }
    }
    return count;
}

/* ========================================================================== */
/* 3. 线程与同步互斥锁接口                                                     */
/* ========================================================================== */

struct bitun_osal_thread {
    TaskHandle_t task;
};

typedef struct {
    bitun_osal_thread_entry_t entry;
    void *arg;
} thread_wrapper_arg_t;

static void thread_wrapper(void *arg) {
    thread_wrapper_arg_t *wrap = (thread_wrapper_arg_t *)arg;
    wrap->entry(wrap->arg);
    free(wrap);
    vTaskDelete(NULL);
}

int bitun_osal_thread_create(bitun_osal_thread_t **thread_out, const char *name, 
                             uint32_t stack_size, uint32_t priority,
                             bitun_osal_thread_entry_t entry, void *arg) {
    if (!thread_out || !entry) return -1;
    
    bitun_osal_thread_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    
    thread_wrapper_arg_t *wrap = malloc(sizeof(*wrap));
    if (!wrap) {
        free(t);
        return -1;
    }
    wrap->entry = entry;
    wrap->arg = arg;
    
    uint32_t stack_bytes = stack_size;
    if (stack_bytes < 2048) {
        stack_bytes = 2048;
    }
    
    BaseType_t ret = xTaskCreate(thread_wrapper, name, stack_bytes, wrap, priority, &t->task);
    if (ret != pdPASS) {
        free(wrap);
        free(t);
        return -1;
    }
    
    *thread_out = t;
    return 0;
}

int bitun_osal_thread_detach(bitun_osal_thread_t *thread) {
    if (!thread) return -1;
    free(thread);
    return 0;
}

void bitun_osal_thread_sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

struct bitun_osal_mutex {
    SemaphoreHandle_t sem;
};

int bitun_osal_mutex_create(bitun_osal_mutex_t **mutex_out) {
    if (!mutex_out) return -1;
    bitun_osal_mutex_t *m = malloc(sizeof(*m));
    if (!m) return -1;
    m->sem = xSemaphoreCreateMutex();
    if (!m->sem) {
        free(m);
        return -1;
    }
    *mutex_out = m;
    return 0;
}

int bitun_osal_mutex_lock(bitun_osal_mutex_t *mutex) {
    if (!mutex || !mutex->sem) return -1;
    return (xSemaphoreTake(mutex->sem, portMAX_DELAY) == pdTRUE) ? 0 : -1;
}

int bitun_osal_mutex_unlock(bitun_osal_mutex_t *mutex) {
    if (!mutex || !mutex->sem) return -1;
    return (xSemaphoreGive(mutex->sem) == pdTRUE) ? 0 : -1;
}

int bitun_osal_mutex_destroy(bitun_osal_mutex_t *mutex) {
    if (!mutex) return -1;
    if (mutex->sem) {
        vSemaphoreDelete(mutex->sem);
    }
    free(mutex);
    return 0;
}

/* ========================================================================== */
/* 4. eventfd 跨线程/进程通信队列                                               */
/* ========================================================================== */

struct bitun_osal_queue {
    int ev_fd;
    uint8_t *buffer;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    SemaphoreHandle_t lock;
};

bitun_osal_queue_t *bitun_osal_queue_create(size_t item_size, size_t capacity) {
    static bool evfd_registered = false;
    if (!evfd_registered) {
        esp_vfs_eventfd_config_t config = { .max_fds = 32 };
        esp_vfs_eventfd_register(&config);
        evfd_registered = true;
    }
    
    bitun_osal_queue_t *q = malloc(sizeof(*q));
    if (!q) return NULL;
    
    q->ev_fd = eventfd(0, 0);
    if (q->ev_fd < 0) {
        free(q);
        return NULL;
    }
    
    int flags = fcntl(q->ev_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(q->ev_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    q->buffer = malloc(item_size * capacity);
    if (!q->buffer) {
        close(q->ev_fd);
        free(q);
        return NULL;
    }
    
    q->item_size = item_size;
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    
    q->lock = xSemaphoreCreateMutex();
    if (!q->lock) {
        free(q->buffer);
        close(q->ev_fd);
        free(q);
        return NULL;
    }
    
    return q;
}

void bitun_osal_queue_destroy(bitun_osal_queue_t *q) {
    if (q) {
        if (q->ev_fd >= 0) {
            close(q->ev_fd);
        }
        free(q->buffer);
        if (q->lock) {
            vSemaphoreDelete(q->lock);
        }
        free(q);
    }
}

int bitun_osal_queue_push(bitun_osal_queue_t *q, const void *item) {
    if (!q || !item) return -1;
    
    if (xSemaphoreTake(q->lock, portMAX_DELAY) != pdTRUE) return -1;
    if (q->count >= q->capacity) {
        xSemaphoreGive(q->lock);
        return -1;
    }
    
    memcpy(q->buffer + (q->tail * q->item_size), item, q->item_size);
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    xSemaphoreGive(q->lock);
    
    uint64_t val = 1;
    ssize_t n = write(q->ev_fd, &val, sizeof(val));
    (void)n;
    
    return 0;
}

int bitun_osal_queue_pop(bitun_osal_queue_t *q, void *item_out) {
    if (!q || !item_out) return -1;
    
    if (xSemaphoreTake(q->lock, portMAX_DELAY) != pdTRUE) return -1;
    if (q->count == 0) {
        xSemaphoreGive(q->lock);
        return -1;
    }
    
    memcpy(item_out, q->buffer + (q->head * q->item_size), q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    xSemaphoreGive(q->lock);
    
    return 0;
}

bitun_socket_t bitun_osal_queue_get_read_fd(bitun_osal_queue_t *q) {
    return q ? q->ev_fd : -1;
}

void bitun_osal_queue_clear_wakeup(bitun_osal_queue_t *q) {
    if (q && q->ev_fd >= 0) {
        uint64_t val = 0;
        ssize_t n = read(q->ev_fd, &val, sizeof(val));
        (void)n;
    }
}

/* ========================================================================== */
/* 5. 全局异步 DNS 解析系统                                                    */
/* ========================================================================== */

typedef struct dns_task {
    char domain[256];
    uint32_t channel_id;
    bitun_osal_queue_t *result_queue;
    struct dns_task *next;
} dns_task_t;

static pthread_t dns_thread;
static pthread_mutex_t dns_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dns_cond = PTHREAD_COND_INITIALIZER;
static dns_task_t *dns_queue_head = NULL;
static dns_task_t *dns_queue_tail = NULL;
static int dns_thread_running = 0;
static int dns_should_stop = 0;

static void *dns_worker_thread(void *arg) {
    (void)arg;
    pthread_detach(pthread_self());
    while (1) {
        pthread_mutex_lock(&dns_lock);
        while (dns_queue_head == NULL && !dns_should_stop) {
            pthread_cond_wait(&dns_cond, &dns_lock);
        }
        if (dns_should_stop) {
            pthread_mutex_unlock(&dns_lock);
            break;
        }
        dns_task_t *task = dns_queue_head;
        dns_queue_head = task->next;
        if (dns_queue_head == NULL) {
            dns_queue_tail = NULL;
        }
        pthread_mutex_unlock(&dns_lock);

        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        bitun_osal_dns_result_t result;
        memset(&result, 0, sizeof(result));
        result.channel_id = task->channel_id;

        int s = getaddrinfo(task->domain, NULL, &hints, &res);
        if (s == 0 && res != NULL) {
            result.success = 1;
            result.resolved_addr = malloc(res->ai_addrlen);
            if (result.resolved_addr) {
                memcpy(result.resolved_addr, res->ai_addr, res->ai_addrlen);
            }
            if (res->ai_family == AF_INET) {
                struct sockaddr_in *addr_in = (struct sockaddr_in *)res->ai_addr;
                memcpy(result.resolved_ipv4, &addr_in->sin_addr.s_addr, 4);
            }
            freeaddrinfo(res);
        } else {
            result.success = 0;
            result.resolved_addr = NULL;
        }

        if (bitun_osal_queue_push(task->result_queue, &result) != 0) {
            if (result.resolved_addr) {
                free(result.resolved_addr);
            }
        }

        free(task);
    }
    return NULL;
}

int bitun_osal_dns_init(void) {
    pthread_mutex_lock(&dns_lock);
    if (dns_thread_running) {
        pthread_mutex_unlock(&dns_lock);
        return 0;
    }
    dns_should_stop = 0;
    dns_queue_head = NULL;
    dns_queue_tail = NULL;
    int ret = pthread_create(&dns_thread, NULL, dns_worker_thread, NULL);
    if (ret == 0) {
        dns_thread_running = 1;
    }
    pthread_mutex_unlock(&dns_lock);
    return (ret == 0) ? 0 : -1;
}

void bitun_osal_dns_deinit(void) {
    pthread_mutex_lock(&dns_lock);
    if (!dns_thread_running) {
        pthread_mutex_unlock(&dns_lock);
        return;
    }
    dns_should_stop = 1;
    pthread_cond_signal(&dns_cond);
    pthread_mutex_unlock(&dns_lock);

    pthread_mutex_lock(&dns_lock);
    dns_thread_running = 0;
    dns_task_t *curr = dns_queue_head;
    while (curr) {
        dns_task_t *next = curr->next;
        free(curr);
        curr = next;
    }
    dns_queue_head = NULL;
    dns_queue_tail = NULL;
    pthread_mutex_unlock(&dns_lock);
}

int bitun_osal_dns_resolve_async(const char *domain, uint32_t channel_id, 
                                 bitun_osal_queue_t *result_queue) {
    if (!domain || !result_queue) return -1;
    dns_task_t *task = malloc(sizeof(dns_task_t));
    if (!task) return -1;
    strncpy(task->domain, domain, sizeof(task->domain) - 1);
    task->domain[sizeof(task->domain) - 1] = '\0';
    task->channel_id = channel_id;
    task->result_queue = result_queue;
    task->next = NULL;

    pthread_mutex_lock(&dns_lock);
    if (!dns_thread_running) {
        pthread_mutex_unlock(&dns_lock);
        free(task);
        return -1;
    }
    if (dns_queue_tail) {
        dns_queue_tail->next = task;
        dns_queue_tail = task;
    } else {
        dns_queue_head = task;
        dns_queue_tail = task;
    }
    pthread_cond_signal(&dns_cond);
    pthread_mutex_unlock(&dns_lock);
    return 0;
}

/* ========================================================================== */
/* 6. 统一密码学加解密与认证接口 (MbedTLS 实现)                                  */
/* ========================================================================== */

int bitun_osal_crypto_hmac_sha256(const uint8_t *key, size_t key_len,
                                  const uint8_t *data, size_t data_len,
                                  uint8_t *mac_out) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return -1;
    int ret = mbedtls_md_hmac(md_info, key, key_len, data, data_len, mac_out);
    return (ret == 0) ? 0 : -1;
}

int bitun_osal_crypto_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                                  const uint8_t *ikm, size_t ikm_len,
                                  const uint8_t *info, size_t info_len,
                                  uint8_t *okm_out, size_t okm_len) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return -1;
    int ret = mbedtls_hkdf(md_info, salt, salt_len, ikm, ikm_len, info, info_len, okm_out, okm_len);
    return (ret == 0) ? 0 : -1;
}

int bitun_osal_crypto_chacha20_poly1305_encrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *plaintext, size_t plaintext_len,
                                                uint8_t *ciphertext_out, uint8_t *tag_out) {
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int ret = mbedtls_chachapoly_setkey(&ctx, key);
    if (ret == 0) {
        ret = mbedtls_chachapoly_encrypt_and_tag(&ctx, plaintext_len, nonce, NULL, 0, plaintext, ciphertext_out, tag_out);
    }
    mbedtls_chachapoly_free(&ctx);
    return (ret == 0) ? (int)plaintext_len : -1;
}

int bitun_osal_crypto_chacha20_poly1305_decrypt(const uint8_t *key, const uint8_t *nonce,
                                                const uint8_t *ciphertext, size_t ciphertext_len,
                                                const uint8_t *tag, uint8_t *plaintext_out) {
    mbedtls_chachapoly_context ctx;
    mbedtls_chachapoly_init(&ctx);
    int ret = mbedtls_chachapoly_setkey(&ctx, key);
    if (ret == 0) {
        ret = mbedtls_chachapoly_auth_decrypt(&ctx, ciphertext_len, nonce, NULL, 0, tag, ciphertext, plaintext_out);
    }
    mbedtls_chachapoly_free(&ctx);
    return (ret == 0) ? (int)ciphertext_len : -1;
}

/* ========================================================================== */
/* 7. 系统时钟与随机数接口                                                     */
/* ========================================================================== */

uint64_t bitun_osal_time_get_ms(void) {
    return esp_timer_get_time() / 1000;
}

uint64_t bitun_osal_time_get_real_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

uint32_t bitun_osal_random_u32(void) {
    return esp_random();
}

void bitun_osal_random_bytes(uint8_t *buf, size_t len) {
    esp_fill_random(buf, len);
}
