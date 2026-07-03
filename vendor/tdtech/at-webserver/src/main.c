#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define AT_TYPE_NETWORK 0
#define AT_TYPE_SERIAL 1

#define WS_MAX_CLIENTS 16
#define WS_TEXT_MAX 4096
#define AT_LINE_MAX 4096
#define AT_RESPONSE_MAX 65536
#define AT_READ_BUF 2048
#define AT_COMMAND_QUEUE 32

#define NOTIFY_SMS 1
#define NOTIFY_CALL 2
#define NOTIFY_MEMORY_FULL 3
#define NOTIFY_SIGNAL 4

typedef struct {
    int enabled;
    int connection_type;
    char network_host[64];
    int network_port;
    int network_timeout_sec;
    char serial_port[128];
    int serial_baudrate;
    int serial_timeout_sec;
    int websocket_port;
    char websocket_host[64];
    char websocket_auth_key[128];
    char websocket_host_v6[64];
    char wechat_webhook[512];
    char log_file[256];
    int notify_sms;
    int notify_call;
    int notify_memory_full;
    int notify_signal;
} app_config_t;

typedef struct {
    int success;
    char data[AT_RESPONSE_MAX];
    char error[AT_RESPONSE_MAX];
} at_response_t;

typedef struct {
    char command[WS_TEXT_MAX];
    int finished;
    at_response_t response;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} command_request_t;

typedef struct {
    command_request_t *items[AT_COMMAND_QUEUE];
    int head;
    int tail;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} command_queue_t;

typedef struct {
    char items[AT_COMMAND_QUEUE][16];
    int head;
    int tail;
    int size;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} sms_queue_t;

typedef struct {
    int fd;
    int active;
    pthread_mutex_t write_mutex;
} at_transport_t;

typedef struct {
    int fd;
    int active;
    int authed;
    pthread_t thread;
} ws_client_t;

typedef struct {
    char command[WS_TEXT_MAX];
    char lines[AT_RESPONSE_MAX];
    time_t deadline;
    command_request_t *request;
} pending_command_t;

typedef struct {
    app_config_t config;
    at_transport_t transport;
    command_queue_t queue;
    sms_queue_t sms_queue;
    ws_client_t clients[WS_MAX_CLIENTS];
    pthread_mutex_t clients_mutex;
    pthread_mutex_t pending_mutex;
    pending_command_t pending;
    volatile sig_atomic_t running;
} app_state_t;

static app_state_t g_app;
static int send_sync_command(app_state_t *app, const char *command, at_response_t *response);
static int g_verbose = 0;

#define LOGF(...) do { if (g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

typedef struct {
    int reference;
    int total;
    int seq;
    int valid;
} sms_partial_t;

typedef struct {
    int in_use;
    int ref;
    char sender[64];
    int total;
    int received_mask;
    char parts[16][512];
    time_t first_seen;
    char timestamp[64];
} partial_sms_entry_t;

static partial_sms_entry_t g_partial_sms[16];
static pthread_mutex_t g_partial_sms_mutex = PTHREAD_MUTEX_INITIALIZER;

static void set_defaults(app_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->enabled = 1;
    config->connection_type = AT_TYPE_NETWORK;
    snprintf(config->network_host, sizeof(config->network_host), "192.168.8.1");
    config->network_port = 20249;
    config->network_timeout_sec = 10;
    snprintf(config->serial_port, sizeof(config->serial_port), "/dev/ttyUSB0");
    config->serial_baudrate = 115200;
    config->serial_timeout_sec = 10;
    config->websocket_port = 8765;
    snprintf(config->websocket_host, sizeof(config->websocket_host), "0.0.0.0");
    snprintf(config->websocket_host_v6, sizeof(config->websocket_host_v6), "::");
    config->websocket_auth_key[0] = '\0';
    config->wechat_webhook[0] = '\0';
    config->log_file[0] = '\0';
    config->notify_sms = 1;
    config->notify_call = 1;
    config->notify_memory_full = 1;
    config->notify_signal = 1;
}

static void print_config(const app_config_t *config) {
    LOGF("service enabled: %d\n", config->enabled);
    LOGF("connection type: %s\n", config->connection_type == AT_TYPE_NETWORK ? "NETWORK" : "SERIAL");
    LOGF("network target: %s:%d timeout=%d\n", config->network_host, config->network_port, config->network_timeout_sec);
    LOGF("serial target: %s baud=%d timeout=%d\n", config->serial_port, config->serial_baudrate, config->serial_timeout_sec);
    LOGF("websocket listen v4: %s:%d\n", config->websocket_host, config->websocket_port);
    LOGF("websocket listen v6: %s:%d\n", config->websocket_host_v6, config->websocket_port);
    LOGF("websocket auth: %s\n", config->websocket_auth_key[0] ? "enabled" : "disabled");
    LOGF("wechat webhook: %s\n", config->wechat_webhook[0] ? "enabled" : "disabled");
    LOGF("log file: %s\n", config->log_file[0] ? config->log_file : "disabled");
    LOGF("notify: sms=%d call=%d memory_full=%d signal=%d\n",
         config->notify_sms, config->notify_call, config->notify_memory_full, config->notify_signal);
}

static void queue_init(command_queue_t *queue) {
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
}

static void sms_queue_init(sms_queue_t *queue) {
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
}

static int queue_push(command_queue_t *queue, command_request_t *request) {
    pthread_mutex_lock(&queue->mutex);
    if (queue->size >= AT_COMMAND_QUEUE) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }
    queue->items[queue->tail] = request;
    queue->tail = (queue->tail + 1) % AT_COMMAND_QUEUE;
    queue->size++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

static command_request_t *queue_pop(command_queue_t *queue) {
    command_request_t *request = NULL;

    pthread_mutex_lock(&queue->mutex);
    while (g_app.running && queue->size == 0) {
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    if (queue->size > 0) {
        request = queue->items[queue->head];
        queue->head = (queue->head + 1) % AT_COMMAND_QUEUE;
        queue->size--;
    }
    pthread_mutex_unlock(&queue->mutex);
    return request;
}

static int sms_queue_push(sms_queue_t *queue, const char *index_text) {
    pthread_mutex_lock(&queue->mutex);
    if (queue->size >= AT_COMMAND_QUEUE) {
        pthread_mutex_unlock(&queue->mutex);
        return -1;
    }
    snprintf(queue->items[queue->tail], sizeof(queue->items[queue->tail]), "%s", index_text);
    queue->tail = (queue->tail + 1) % AT_COMMAND_QUEUE;
    queue->size++;
    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return 0;
}

static int sms_queue_pop(sms_queue_t *queue, char *index_text, size_t index_size) {
    int ok = -1;

    pthread_mutex_lock(&queue->mutex);
    while (g_app.running && queue->size == 0) {
        pthread_cond_wait(&queue->cond, &queue->mutex);
    }
    if (queue->size > 0) {
        snprintf(index_text, index_size, "%s", queue->items[queue->head]);
        queue->head = (queue->head + 1) % AT_COMMAND_QUEUE;
        queue->size--;
        ok = 0;
    }
    pthread_mutex_unlock(&queue->mutex);
    return ok;
}

static void request_init(command_request_t *request, const char *command) {
    memset(request, 0, sizeof(*request));
    snprintf(request->command, sizeof(request->command), "%s", command);
    pthread_mutex_init(&request->mutex, NULL);
    pthread_cond_init(&request->cond, NULL);
}

static void request_finish(command_request_t *request, int success, const char *data, const char *error) {
    pthread_mutex_lock(&request->mutex);
    request->response.success = success;
    snprintf(request->response.data, sizeof(request->response.data), "%s", data ? data : "");
    snprintf(request->response.error, sizeof(request->response.error), "%s", error ? error : "");
    request->finished = 1;
    pthread_cond_signal(&request->cond);
    pthread_mutex_unlock(&request->mutex);
}

static void request_destroy(command_request_t *request) {
    pthread_mutex_destroy(&request->mutex);
    pthread_cond_destroy(&request->cond);
}

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int transport_connect_network(at_transport_t *transport, const app_config_t *config) {
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) config->network_port);
    if (inet_pton(AF_INET, config->network_host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }

    transport->fd = fd;
    transport->active = 1;
    pthread_mutex_init(&transport->write_mutex, NULL);
    return 0;
}

