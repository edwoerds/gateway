#include "server.h"
#include "json_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

extern volatile sig_atomic_t g_stop;

/* ==================================================================
 * 客户端表
 * ================================================================ */

typedef struct {
    int         fd;              /* socket，-1 = 空位 */
    sub_mask_t  sub_mask;        /* 订阅位掩码，默认 SUB_ALL */
    time_t      last_heartbeat;  /* 最后一次活动时间（收到任何命令即更新） */
} Client;

static Client     g_clients[MAX_CLIENTS];
static int        g_client_count = 0;
static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;

/* 最新传感器值快照 */
static int        g_last_temp, g_last_humi, g_last_light;
static int        g_last_pressure, g_last_gas;
static pthread_mutex_t g_data_lock = PTHREAD_MUTEX_INITIALIZER;

static int   g_epfd = -1;         /* epoll 实例 fd */
static int   g_heartbeat_to = HEARTBEAT_TO;

/* ==================================================================
 * 工具函数
 * ================================================================ */

static void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static time_t now_sec(void)
{
    return time(NULL);
}

/* ==================================================================
 * 客户端管理
 * ================================================================ */

static int add_client(int fd)
{
    pthread_mutex_lock(&g_clients_lock);

    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == -1) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_clients_lock);
        return -1;  /* 满了 */
    }

    g_clients[slot].fd             = fd;
    g_clients[slot].sub_mask       = SUB_ALL;  /* 默认订阅全部 */
    g_clients[slot].last_heartbeat = now_sec();
    g_client_count++;

    printf("[server] client %d connected (total: %d)\n", fd, g_client_count);
    pthread_mutex_unlock(&g_clients_lock);
    return slot;
}