static speed_t baud_to_flag(int baudrate) {
    switch (baudrate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        case 230400:
#ifdef B230400
            return B230400;
#else
            return B115200;
#endif
        case 460800:
#ifdef B460800
            return B460800;
#else
            return B115200;
#endif
        case 921600:
#ifdef B921600
            return B921600;
#else
            return B115200;
#endif
        default: return B115200;
    }
}

static int transport_connect_serial(at_transport_t *transport, const app_config_t *config) {
    struct termios tty;
    int fd = open(config->serial_port, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd < 0) return -1;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    cfsetispeed(&tty, baud_to_flag(config->serial_baudrate));
    cfsetospeed(&tty, baud_to_flag(config->serial_baudrate));
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
    tty.c_iflag &= ~(IGNBRK | IXON | IXOFF | IXANY);
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif
    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }

    transport->fd = fd;
    transport->active = 1;
    pthread_mutex_init(&transport->write_mutex, NULL);
    return 0;
}

static void transport_close(at_transport_t *transport) {
    if (!transport->active) return;
    close(transport->fd);
    transport->fd = -1;
    transport->active = 0;
    pthread_mutex_destroy(&transport->write_mutex);
}

static int transport_write_all(at_transport_t *transport, const char *buf, size_t len) {
    size_t offset = 0;

    pthread_mutex_lock(&transport->write_mutex);
    while (offset < len) {
        ssize_t written = write(transport->fd, buf + offset, len - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            pthread_mutex_unlock(&transport->write_mutex);
            return -1;
        }
        offset += (size_t) written;
    }
    pthread_mutex_unlock(&transport->write_mutex);
    return 0;
}

static void trim_eol(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[len - 1] = '\0';
        len--;
    }
}

static void trim_space(char *line) {
    size_t start = 0;
    size_t len = strlen(line);
    while (line[start] == ' ' || line[start] == '\t') start++;
    if (start > 0) memmove(line, line + start, len - start + 1);
    len = strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[len - 1] = '\0';
        len--;
    }
}

static int is_terminal_line(const char *line) {
    return strcmp(line, "OK") == 0 ||
           strcmp(line, "ERROR") == 0 ||
           strncmp(line, "+CMS ERROR:", 11) == 0 ||
           strncmp(line, "+CME ERROR:", 11) == 0;
}

static int is_error_line(const char *line) {
    return strcmp(line, "ERROR") == 0 ||
           strncmp(line, "+CMS ERROR:", 11) == 0 ||
           strncmp(line, "+CME ERROR:", 11) == 0;
}

static void normalize_command(const char *input, char *output, size_t output_size) {
    size_t i = 0;
    size_t j = 0;
    char cleaned[WS_TEXT_MAX];

    memset(cleaned, 0, sizeof(cleaned));
    while (input[i] != '\0' && j + 1 < sizeof(cleaned)) {
        if (input[i] == '\r') {
            i++;
            continue;
        }
        if (input[i] == '\n') {
            break;
        }
        cleaned[j++] = input[i++];
    }
    cleaned[j] = '\0';

    if (strncmp(cleaned, "AT^SYSCFGEX", 11) == 0) {
        char *ok = strstr(cleaned, "OK");
        if (ok != NULL) *ok = '\0';
    }
    snprintf(output, output_size, "%s\r", cleaned);
}

static void json_escape(const char *src, char *dst, size_t dst_size) {
    size_t i;
    size_t j = 0;

    for (i = 0; src[i] != '\0' && j + 2 < dst_size; ++i) {
        switch (src[i]) {
            case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
            case '"': dst[j++] = '\\'; dst[j++] = '"'; break;
            case '\n': dst[j++] = '\\'; dst[j++] = 'n'; break;
            case '\r': dst[j++] = '\\'; dst[j++] = 'r'; break;
            case '\t': dst[j++] = '\\'; dst[j++] = 't'; break;
            default: dst[j++] = src[i]; break;
        }
    }
    dst[j] = '\0';
}

static char *json_escape_alloc(const char *src) {
    size_t src_len;
    size_t dst_size;
    char *dst;

    if (src == NULL) src = "";
    src_len = strlen(src);
    dst_size = src_len * 2 + 1;
    dst = (char *) malloc(dst_size);
    if (dst == NULL) return NULL;
    json_escape(src, dst, dst_size);
    return dst;
}

static char *build_response_json_alloc(const at_response_t *response) {
    char *data = NULL;
    char *error = NULL;
    char *json;
    size_t json_size;

    if (response->data[0]) {
        data = json_escape_alloc(response->data);
        if (data == NULL) return NULL;
    }
    if (response->error[0]) {
        error = json_escape_alloc(response->error);
        if (error == NULL) {
            free(data);
            return NULL;
        }
    }

    if (data != NULL && error != NULL) {
        json_size = strlen(data) + strlen(error) + 64;
    } else if (data != NULL) {
        json_size = strlen(data) + 64;
    } else if (error != NULL) {
        json_size = strlen(error) + 64;
    } else {
        json_size = 64;
    }

    json = (char *) malloc(json_size);
    if (json == NULL) {
        free(data);
        free(error);
        return NULL;
    }

    if (data != NULL && error != NULL) {
        snprintf(json, json_size, "{\"success\":%s,\"data\":\"%s\",\"error\":\"%s\"}",
                 response->success ? "true" : "false", data, error);
    } else if (data != NULL) {
        snprintf(json, json_size, "{\"success\":%s,\"data\":\"%s\",\"error\":null}",
                 response->success ? "true" : "false", data);
    } else if (error != NULL) {
        snprintf(json, json_size, "{\"success\":%s,\"data\":null,\"error\":\"%s\"}",
                 response->success ? "true" : "false", error);
    } else {
        snprintf(json, json_size, "{\"success\":%s,\"data\":null,\"error\":null}",
                 response->success ? "true" : "false");
    }

    free(data);
    free(error);
    return json;
}

static int ensure_parent_dir(const char *path) {
    char dir[256];
    char *slash;
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash == NULL) return 0;
    *slash = '\0';
    if (dir[0] == '\0') return 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' >/dev/null 2>&1", dir);
    return system(cmd);
}

static int notification_enabled(int type) {
    switch (type) {
        case NOTIFY_SMS: return g_app.config.notify_sms;
        case NOTIFY_CALL: return g_app.config.notify_call;
        case NOTIFY_MEMORY_FULL: return g_app.config.notify_memory_full;
        case NOTIFY_SIGNAL: return g_app.config.notify_signal;
        default: return 1;
    }
}

static void write_log_notification(const char *title, const char *content, int type) {
    FILE *fp;
    char ts[64];
    time_t now;
    struct tm tm_now;

    if (!g_app.config.log_file[0] || !notification_enabled(type)) return;
    ensure_parent_dir(g_app.config.log_file);
    fp = fopen(g_app.config.log_file, "a");
    if (fp == NULL) return;
    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
    fprintf(fp, "[%s] %s\n%s\n--------------------------------------------------\n",
            ts, title ? title : "", content ? content : "");
    fclose(fp);
}

static void shell_escape_single_quotes(const char *src, char *dst, size_t dst_size) {
    size_t i;
    size_t j = 0;
    for (i = 0; src[i] != '\0' && j + 5 < dst_size; ++i) {
        if (src[i] == '\'') {
            dst[j++] = '\'';
            dst[j++] = '\\';
            dst[j++] = '\'';
            dst[j++] = '\'';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static void send_wechat_notification(const char *title, const char *content, int type) {
    char message[2048];
    char escaped_url[1024];
    char escaped_payload[4096];
    char cmd[6144];

    if (!g_app.config.wechat_webhook[0] || !notification_enabled(type)) return;

    snprintf(message, sizeof(message), "%s\n%s", title ? title : "", content ? content : "");
    shell_escape_single_quotes(g_app.config.wechat_webhook, escaped_url, sizeof(escaped_url));
    shell_escape_single_quotes(message, escaped_payload, sizeof(escaped_payload));

    snprintf(
        cmd,
        sizeof(cmd),
        "(command -v curl >/dev/null 2>&1 && "
        "curl -k -sS -m 5 -H 'Content-Type: application/json' "
        "-d '{\"msgtype\":\"text\",\"text\":{\"content\":\"%s\"}}' '%s' >/dev/null 2>&1) || "
        "(command -v wget >/dev/null 2>&1 && "
        "wget -q -T 5 --header='Content-Type: application/json' "
        "--post-data='{\"msgtype\":\"text\",\"text\":{\"content\":\"%s\"}}' -O - '%s' >/dev/null 2>&1)",
        escaped_payload, escaped_url, escaped_payload, escaped_url
    );
    system(cmd);
}

static void notify_event(const char *title, const char *content, int type) {
    if (!notification_enabled(type)) return;
    write_log_notification(title, content, type);
    send_wechat_notification(title, content, type);
}

static uint32_t rol32(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_process_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int t;

    for (t = 0; t < 16; ++t) {
        w[t] = ((uint32_t) block[t * 4] << 24) |
               ((uint32_t) block[t * 4 + 1] << 16) |
               ((uint32_t) block[t * 4 + 2] << 8) |
               (uint32_t) block[t * 4 + 3];
    }
    for (t = 16; t < 80; ++t) {
        w[t] = rol32(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (t = 0; t < 80; ++t) {
        uint32_t f, k, temp;
        if (t < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        temp = rol32(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t state[5] = {
        0x67452301,
        0xEFCDAB89,
        0x98BADCFE,
        0x10325476,
        0xC3D2E1F0
    };
    uint8_t block[64];
    size_t offset = 0;
    uint64_t bits = (uint64_t) len * 8;

    while (len - offset >= 64) {
        sha1_process_block(state, data + offset);
        offset += 64;
    }

    memset(block, 0, sizeof(block));
    memcpy(block, data + offset, len - offset);
    block[len - offset] = 0x80;

    if ((len - offset) >= 56) {
        sha1_process_block(state, block);
        memset(block, 0, sizeof(block));
    }

    block[56] = (uint8_t) (bits >> 56);
    block[57] = (uint8_t) (bits >> 48);
    block[58] = (uint8_t) (bits >> 40);
    block[59] = (uint8_t) (bits >> 32);
    block[60] = (uint8_t) (bits >> 24);
    block[61] = (uint8_t) (bits >> 16);
    block[62] = (uint8_t) (bits >> 8);
    block[63] = (uint8_t) bits;
    sha1_process_block(state, block);

    for (int i = 0; i < 5; ++i) {
        out[i * 4] = (uint8_t) (state[i] >> 24);
        out[i * 4 + 1] = (uint8_t) (state[i] >> 16);
        out[i * 4 + 2] = (uint8_t) (state[i] >> 8);
        out[i * 4 + 3] = (uint8_t) state[i];
    }
}

static void base64_encode(const uint8_t *input, size_t len, char *output, size_t out_size) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    size_t j = 0;
    for (i = 0; i < len && j + 4 < out_size; i += 3) {
        uint32_t a = input[i];
        uint32_t b = (i + 1 < len) ? input[i + 1] : 0;
        uint32_t c = (i + 2 < len) ? input[i + 2] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        output[j++] = table[(triple >> 18) & 0x3F];
        output[j++] = table[(triple >> 12) & 0x3F];
        output[j++] = (i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=';
        output[j++] = (i + 2 < len) ? table[triple & 0x3F] : '=';
    }
    output[j] = '\0';
}

static int write_all_fd(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *) buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t) n;
    }
    return 0;
}

static int read_exact_fd(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *) buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t) n;
    }
    return 0;
}

static int websocket_send_text(int fd, const char *text) {
    uint8_t header[10];
    size_t payload_len = strlen(text);
    size_t header_len = 0;

    header[0] = 0x81;
    if (payload_len < 126) {
        header[1] = (uint8_t) payload_len;
        header_len = 2;
    } else if (payload_len <= 0xFFFF) {
        header[1] = 126;
        header[2] = (payload_len >> 8) & 0xFF;
        header[3] = payload_len & 0xFF;
        header_len = 4;
    } else {
        uint64_t n = (uint64_t) payload_len;
        header[1] = 127;
        header[2] = (uint8_t) ((n >> 56) & 0xFF);
        header[3] = (uint8_t) ((n >> 48) & 0xFF);
        header[4] = (uint8_t) ((n >> 40) & 0xFF);
        header[5] = (uint8_t) ((n >> 32) & 0xFF);
        header[6] = (uint8_t) ((n >> 24) & 0xFF);
        header[7] = (uint8_t) ((n >> 16) & 0xFF);
        header[8] = (uint8_t) ((n >> 8) & 0xFF);
        header[9] = (uint8_t) (n & 0xFF);
        header_len = 10;
    }

    if (write_all_fd(fd, header, header_len) < 0) return -1;
    if (write_all_fd(fd, text, payload_len) < 0) return -1;
    return 0;
}

static void remove_client_fd(int fd) {
    pthread_mutex_lock(&g_app.clients_mutex);
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (g_app.clients[i].active && g_app.clients[i].fd == fd) {
            g_app.clients[i].active = 0;
            close(g_app.clients[i].fd);
            break;
        }
    }
    pthread_mutex_unlock(&g_app.clients_mutex);
}

static void broadcast_json(const char *json) {
    int fds[WS_MAX_CLIENTS];
    int count = 0;

    pthread_mutex_lock(&g_app.clients_mutex);
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (g_app.clients[i].active && g_app.clients[i].authed) {
            fds[count++] = g_app.clients[i].fd;
        }
    }
    pthread_mutex_unlock(&g_app.clients_mutex);

    for (int i = 0; i < count; ++i) {
        if (websocket_send_text(fds[i], json) < 0) {
            remove_client_fd(fds[i]);
        }
    }
}

static void broadcast_raw_data(const char *line) {
    char *escaped;
    char *json;
    size_t json_size;

    escaped = json_escape_alloc(line);
    if (escaped == NULL) return;
    json_size = strlen(escaped) + 64;
    json = (char *) malloc(json_size);
    if (json != NULL) {
        snprintf(json, json_size, "{\"type\":\"raw_data\",\"data\":\"%s\"}", escaped);
        broadcast_json(json);
        free(json);
    }
    free(escaped);
}

static void broadcast_incoming_call(const char *number, const char *state) {
    char escaped[256];
    char json[512];
    char ts[64];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
    json_escape(number ? number : "", escaped, sizeof(escaped));
    snprintf(json, sizeof(json),
             "{\"type\":\"incoming_call\",\"data\":{\"time\":\"%s\",\"number\":\"%s\",\"state\":\"%s\"}}",
             ts, escaped, state);
    broadcast_json(json);
}

static void broadcast_pdcp_data(const char *line) {
    char copy[512];
    char *parts[16];
    char *tok;
    int count = 0;
    char json[1024];

    snprintf(copy, sizeof(copy), "%s", line);
    tok = strtok(copy + 14, ",");
    while (tok != NULL && count < 16) {
        parts[count++] = tok;
        tok = strtok(NULL, ",");
    }

    if (count >= 14) {
        snprintf(
            json,
            sizeof(json),
            "{\"type\":\"pdcp_data\",\"data\":{\"id\":%d,\"pduSessionId\":%d,\"discardTimerLen\":%d,"
            "\"avgDelay\":%.1f,\"minDelay\":%.1f,\"maxDelay\":%.1f,"
            "\"highPriQueMaxBuffTime\":%.1f,\"lowPriQueMaxBuffTime\":%.1f,"
            "\"highPriQueBuffPktNums\":%d,\"lowPriQueBuffPktNums\":%d,"
            "\"ulPdcpRate\":%d,\"dlPdcpRate\":%d,\"ulDiscardCnt\":%d,\"dlDiscardCnt\":%d}}",
            atoi(parts[0]), atoi(parts[1]), atoi(parts[2]),
            atof(parts[3]) / 10.0, atof(parts[4]) / 10.0, atof(parts[5]) / 10.0,
            atof(parts[6]) / 10.0, atof(parts[7]) / 10.0,
            atoi(parts[8]), atoi(parts[9]), atoi(parts[10]), atoi(parts[11]),
            atoi(parts[12]), atoi(parts[13])
        );
        broadcast_json(json);
        return;
    }

    broadcast_raw_data(line);
}