static void remove_client(int fd, int epfd)
{
    pthread_mutex_lock(&g_clients_lock);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == fd) {
            if (epfd >= 0)
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            g_clients[i].fd = -1;
            g_client_count--;
            printf("[server] client %d disconnected (total: %d)\n",
                   fd, g_client_count);
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* ==================================================================
 * 广播（按订阅过滤）
 * ================================================================ */

static void broadcast_json(const char *json_buf, sub_mask_t event_bit)
{
    pthread_mutex_lock(&g_clients_lock);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == -1) continue;
        if (!(g_clients[i].sub_mask & event_bit)) continue;

        ssize_t n = write(g_clients[i].fd, json_buf, strlen(json_buf));
        if (n <= 0) {
            /* 写失败 → 客户端断开 */
            epoll_ctl(g_epfd, EPOLL_CTL_DEL, g_clients[i].fd, NULL);
            close(g_clients[i].fd);
            g_clients[i].fd = -1;
            g_client_count--;
            printf("[server] client write failed, removed (total: %d)\n",
                   g_client_count);
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* ==================================================================
 * JSON 命令处理
 * ================================================================ */

static int write_fd(int fd, const char *msg)
{
    if (!msg) return -1;
    return (int)write(fd, msg, strlen(msg));
}

/* SUB: {"cmd":"SUB","type":["temp","humi"]} */
static void handle_sub(int fd, char types[][16], int count)
{
    sub_mask_t mask = SUB_NONE;
    for (int i = 0; i < count; i++) {
        if      (strcmp(types[i], "temp")     == 0) mask |= SUB_TEMP;
        else if (strcmp(types[i], "humi")     == 0) mask |= SUB_HUMI;
        else if (strcmp(types[i], "light")    == 0) mask |= SUB_LIGHT;
        else if (strcmp(types[i], "pressure") == 0) mask |= SUB_PRESSURE;
        else if (strcmp(types[i], "gas")      == 0) mask |= SUB_GAS;
        else if (strcmp(types[i], "sysinfo")  == 0) mask |= SUB_SYSINFO;
        else if (strcmp(types[i], "all")      == 0) mask |= SUB_ALL;
    }

    pthread_mutex_lock(&g_clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == fd) {
            g_clients[i].sub_mask = mask;
            g_clients[i].last_heartbeat = now_sec();
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* PING → {"result":"PONG"} */
static void handle_ping(int fd)
{
    char buf[128];
    json_serialize_result(buf, sizeof(buf), "PONG", NULL);
    write_fd(fd, buf);

    /* 更新心跳时间 */
    pthread_mutex_lock(&g_clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == fd) {
            g_clients[i].last_heartbeat = now_sec();
            break;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* LIST → {"result":"LIST","data":[...]}\n */
static void handle_list(int fd)
{
    char buf[JSON_BUF_SIZE * 4];
    int pos = 0, remain = sizeof(buf);
    int n;

    n = json_serialize_list_begin(buf, sizeof(buf));
    if (n < 0) return;
    pos = n; remain = sizeof(buf) - pos;

    pthread_mutex_lock(&g_data_lock);
    int count = 0;
    time_t now = now_sec();

    #define APPEND(t, v) do { \
        n = json_serialize_list_item(buf + pos, remain, t, v, now, &count); \
        if (n < 0) break; \
        pos += n; remain -= n; \
    } while(0)

    APPEND(TEMP_READING,     g_last_temp);
    APPEND(HUMID_READING,    g_last_humi);
    APPEND(LIGHT_READING,    g_last_light);
    APPEND(PRESSURE_READING, g_last_pressure);
    APPEND(GAS_READING,      g_last_gas);

    pthread_mutex_unlock(&g_data_lock);

    n = json_serialize_list_end(buf + pos, remain);
    if (n > 0) pos += n;

    write_fd(fd, buf);
}

/* CLIENTS → {"result":"CLIENTS","count":N}\n */
static void handle_clients(int fd)
{
    char buf[128];
    pthread_mutex_lock(&g_clients_lock);
    int c = g_client_count;
    pthread_mutex_unlock(&g_clients_lock);
    snprintf(buf, sizeof(buf), "{\"result\":\"CLIENTS\",\"count\":%d}\n", c);
    write_fd(fd, buf);
}

/* ==================================================================
 * 客户端数据处理
 * ================================================================ */

static int handle_client_data(int fd)
{
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) return -1;

    buf[n] = '\0';

    /* 去掉末尾换行 */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
        buf[--n] = '\0';
    if (n == 0) return 0;

    /* 解析 JSON 命令 */
    CmdType cmd;
    char types[8][16];
    int count = 0;

    if (json_parse_cmd(buf, &cmd, types, 8, &count) < 0) {
        char err[256];
        json_serialize_result(err, sizeof(err), "ERR", "unknown command");
        write_fd(fd, err);
        return 0;
    }

    switch (cmd) {
        case CMD_SUB:     handle_sub(fd, types, count); break;
        case CMD_PING:    handle_ping(fd); break;
        case CMD_LIST:    handle_list(fd); break;
        case CMD_CLIENTS: handle_clients(fd); break;
        default: break;
    }
    return 0;
}

/* ==================================================================
 * 心跳检测
 * ================================================================ */

static void check_heartbeats(void)
{
    time_t now = now_sec();
    pthread_mutex_lock(&g_clients_lock);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd == -1) continue;
        if (now - g_clients[i].last_heartbeat > g_heartbeat_to) {
            int fd = g_clients[i].fd;
            printf("[server] client %d heartbeat timeout (%ds), closing\n",
                   fd, g_heartbeat_to);
            epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            g_clients[i].fd = -1;
            g_client_count--;
        }
    }
    pthread_mutex_unlock(&g_clients_lock);
}

/* ==================================================================
 * 推送给新客户端的全量快照
 * ================================================================ */

static void push_snapshot(int fd)
{
    pthread_mutex_lock(&g_data_lock);

    /* 逐条推送当前值 */
    time_t now = now_sec();
    char tmp[JSON_BUF_SIZE];

    if (json_serialize_event(tmp, sizeof(tmp),
            TEMP_READING, g_last_temp, now) > 0)
        write_fd(fd, tmp);
    if (json_serialize_event(tmp, sizeof(tmp),
            HUMID_READING, g_last_humi, now) > 0)
        write_fd(fd, tmp);
    /* 只推有值的 */
    if (g_last_humi > 0) /* 简单判断：启动后至少有一次采集 */
        write_fd(fd, tmp);
    /* 实际上简单点：推 LIST 快照 */
    pthread_mutex_unlock(&g_data_lock);

    handle_list(fd);
}

/* ==================================================================
 * 服务器主线程
 * ================================================================ */

static void *server_thread_func(void *arg)
{
    int port = *(int *)arg;
    free(arg);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("[server] socket"); return NULL; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[server] bind"); close(listen_fd); return NULL;
    }
    if (listen(listen_fd, 10) < 0) {
        perror("[server] listen"); close(listen_fd); return NULL;
    }
    set_nonblocking(listen_fd);

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) { perror("[server] epoll"); close(listen_fd); return NULL; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    printf("[server] listening on port %d\n", port);

    struct epoll_event events[64];

    while (!g_stop) {
        int nfds = epoll_wait(g_epfd, events, 64, 1000);
        if (nfds < 0 && errno != EINTR) break;

        /* 心跳检测 */
        check_heartbeats();

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listen_fd) {
                /* 新连接 */
                struct sockaddr_in cli_addr;
                socklen_t cli_len = sizeof(cli_addr);
                int client_fd = accept(listen_fd,
                    (struct sockaddr*)&cli_addr, &cli_len);
                if (client_fd < 0) continue;

                set_nonblocking(client_fd);

                if (add_client(client_fd) < 0) {
                    close(client_fd);
                    continue;
                }

                /* 加入 epoll */
                struct epoll_event cev;
                cev.events = EPOLLIN;
                cev.data.fd = client_fd;
                epoll_ctl(g_epfd, EPOLL_CTL_ADD, client_fd, &cev);

                /* 推送全量快照 */
                push_snapshot(client_fd);

            } else {
                /* 已有客户端发数据 */
                int client_fd = events[i].data.fd;
                if (handle_client_data(client_fd) < 0) {
                    remove_client(client_fd, g_epfd);
                }
            }
        }
    }

    close(listen_fd);
    close(g_epfd);
    g_epfd = -1;
    printf("[server] server stopped\n");
    return NULL;
}