static void broadcast_sms_notice(const char *sender, const char *content, const char *timestamp) {
    char *sender_escaped;
    char *content_escaped;
    char *ts_escaped;
    char *json;
    size_t json_size;

    sender_escaped = json_escape_alloc(sender ? sender : "");
    content_escaped = json_escape_alloc(content ? content : "");
    ts_escaped = json_escape_alloc(timestamp ? timestamp : "");
    if (sender_escaped == NULL || content_escaped == NULL || ts_escaped == NULL) {
        free(sender_escaped);
        free(content_escaped);
        free(ts_escaped);
        return;
    }

    json_size = strlen(sender_escaped) + strlen(content_escaped) + strlen(ts_escaped) + 96;
    json = (char *) malloc(json_size);
    if (json != NULL) {
        snprintf(json, json_size,
                 "{\"type\":\"new_sms\",\"data\":{\"sender\":\"%s\",\"content\":\"%s\",\"time\":\"%s\"}}",
                 sender_escaped, content_escaped, ts_escaped);
        broadcast_json(json);
        free(json);
    }

    free(sender_escaped);
    free(content_escaped);
    free(ts_escaped);
}

static void broadcast_memory_full(void) {
    broadcast_json("{\"type\":\"memory_full\",\"data\":{\"message\":\"短信存储空间已满\"}}");
    notify_event("存储空间已满", "请及时清理短信存储空间，否则可能无法接收新短信。", NOTIFY_MEMORY_FULL);
}

static char gsm7_to_char(uint8_t septet) {
    if (septet == 10) return '\n';
    if (septet == 13) return '\r';
    if (septet >= 32 && septet <= 126) return (char) septet;
    if (septet >= '0' && septet <= '9') return (char) septet;
    if (septet >= 'A' && septet <= 'Z') return (char) septet;
    if (septet >= 'a' && septet <= 'z') return (char) septet;
    return '?';
}

static void notify_sms_message(const char *sender, const char *content, const char *timestamp) {
    char *log_content;
    size_t log_size;

    broadcast_sms_notice(sender, content, timestamp);

    log_size = strlen(sender ? sender : "") + strlen(content ? content : "") + strlen(timestamp ? timestamp : "") + 64;
    log_content = (char *) malloc(log_size);
    if (log_content == NULL) return;
    snprintf(log_content, log_size, "发送者: %s\n内容: %s\n时间: %s",
             sender ? sender : "", content ? content : "", timestamp ? timestamp : "");
    notify_event("新短信通知", log_content, NOTIFY_SMS);
    free(log_content);
}

static void decode_number(const uint8_t *bytes, int number_length, char *out, size_t out_size) {
    size_t pos = 0;
    for (int i = 0; i < (number_length + 1) / 2 && pos + 1 < out_size; ++i) {
        int digit1 = bytes[i] & 0x0F;
        int digit2 = bytes[i] >> 4;
        if (digit1 <= 9 && pos + 1 < out_size) out[pos++] = (char) ('0' + digit1);
        if ((int) pos < number_length && digit2 <= 9 && pos + 1 < out_size) out[pos++] = (char) ('0' + digit2);
    }
    out[pos] = '\0';
}

static void decode_timestamp_bytes(const uint8_t *bytes, char *out, size_t out_size) {
    int year = 2000 + ((bytes[0] & 0x0F) * 10) + (bytes[0] >> 4);
    int month = ((bytes[1] & 0x0F) * 10) + (bytes[1] >> 4);
    int day = ((bytes[2] & 0x0F) * 10) + (bytes[2] >> 4);
    int hour = ((bytes[3] & 0x0F) * 10) + (bytes[3] >> 4);
    int minute = ((bytes[4] & 0x0F) * 10) + (bytes[4] >> 4);
    int second = ((bytes[5] & 0x0F) * 10) + (bytes[5] >> 4);
    snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute, second);
}

static void decode_ucs2(const uint8_t *bytes, size_t len, char *out, size_t out_size) {
    size_t pos = 0;
    for (size_t i = 0; i + 1 < len && pos + 4 < out_size; i += 2) {
        uint16_t ch = (uint16_t) ((bytes[i] << 8) | bytes[i + 1]);
        if (ch < 0x80) {
            out[pos++] = (char) ch;
        } else if (ch < 0x800) {
            out[pos++] = (char) (0xC0 | (ch >> 6));
            out[pos++] = (char) (0x80 | (ch & 0x3F));
        } else {
            out[pos++] = (char) (0xE0 | (ch >> 12));
            out[pos++] = (char) (0x80 | ((ch >> 6) & 0x3F));
            out[pos++] = (char) (0x80 | (ch & 0x3F));
        }
    }
    out[pos] = '\0';
}

static void decode_7bit(const uint8_t *bytes, size_t len, int expected_length, char *out, size_t out_size) {
    uint32_t acc = 0;
    int bits = 0;
    int count = 0;
    size_t pos = 0;

    for (size_t i = 0; i < len && count < expected_length && pos + 2 < out_size; ++i) {
        acc |= ((uint32_t) bytes[i]) << bits;
        bits += 8;
        while (bits >= 7 && count < expected_length && pos + 2 < out_size) {
            uint8_t septet = acc & 0x7F;
            out[pos++] = gsm7_to_char(septet);
            acc >>= 7;
            bits -= 7;
            count++;
        }
    }
    out[pos] = '\0';
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_size) {
    size_t len = strlen(hex);
    size_t count = 0;
    char byte_str[3];

    if (len % 2 != 0) return -1;
    byte_str[2] = '\0';
    for (size_t i = 0; i + 1 < len && count < out_size; i += 2) {
        byte_str[0] = hex[i];
        byte_str[1] = hex[i + 1];
        out[count++] = (uint8_t) strtoul(byte_str, NULL, 16);
    }
    return (int) count;
}

static int decode_incoming_pdu(
    const char *pdu_hex,
    char *sender,
    size_t sender_size,
    char *content,
    size_t content_size,
    char *timestamp,
    size_t ts_size,
    sms_partial_t *partial
) {
    uint8_t pdu[512];
    int pdu_len = hex_to_bytes(pdu_hex, pdu, sizeof(pdu));
    int pos = 0;
    int smsc_length;
    int sender_length;
    int dcs;
    int user_data_length;
    int udh_length = 0;
    uint8_t *data_bytes;
    int data_len;

    if (partial != NULL) memset(partial, 0, sizeof(*partial));
    if (pdu_len <= 0) return -1;
    smsc_length = pdu[pos];
    pos += 1 + smsc_length;
    if (pos + 2 >= pdu_len) return -1;
    pos += 1;
    sender_length = pdu[pos++];
    pos += 1;
    if (pos + (sender_length + 1) / 2 >= pdu_len) return -1;
    decode_number(&pdu[pos], sender_length, sender, sender_size);
    pos += (sender_length + 1) / 2;
    pos += 1;
    dcs = pdu[pos++];
    if (pos + 7 >= pdu_len) return -1;
    decode_timestamp_bytes(&pdu[pos], timestamp, ts_size);
    pos += 7;
    if (pos >= pdu_len) return -1;
    user_data_length = pdu[pos++];
    data_bytes = &pdu[pos];
    data_len = pdu_len - pos;

    if (data_len <= 0) return -1;
    if (pdu[1 + smsc_length] & 0x40) {
        udh_length = data_bytes[0] + 1;
        if (udh_length > data_len) udh_length = 0;
        if (udh_length >= 6 && partial != NULL) {
            uint8_t iei = data_bytes[1];
            if (iei == 0x00 && data_bytes[2] == 0x03) {
                partial->reference = data_bytes[3];
                partial->total = data_bytes[4];
                partial->seq = data_bytes[5];
                partial->valid = 1;
            } else if (iei == 0x08 && data_bytes[2] == 0x04 && udh_length >= 7) {
                partial->reference = ((int) data_bytes[3] << 8) | data_bytes[4];
                partial->total = data_bytes[5];
                partial->seq = data_bytes[6];
                partial->valid = 1;
            }
        }
    }

    if ((dcs & 0x0F) == 0x08) {
        decode_ucs2(data_bytes + udh_length, (size_t) (data_len - udh_length), content, content_size);
    } else {
        decode_7bit(data_bytes + udh_length, (size_t) (data_len - udh_length), user_data_length, content, content_size);
    }
    return 0;
}

static void process_cmgr_response(const char *response_lines) {
    char *copy;
    char *saveptr = NULL;
    char *line;
    char *pdu_line = NULL;
    char sender[128] = "";
    char *content;
    char timestamp[64] = "";
    sms_partial_t partial;

    if (response_lines == NULL) return;
    copy = (char *) malloc(strlen(response_lines) + 1);
    content = (char *) calloc(1, AT_RESPONSE_MAX);
    if (copy == NULL || content == NULL) {
        free(copy);
        free(content);
        return;
    }

    memset(&partial, 0, sizeof(partial));
    snprintf(copy, strlen(response_lines) + 1, "%s", response_lines);
    line = strtok_r(copy, "\r\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "+CMGR:", 6) == 0) {
            line = strtok_r(NULL, "\r\n", &saveptr);
            if (line != NULL && strcmp(line, "OK") != 0 && strcmp(line, "ERROR") != 0) {
                pdu_line = line;
                break;
            }
        }
        line = strtok_r(NULL, "\r\n", &saveptr);
    }

    if (pdu_line != NULL &&
        decode_incoming_pdu(
            pdu_line,
            sender,
            sizeof(sender),
            content,
            AT_RESPONSE_MAX,
            timestamp,
            sizeof(timestamp),
            &partial
        ) == 0) {
        if (partial.valid && partial.total > 1 && partial.total <= 16 && partial.seq >= 1 && partial.seq <= partial.total) {
            pthread_mutex_lock(&g_partial_sms_mutex);
            partial_sms_entry_t *entry = NULL;
            time_t now = time(NULL);
            for (int i = 0; i < 16; ++i) {
                if (g_partial_sms[i].in_use && now - g_partial_sms[i].first_seen > 600) {
                    g_partial_sms[i].in_use = 0;
                }
            }
            for (int i = 0; i < 16; ++i) {
                if (g_partial_sms[i].in_use && g_partial_sms[i].ref == partial.reference &&
                    strcmp(g_partial_sms[i].sender, sender) == 0) {
                    entry = &g_partial_sms[i];
                    break;
                }
            }
            if (entry == NULL) {
                for (int i = 0; i < 16; ++i) {
                    if (!g_partial_sms[i].in_use) {
                        entry = &g_partial_sms[i];
                        memset(entry, 0, sizeof(*entry));
                        entry->in_use = 1;
                        entry->ref = partial.reference;
                        snprintf(entry->sender, sizeof(entry->sender), "%s", sender);
                        entry->total = partial.total;
                        entry->first_seen = now;
                        snprintf(entry->timestamp, sizeof(entry->timestamp), "%s", timestamp);
                        break;
                    }
                }
            }
            if (entry != NULL) {
                snprintf(entry->parts[partial.seq - 1], sizeof(entry->parts[0]), "%s", content);
                entry->received_mask |= (1 << (partial.seq - 1));
                int full_mask = (1 << entry->total) - 1;
                if ((entry->received_mask & full_mask) == full_mask) {
                    size_t full_size = (size_t) entry->total * sizeof(entry->parts[0]) + 1;
                    char *full_content = (char *) calloc(1, full_size);
                    if (full_content != NULL) {
                        for (int i = 0; i < entry->total; ++i) {
                            strncat(full_content, entry->parts[i], full_size - strlen(full_content) - 1);
                        }
                        notify_sms_message(entry->sender, full_content, entry->timestamp);
                        free(full_content);
                    }
                    entry->in_use = 0;
                }
            }
            pthread_mutex_unlock(&g_partial_sms_mutex);
        } else {
            notify_sms_message(sender, content, timestamp);
        }
    }
    free(copy);
    free(content);
}

static void fetch_and_broadcast_sms(const char *index_text) {
    at_response_t *response;
    char command[64];

    if (index_text == NULL || *index_text == '\0') return;
    response = (at_response_t *) calloc(1, sizeof(*response));
    if (response == NULL) return;
    snprintf(command, sizeof(command), "AT+CMGR=%s", index_text);
    if (send_sync_command(&g_app, command, response) == 0 && response->data[0]) {
        process_cmgr_response(response->data);
    }
    free(response);
}

static void *sms_worker_thread(void *arg) {
    app_state_t *app = (app_state_t *) arg;
    char index_text[16];

    while (app->running) {
        if (sms_queue_pop(&app->sms_queue, index_text, sizeof(index_text)) < 0) continue;
        if (index_text[0]) {
            fetch_and_broadcast_sms(index_text);
        }
    }
    return NULL;
}

static void pending_clear(app_state_t *app) {
    memset(&app->pending, 0, sizeof(app->pending));
}

static void pending_cancel_if_matches(app_state_t *app, command_request_t *request) {
    pthread_mutex_lock(&app->pending_mutex);
    if (app->pending.request == request) {
        pending_clear(app);
    }
    pthread_mutex_unlock(&app->pending_mutex);
}

static void pending_complete_locked(app_state_t *app) {
    pending_command_t pending = app->pending;
    const char *last_line = pending.lines;
    char *pos = strrchr(pending.lines, '\n');

    if (pos != NULL) last_line = pos + 1;
    if (*last_line == '\r') last_line++;

    if (is_error_line(last_line)) {
        request_finish(pending.request, 0, "", pending.lines[0] ? pending.lines : "AT command failed");
    } else {
        request_finish(pending.request, 1, pending.lines, "");
    }
    pending_clear(app);
    pthread_mutex_unlock(&app->pending_mutex);
}

static void pending_add_line(app_state_t *app, const char *line) {
    size_t used;
    size_t remain;

    pthread_mutex_lock(&app->pending_mutex);
    if (app->pending.request == NULL) {
        pthread_mutex_unlock(&app->pending_mutex);
        return;
    }
    if (strcmp(line, app->pending.command) == 0) {
        pthread_mutex_unlock(&app->pending_mutex);
        return;
    }

    used = strlen(app->pending.lines);
    remain = sizeof(app->pending.lines) - used - 1;
    if (used > 0 && remain > 2) {
        strncat(app->pending.lines, "\r\n", remain);
        used = strlen(app->pending.lines);
        remain = sizeof(app->pending.lines) - used - 1;
    }
    if (remain > 0) {
        strncat(app->pending.lines, line, remain);
    }

    if (is_terminal_line(line)) {
        pending_complete_locked(app);
        return;
    }
    pthread_mutex_unlock(&app->pending_mutex);
}

static void pending_timeout_check(app_state_t *app) {
    pthread_mutex_lock(&app->pending_mutex);
    if (app->pending.request != NULL && time(NULL) >= app->pending.deadline) {
        request_finish(app->pending.request, 0, "", "AT command timed out");
        pending_clear(app);
    }
    pthread_mutex_unlock(&app->pending_mutex);
}

static int should_broadcast_raw_line(app_state_t *app, const char *line) {
    int has_pending;
    int is_cmd_echo = 0;

    pthread_mutex_lock(&app->pending_mutex);
    has_pending = (app->pending.request != NULL);
    if (has_pending && strcmp(line, app->pending.command) == 0) {
        is_cmd_echo = 1;
    }
    pthread_mutex_unlock(&app->pending_mutex);

    if (is_cmd_echo) return 0;
    if (has_pending) return 0;
    return 1;
}