/* ==================================================================
 * 对外接口
 * ================================================================ */

void server_start(int port, int heartbeat_timeout)
{
    g_heartbeat_to = (heartbeat_timeout > 0) ? heartbeat_timeout : HEARTBEAT_TO;

    /* 初始化客户端表 */
    for (int i = 0; i < MAX_CLIENTS; i++)
        g_clients[i].fd = -1;

    /* 传 port 到线程 */
    int *p = malloc(sizeof(int));
    *p = port;
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_func, p);
    pthread_detach(tid);
}

static void server_on_sensor_broadcast(EventType type, int value, time_t ts)
{
    char buf[JSON_BUF_SIZE];
    if (json_serialize_event(buf, sizeof(buf), type, value, ts) <= 0)
        return;
    sub_mask_t bit = event_to_sub(type);
    broadcast_json(buf, bit);
}

void server_on_sensor(EventType type, int value)
{
    /* 更新最新值 */
    pthread_mutex_lock(&g_data_lock);
    switch (type) {
        case TEMP_READING:     g_last_temp     = value; break;
        case HUMID_READING:    g_last_humi     = value; break;
        case LIGHT_READING:    g_last_light    = value; break;
        case PRESSURE_READING: g_last_pressure = value; break;
        case GAS_READING:      g_last_gas      = value; break;
        default: break;
    }
    pthread_mutex_unlock(&g_data_lock);

    /* SYS_INFO 走专用序列化（未来），不通过通用事件广播 */
    if (type == SYS_INFO_READING)
        return;

    time_t ts = time(NULL);
    server_on_sensor_broadcast(type, value, ts);
}