static void handle_unsolicited_line(const char *line) {
    static char last_clip_number[64];
    static time_t last_call_time = 0;
    static int memory_full_notified = 0;
    static int last_rsrp = 0;
    static int have_last_rsrp = 0;

    if (strstr(line, "RING") != NULL || strstr(line, "IRING") != NULL) {
        time_t now = time(NULL);
        if (now - last_call_time > 30) {
            broadcast_incoming_call(last_clip_number, "ringing");
            {
                char content[512];
                char ts[64];
                struct tm tm_now;
                localtime_r(&now, &tm_now);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
                snprintf(content, sizeof(content), "时间：%s\n号码：%s\n状态：来电振铃", ts, last_clip_number);
                notify_event("来电提醒", content, NOTIFY_CALL);
            }
            last_call_time = now;
        }
        return;
    }
    if (strncmp(line, "+CLIP:", 6) == 0) {
        const char *start = strchr(line, '"');
        if (start != NULL) {
            const char *end = strchr(start + 1, '"');
            if (end != NULL) {
                size_t len = (size_t) (end - start - 1);
                if (len >= sizeof(last_clip_number)) len = sizeof(last_clip_number) - 1;
                memcpy(last_clip_number, start + 1, len);
                last_clip_number[len] = '\0';
            }
        }
        time_t now = time(NULL);
        if (now - last_call_time > 30) {
            broadcast_incoming_call(last_clip_number, "ringing");
            {
                char content[512];
                char ts[64];
                struct tm tm_now;
                localtime_r(&now, &tm_now);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
                snprintf(content, sizeof(content), "时间：%s\n号码：%s\n状态：来电振铃", ts, last_clip_number);
                notify_event("来电提醒", content, NOTIFY_CALL);
            }
            last_call_time = now;
        }
        return;
    }
    if (strncmp(line, "^CEND:", 6) == 0 || strcmp(line, "NO CARRIER") == 0) {
        broadcast_incoming_call(last_clip_number, "ended");
        if (last_clip_number[0]) {
            char content[512];
            char ts[64];
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
            snprintf(content, sizeof(content), "时间：%s\n号码：%s\n状态：通话结束", ts, last_clip_number);
            notify_event("来电提醒", content, NOTIFY_CALL);
        }
        last_clip_number[0] = '\0';
        last_call_time = 0;
        return;
    }
    if (strncmp(line, "^PDCPDATAINFO:", 14) == 0) {
        broadcast_pdcp_data(line);
        return;
    }
    if (strstr(line, "CMS ERROR: 322") != NULL || strstr(line, "MEMORY FULL") != NULL || strstr(line, "^SMMEMFULL") != NULL) {
        if (!memory_full_notified) {
            broadcast_memory_full();
            memory_full_notified = 1;
        }
        return;
    }
    if (strncmp(line, "^HCSQ:", 6) == 0) {
        char copy[256];
        char *parts[8];
        int count = 0;
        int rsrp_raw;
        int rsrp;
        snprintf(copy, sizeof(copy), "%s", line + 6);
        trim_space(copy);
        char *tok = strtok(copy, ",");
        while (tok != NULL && count < 8) {
            parts[count++] = tok;
            tok = strtok(NULL, ",");
        }
        if (count >= 2) {
            rsrp_raw = atoi(parts[1]);
            rsrp = -140 + rsrp_raw;
            if (!have_last_rsrp || abs(rsrp - last_rsrp) >= 1) {
                char msg[256];
                snprintf(msg, sizeof(msg), "时间: %ld\n原始信号: %s\n计算RSRP: %d dBm", (long) time(NULL), line, rsrp);
                notify_event("信号监控", msg, NOTIFY_SIGNAL);
                last_rsrp = rsrp;
                have_last_rsrp = 1;
            }
        }
        return;
    }
    if (strncmp(line, "+CMTI:", 6) == 0) {
        const char *quote1 = strchr(line, '"');
        const char *quote2 = quote1 ? strchr(quote1 + 1, '"') : NULL;
        const char *comma = quote2 ? strchr(quote2, ',') : NULL;
        char storage[16] = "";
        char index_text[16] = "";

        if (quote1 && quote2) {
            size_t len = (size_t) (quote2 - quote1 - 1);
            if (len >= sizeof(storage)) len = sizeof(storage) - 1;
            memcpy(storage, quote1 + 1, len);
            storage[len] = '\0';
        }
        if (comma) {
            snprintf(index_text, sizeof(index_text), "%s", comma + 1);
            trim_eol(index_text);
            trim_space(index_text);
        }
        if (index_text[0]) {
            if (sms_queue_push(&g_app.sms_queue, index_text) < 0) {
                broadcast_sms_notice(storage[0] ? storage : "ME", "new sms", "");
            }
        } else {
            broadcast_sms_notice(storage[0] ? storage : "ME", "new sms", "");
        }
    }
}

static void *reader_thread(void *arg) {
    app_state_t *app = (app_state_t *) arg;
    char read_buf[AT_READ_BUF];
    char line_buf[AT_LINE_MAX];
    size_t line_len = 0;

    while (app->running) {
        ssize_t n = read(app->transport.fd, read_buf, sizeof(read_buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                pending_timeout_check(app);
                usleep(10000);
                continue;
            }
            break;
        }
        if (n == 0) {
            pending_timeout_check(app);
            usleep(10000);
            continue;
        }

        for (ssize_t i = 0; i < n; ++i) {
            char c = read_buf[i];
            if (c == '\r') continue;
            if (c == '\n') {
                if (line_len == 0) continue;
                line_buf[line_len] = '\0';
                trim_eol(line_buf);
                if (line_buf[0]) {
                    LOGF("AT raw: %s\n", line_buf);
                    if (should_broadcast_raw_line(app, line_buf)) {
                        broadcast_raw_data(line_buf);
                    }
                    handle_unsolicited_line(line_buf);
                    pending_add_line(app, line_buf);
                }
                line_len = 0;
                continue;
            }
            if (line_len + 1 < sizeof(line_buf)) {
                line_buf[line_len++] = c;
            }
        }
    }

    pthread_mutex_lock(&app->pending_mutex);
    if (app->pending.request != NULL) {
        request_finish(app->pending.request, 0, "", "AT reader stopped");
        pending_clear(app);
    }
    pthread_mutex_unlock(&app->pending_mutex);
    return NULL;
}

static int send_sync_command(app_state_t *app, const char *command, at_response_t *response) {
    command_request_t *request;
    char wire[WS_TEXT_MAX];
    struct timespec ts;
    int rc;

    request = (command_request_t *) calloc(1, sizeof(*request));
    if (request == NULL) return -1;
    request_init(request, command);
    normalize_command(command, wire, sizeof(wire));

    while (app->running) {
        int has_pending;
        pthread_mutex_lock(&app->pending_mutex);
        has_pending = (app->pending.request != NULL);
        pthread_mutex_unlock(&app->pending_mutex);
        if (!has_pending) break;
        usleep(10000);
    }

    pthread_mutex_lock(&app->pending_mutex);
    snprintf(app->pending.command, sizeof(app->pending.command), "%s", wire);
    trim_eol(app->pending.command);
    app->pending.lines[0] = '\0';
    app->pending.request = request;
    app->pending.deadline = time(NULL) + 2;
    pthread_mutex_unlock(&app->pending_mutex);

    if (transport_write_all(&app->transport, wire, strlen(wire)) < 0) {
        pthread_mutex_lock(&app->pending_mutex);
        pending_clear(app);
        pthread_mutex_unlock(&app->pending_mutex);
        request_destroy(request);
        free(request);
        return -1;
    }

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 3;

    pthread_mutex_lock(&request->mutex);
    while (!request->finished) {
        if (pthread_cond_timedwait(&request->cond, &request->mutex, &ts) == ETIMEDOUT) break;
    }
    if (!request->finished) {
        request_finish(request, 0, "", "AT response timed out");
        pending_cancel_if_matches(app, request);
    }
    *response = request->response;
    pthread_mutex_unlock(&request->mutex);
    rc = response->success ? 0 : -1;
    request_destroy(request);
    free(request);
    return rc;
}

static int init_modem(app_state_t *app) {
    at_response_t *response;

    response = (at_response_t *) calloc(1, sizeof(*response));
    if (response == NULL) return -1;

    if (send_sync_command(app, "AT+CNMI?", response) == 0) {
        if (strstr(response->data, "+CNMI: 2,1,0,2,0") == NULL) {
            send_sync_command(app, "AT+CNMI=2,1,0,2,0", response);
        }
    }
    if (send_sync_command(app, "AT+CMGF?", response) == 0) {
        if (strstr(response->data, "+CMGF: 0") == NULL) {
            send_sync_command(app, "AT+CMGF=0", response);
        }
    }
    send_sync_command(app, "AT+CLIP=1", response);
    free(response);
    return 0;
}

static void *dispatch_thread(void *arg) {
    app_state_t *app = (app_state_t *) arg;
    char wire[WS_TEXT_MAX];

    while (app->running) {
        command_request_t *request = queue_pop(&app->queue);
        if (request == NULL) continue;

        if (strcmp(request->command, "AT+CONNECT?") == 0) {
            request_finish(request, 1,
                           app->config.connection_type == AT_TYPE_NETWORK ? "+CONNECT: 0\r\nOK" : "+CONNECT: 1\r\nOK",
                           "");
            continue;
        }

        normalize_command(request->command, wire, sizeof(wire));
        trim_eol(wire);

        pthread_mutex_lock(&app->pending_mutex);
        snprintf(app->pending.command, sizeof(app->pending.command), "%s", wire);
        app->pending.lines[0] = '\0';
        app->pending.request = request;
        app->pending.deadline = time(NULL) + 2;
        pthread_mutex_unlock(&app->pending_mutex);

        snprintf(wire, sizeof(wire), "%s\r", app->pending.command);
        LOGF("AT command: %s\n", request->command);
        if (transport_write_all(&app->transport, wire, strlen(wire)) < 0) {
            pthread_mutex_lock(&app->pending_mutex);
            pending_clear(app);
            pthread_mutex_unlock(&app->pending_mutex);
            request_finish(request, 0, "", "AT write failed");
        }
    }
    return NULL;
}

static int websocket_read_frame(int fd, char *text, size_t text_size) {
    uint8_t hdr[2];
    uint8_t ext[8];
    uint8_t mask[4];
    size_t payload_len;
    uint8_t opcode;

    if (read_exact_fd(fd, hdr, 2) < 0) return -1;
    opcode = hdr[0] & 0x0F;
    payload_len = hdr[1] & 0x7F;
    if (!(hdr[1] & 0x80)) return -1;
    if (opcode == 0x8) return -1;
    if (payload_len == 126) {
        if (read_exact_fd(fd, ext, 2) < 0) return -1;
        payload_len = ((size_t) ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        if (read_exact_fd(fd, ext, 8) < 0) return -1;
        payload_len =
            ((size_t) ext[0] << 56) | ((size_t) ext[1] << 48) |
            ((size_t) ext[2] << 40) | ((size_t) ext[3] << 32) |
            ((size_t) ext[4] << 24) | ((size_t) ext[5] << 16) |
            ((size_t) ext[6] << 8) | (size_t) ext[7];
    }
    if (payload_len + 1 > text_size) return -1;
    if (read_exact_fd(fd, mask, 4) < 0) return -1;
    if (read_exact_fd(fd, text, payload_len) < 0) return -1;
    for (size_t i = 0; i < payload_len; ++i) {
        text[i] ^= mask[i % 4];
    }
    text[payload_len] = '\0';
    return 0;
}

static int websocket_handshake(int fd) {
    char buffer[4096];
    char *key;
    char *line_end;
    char accept_source[256];
    char accept_value[64];
    uint8_t sha_out[20];
    char response[512];
    const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);

    if (n <= 0) return -1;
    buffer[n] = '\0';
    key = strstr(buffer, "Sec-WebSocket-Key:");
    if (key == NULL) return -1;
    key += strlen("Sec-WebSocket-Key:");
    while (*key == ' ') key++;
    line_end = strstr(key, "\r\n");
    if (line_end == NULL) return -1;
    *line_end = '\0';

    snprintf(accept_source, sizeof(accept_source), "%s%s", key, magic);
    sha1((const uint8_t *) accept_source, strlen(accept_source), sha_out);
    base64_encode(sha_out, sizeof(sha_out), accept_value, sizeof(accept_value));

    snprintf(response, sizeof(response),
             "HTTP/1.1 101 Switching Protocols\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n",
             accept_value);
    return write_all_fd(fd, response, strlen(response));
}

static int extract_auth_key(const char *json, char *out, size_t out_size) {
    const char *key = strstr(json, "\"auth_key\"");
    const char *colon;
    const char *quote1;
    const char *quote2;
    size_t len;

    if (key == NULL) return -1;
    colon = strchr(key, ':');
    if (colon == NULL) return -1;
    quote1 = strchr(colon, '"');
    if (quote1 == NULL) return -1;
    quote2 = strchr(quote1 + 1, '"');
    if (quote2 == NULL) return -1;
    len = (size_t) (quote2 - quote1 - 1);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, quote1 + 1, len);
    out[len] = '\0';
    return 0;
}

static void *heartbeat_thread(void *arg) {
    (void) arg;
    while (g_app.running) {
        sleep(30);
        broadcast_json("ping");
    }
    return NULL;
}

static void *client_thread(void *arg) {
    ws_client_t *client = (ws_client_t *) arg;
    char text[WS_TEXT_MAX];

    if (g_app.config.websocket_auth_key[0]) {
        char client_key[128];
        if (websocket_read_frame(client->fd, text, sizeof(text)) < 0 ||
            extract_auth_key(text, client_key, sizeof(client_key)) < 0 ||
            strcmp(client_key, g_app.config.websocket_auth_key) != 0) {
            websocket_send_text(client->fd, "{\"error\":\"Authentication failed\",\"message\":\"密钥验证失败\"}");
            remove_client_fd(client->fd);
            return NULL;
        }
        client->authed = 1;
        websocket_send_text(client->fd, "{\"success\":true,\"message\":\"认证成功\"}");
    } else {
        client->authed = 1;
    }

    while (g_app.running && client->active) {
        command_request_t *request;
        char *response_json;
        struct timespec ts;
        const char *fallback_json = "{\"success\":false,\"data\":null,\"error\":\"out of memory\"}";
        const char *send_json;

        if (websocket_read_frame(client->fd, text, sizeof(text)) < 0) break;
        if (strcmp(text, "ping") == 0) {
            if (websocket_send_text(client->fd, "pong") < 0) break;
            continue;
        }

        request = (command_request_t *) calloc(1, sizeof(*request));
        if (request == NULL) {
            if (websocket_send_text(client->fd, fallback_json) < 0) break;
            continue;
        }
        request_init(request, text);
        if (queue_push(&g_app.queue, request) < 0) {
            request_finish(request, 0, "", "AT queue is full");
        }

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 4;
        pthread_mutex_lock(&request->mutex);
        while (!request->finished) {
            if (pthread_cond_timedwait(&request->cond, &request->mutex, &ts) == ETIMEDOUT) {
                request->response.success = 0;
                snprintf(request->response.error, sizeof(request->response.error), "%s", "AT response timed out");
                request->response.data[0] = '\0';
                request->finished = 1;
                pending_cancel_if_matches(&g_app, request);
                break;
            }
        }
        response_json = build_response_json_alloc(&request->response);
        pthread_mutex_unlock(&request->mutex);
        send_json = (response_json != NULL) ? response_json : fallback_json;
        if (websocket_send_text(client->fd, send_json) < 0) {
            free(response_json);
            request_destroy(request);
            free(request);
            break;
        }
        free(response_json);
        request_destroy(request);
        free(request);
    }

    remove_client_fd(client->fd);
    return NULL;
}

static int add_client(int fd) {
    pthread_mutex_lock(&g_app.clients_mutex);
    for (int i = 0; i < WS_MAX_CLIENTS; ++i) {
        if (!g_app.clients[i].active) {
            g_app.clients[i].fd = fd;
            g_app.clients[i].active = 1;
            g_app.clients[i].authed = 0;
            pthread_mutex_unlock(&g_app.clients_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&g_app.clients_mutex);
    return -1;
}

static int start_websocket_server(const app_config_t *config) {
    int fd;
    int opt = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) config->websocket_port);
    if (strcmp(config->websocket_host, "0.0.0.0") == 0 || strcmp(config->websocket_host, "*") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, config->websocket_host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int start_websocket_server_v6(const app_config_t *config) {
    int fd;
    int opt = 1;
    struct sockaddr_in6 addr6;

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons((uint16_t) config->websocket_port);
    if (strcmp(config->websocket_host_v6, "::") == 0 || strcmp(config->websocket_host_v6, "*") == 0) {
        addr6.sin6_addr = in6addr_any;
    } else if (inet_pton(AF_INET6, config->websocket_host_v6, &addr6.sin6_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *) &addr6, sizeof(addr6)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void uci_apply_line(app_config_t *config, const char *line) {
    const char *prefix = "at-webserver.config.";
    const char *key;
    const char *value;
    char key_buf[128];
    size_t key_len;

    if (strncmp(line, prefix, strlen(prefix)) != 0) return;
    key = line + strlen(prefix);
    value = strchr(key, '=');
    if (value == NULL) return;
    key_len = (size_t) (value - key);
    if (key_len >= sizeof(key_buf)) key_len = sizeof(key_buf) - 1;
    memcpy(key_buf, key, key_len);
    key_buf[key_len] = '\0';
    value++;
    while (*value == '\'' || *value == '"') value++;

    char val_buf[256];
    snprintf(val_buf, sizeof(val_buf), "%s", value);
    trim_eol(val_buf);
    size_t len = strlen(val_buf);
    while (len > 0 && (val_buf[len - 1] == '\'' || val_buf[len - 1] == '"')) {
        val_buf[len - 1] = '\0';
        len--;
    }

    if (strcmp(key_buf, "enabled") == 0) {
        config->enabled = atoi(val_buf);
    } else if (strcmp(key_buf, "connection_type") == 0) {
        config->connection_type = (strcmp(val_buf, "SERIAL") == 0) ? AT_TYPE_SERIAL : AT_TYPE_NETWORK;
    } else if (strcmp(key_buf, "network_host") == 0) {
        snprintf(config->network_host, sizeof(config->network_host), "%s", val_buf);
    } else if (strcmp(key_buf, "network_port") == 0) {
        config->network_port = atoi(val_buf);
    } else if (strcmp(key_buf, "network_timeout") == 0) {
        config->network_timeout_sec = atoi(val_buf);
    } else if (strcmp(key_buf, "serial_port") == 0) {
        snprintf(config->serial_port, sizeof(config->serial_port), "%s", val_buf);
    } else if (strcmp(key_buf, "serial_port_custom") == 0) {
        if (strcmp(config->serial_port, "custom") == 0) {
            snprintf(config->serial_port, sizeof(config->serial_port), "%s", val_buf);
        }
    } else if (strcmp(key_buf, "serial_baudrate") == 0) {
        config->serial_baudrate = atoi(val_buf);
    } else if (strcmp(key_buf, "serial_timeout") == 0) {
        config->serial_timeout_sec = atoi(val_buf);
    } else if (strcmp(key_buf, "websocket_port") == 0) {
        config->websocket_port = atoi(val_buf);
    } else if (strcmp(key_buf, "websocket_auth_key") == 0) {
        snprintf(config->websocket_auth_key, sizeof(config->websocket_auth_key), "%s", val_buf);
    } else if (strcmp(key_buf, "wechat_webhook") == 0) {
        snprintf(config->wechat_webhook, sizeof(config->wechat_webhook), "%s", val_buf);
    } else if (strcmp(key_buf, "log_file") == 0) {
        snprintf(config->log_file, sizeof(config->log_file), "%s", val_buf);
    } else if (strcmp(key_buf, "notify_sms") == 0) {
        config->notify_sms = atoi(val_buf);
    } else if (strcmp(key_buf, "notify_call") == 0) {
        config->notify_call = atoi(val_buf);
    } else if (strcmp(key_buf, "notify_memory_full") == 0) {
        config->notify_memory_full = atoi(val_buf);
    } else if (strcmp(key_buf, "notify_signal") == 0) {
        config->notify_signal = atoi(val_buf);
    }
}

static int load_uci_config(app_config_t *config) {
    FILE *fp = popen("uci show at-webserver 2>/dev/null", "r");
    char line[512];

    if (fp == NULL) return -1;
    while (fgets(line, sizeof(line), fp) != NULL) {
        uci_apply_line(config, line);
    }
    pclose(fp);
    return 0;
}

static void on_signal(int signo) {
    (void) signo;
    g_app.running = 0;
    pthread_cond_broadcast(&g_app.queue.cond);
    pthread_cond_broadcast(&g_app.sms_queue.cond);
}

int main(void) {
    pthread_t reader_tid;
    pthread_t dispatch_tid;
    pthread_t heartbeat_tid;
    pthread_t sms_worker_tid;
    int server_fd_v4;
    int server_fd_v6;

    memset(&g_app, 0, sizeof(g_app));
    g_verbose = (getenv("AT_SERVER_VERBOSE") != NULL);
    set_defaults(&g_app.config);
    load_uci_config(&g_app.config);
    print_config(&g_app.config);

    if (!g_app.config.enabled) {
        fprintf(stderr, "service disabled in UCI\n");
        return 1;
    }

    queue_init(&g_app.queue);
    sms_queue_init(&g_app.sms_queue);
    pthread_mutex_init(&g_app.clients_mutex, NULL);
    pthread_mutex_init(&g_app.pending_mutex, NULL);
    pending_clear(&g_app);
    g_app.running = 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (g_app.config.connection_type == AT_TYPE_NETWORK) {
        if (transport_connect_network(&g_app.transport, &g_app.config) < 0) {
            perror("connect network AT");
            return 1;
        }
    } else {
        if (transport_connect_serial(&g_app.transport, &g_app.config) < 0) {
            perror("connect serial AT");
            return 1;
        }
    }

    pthread_create(&reader_tid, NULL, reader_thread, &g_app);
    init_modem(&g_app);
    pthread_create(&dispatch_tid, NULL, dispatch_thread, &g_app);
    pthread_create(&heartbeat_tid, NULL, heartbeat_thread, NULL);
    pthread_create(&sms_worker_tid, NULL, sms_worker_thread, &g_app);

    server_fd_v4 = start_websocket_server(&g_app.config);
    server_fd_v6 = start_websocket_server_v6(&g_app.config);
    if (server_fd_v4 < 0 && server_fd_v6 < 0) {
        perror("start websocket server");
        g_app.running = 0;
        pthread_cond_broadcast(&g_app.queue.cond);
        pthread_cond_broadcast(&g_app.sms_queue.cond);
        pthread_join(reader_tid, NULL);
        pthread_join(dispatch_tid, NULL);
        pthread_join(heartbeat_tid, NULL);
        pthread_join(sms_worker_tid, NULL);
        transport_close(&g_app.transport);
        return 1;
    }

    if (server_fd_v4 >= 0) {
        printf("websocket server listening on ws://%s:%d\n", g_app.config.websocket_host, g_app.config.websocket_port);
    }
    if (server_fd_v6 >= 0) {
        printf("websocket server listening on ws://[%s]:%d\n", g_app.config.websocket_host_v6, g_app.config.websocket_port);
    }

    while (g_app.running) {
        fd_set readfds;
        int maxfd = -1;
        int client_fd = -1;
        int index;

        FD_ZERO(&readfds);
        if (server_fd_v4 >= 0) {
            FD_SET(server_fd_v4, &readfds);
            if (server_fd_v4 > maxfd) maxfd = server_fd_v4;
        }
        if (server_fd_v6 >= 0) {
            FD_SET(server_fd_v6, &readfds);
            if (server_fd_v6 > maxfd) maxfd = server_fd_v6;
        }

        if (maxfd < 0) break;
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (server_fd_v4 >= 0 && FD_ISSET(server_fd_v4, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            client_fd = accept(server_fd_v4, (struct sockaddr *) &client_addr, &client_len);
        } else if (server_fd_v6 >= 0 && FD_ISSET(server_fd_v6, &readfds)) {
            struct sockaddr_in6 client_addr6;
            socklen_t client_len6 = sizeof(client_addr6);
            client_fd = accept(server_fd_v6, (struct sockaddr *) &client_addr6, &client_len6);
        }

        if (client_fd < 0) continue;
        if (websocket_handshake(client_fd) < 0) {
            close(client_fd);
            continue;
        }
        index = add_client(client_fd);
        if (index < 0) {
            close(client_fd);
            continue;
        }
        pthread_create(&g_app.clients[index].thread, NULL, client_thread, &g_app.clients[index]);
        pthread_detach(g_app.clients[index].thread);
    }

    g_app.running = 0;
    pthread_cond_broadcast(&g_app.queue.cond);
    pthread_cond_broadcast(&g_app.sms_queue.cond);
    if (server_fd_v4 >= 0) close(server_fd_v4);
    if (server_fd_v6 >= 0) close(server_fd_v6);
    pthread_join(reader_tid, NULL);
    pthread_join(dispatch_tid, NULL);
    pthread_join(heartbeat_tid, NULL);
    pthread_join(sms_worker_tid, NULL);
    transport_close(&g_app.transport);
    return 0;
}
