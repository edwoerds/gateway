# 智能家居网关 v2.0 — 架构设计文档

> 版本：v2.0 | 日期：2026-07-30 | 作者：曹彬

---

## 目录

1. [系统架构总览](#1-系统架构总览)
2. [模块设计](#2-模块设计)
3. [线程模型](#3-线程模型)
4. [锁策略](#4-锁策略)
5. [JSON 协议规范](#5-json-协议规范)
6. [订阅机制](#6-订阅机制)
7. [心跳检测](#7-心跳检测)
8. [系统监控模块](#8-系统监控模块)
9. [内核模块](#9-内核模块)
10. [关键时序图](#10-关键时序图)
11. [错误处理策略](#11-错误处理策略)
12. [构建系统设计](#12-构建系统设计)
13. [面试追问预设](#13-面试追问预设)

---

## 1. 系统架构总览

### 1.1 架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                    🏠 Smart Home Gateway v2.0                        │
│                                                                      │
│  用户态 (user space)                                                  │
│  ═══════════════════════════════════════════════════════════════════ │
│                                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌───────┐ │
│  │TempSensor│  │HumiSensor│  │LightSens │  │PressureS │  │GasSens│ │
│  │ pthread  │  │ pthread  │  │ pthread  │  │ pthread  │  │pthread│ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘  └───┬───┘ │
│       │              │              │              │           │     │
│       └──────────────┴──────────────┴──────────────┴───────────┘     │
│                                      │ queue_push()                 │
│                                      ▼                              │
│                        ┌─────────────────────────┐                  │
│                        │  EventQueue (环形缓冲)    │                  │
│                        │  CAP=128, lock+cond      │                  │
│                        └───────────┬─────────────┘                  │
│                                    │ queue_pop()                    │
│                                    ▼                                │
│                        ┌─────────────────────────┐                  │
│                        │  Dispatcher (表驱动分发)  │                  │
│                        │  entries[16], O(n) scan  │                  │
│                        └───────────┬─────────────┘                  │
│                                    │                                │
│             ┌──────────────────────┼──────────────────────┐         │
│             ▼                      ▼                      ▼         │
│      ┌─────────────┐       ┌─────────────┐       ┌─────────────┐   │
│      │ on_temp()   │       │ on_humi()   │       │ on_sysinfo()│   │
│      │ server_on_  │       │ server_on_  │       │ server_on_  │   │
│      │ sensor()    │       │ sensor()    │       │ sensor()    │   │
│      └──────┬──────┘       └──────┬──────┘       └──────┬──────┘   │
│             └─────────────────────┼─────────────────────┘           │
│                                   │                                 │
│                                   ▼                                 │
│         ┌──────────────────────────────────────────────────┐       │
│         │             Server (epoll TCP)                    │       │
│         │                                                  │       │
│         │  ┌────────────────────────────────────────┐     │       │
│         │  │  Client 数组 [0..63]                    │     │       │
│         │  │  ┌──────┬──────────┬────────────────┐  │     │       │
│         │  │  │ fd   │ sub_mask │ last_heartbeat │  │     │       │
│         │  │  ├──────┼──────────┼────────────────┤  │     │       │
│         │  │  │  5   │ SUB_ALL  │  1753861200    │  │     │       │
│         │  │  │  7   │ TEMP     │  1753861215    │  │     │       │
│         │  │  │ ...  │ ...      │  ...           │  │     │       │
│         │  │  └──────┴──────────┴────────────────┘  │     │       │
│         │  └────────────────────────────────────────┘     │       │
│         │                                                  │       │
│         │  发送路径: sensor数据 → json_proto序列化        │       │
│         │           → 遍历client[] 位掩码过滤             │       │
│         │           → write(fd, json_buf, len)            │       │
│         │                                                  │       │
│         │  接收路径: read(fd) → json_proto解析            │       │
│         │           → cmd路由(SUB/PING/LIST/CLIENTS)      │       │
│         └──────────────────────┬───────────────────────────┘       │
│                                │                                     │
│         ┌──────────────────────┼──────────────────────┐            │
│         ▼                      ▼                      ▼            │
│  ┌────────────┐        ┌────────────┐        ┌────────────┐       │
│  │  Client #1  │        │  Client #2  │        │  Client #N  │       │
│  │  sub: TEMP  │        │  sub: ALL   │        │  sub: HUMI  │       │
│  │  + LIGHT    │        │             │        │             │       │
│  └────────────┘        └────────────┘        └────────────┘       │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────┐     │
│  │              SysMonitor (netlink + /proc)                   │     │
│  │  ┌─────────────────┐   ┌──────────────────┐               │     │
│  │  │  /proc reader    │   │  netlink socket   │               │     │
│  │  │  /proc/stat      │   │  NETLINK_         │               │     │
│  │  │  /proc/meminfo   │   │  KOBJECT_UEVENT   │               │     │
│  │  │  /proc/loadavg   │   │  (热插拔事件)      │               │     │
│  │  └────────┬────────┘   └────────┬─────────┘               │     │
│  │           │                     │                          │     │
│  │           └──────────┬──────────┘                          │     │
│  │                      ▼                                     │     │
│  │           ┌──────────────────┐                             │     │
│  │           │  SysInfo 结构体   │──▶ queue_push(SYSINFO)     │     │
│  │           │  cpu/mem/procs   │                             │     │
│  │           └──────────────────┘                             │     │
│  └────────────────────────────────────────────────────────────┘     │
│                                                                      │
│  ════════════════════════════════════════════════════════════════    │
│  内核态 (kernel space)                                               │
│  ════════════════════════════════════════════════════════════════    │
│                                                                      │
│  ┌─────────────────────┐   ┌──────────────────────────────┐        │
│  │  TCP/IP 协议栈       │   │  自定义内核模块 gw_info.ko    │        │
│  │  (epoll 依赖)        │   │  ┌─────────────────────────┐ │        │
│  └─────────────────────┘   │  │ /proc/gateway/info      │ │        │
│                             │  │ read → nr_processes()   │ │        │
│                             │  │        nr_free_pages()  │ │        │
│                             │  └─────────────────────────┘ │        │
│                             └──────────────────────────────┘        │
└─────────────────────────────────────────────────────────────────────┘
```

### 1.2 数据流方向

```
传感器线程 ──push──▶ EventQueue ──pop──▶ Dispatcher ──回调──▶ server_on_sensor()
                                                                     │
                          客户端A ◀── write ── 位掩码过滤 ◀── JSON序列化
                          客户端B ◀── write ── 位掩码过滤 ◀── JSON序列化
                          客户端C ◀── skip   (掩码不匹配，不发)

客户端A ──read──▶ json_proto 解析 ──▶ cmd路由 ──▶ 更新 sub_mask / 回复 PONG / 回复 LIST
```

---

## 2. 模块设计

### 2.1 模块划分

| 模块 | 源文件 | 职责 | 对外接口数量 |
|------|--------|------|-------------|
| 事件系统 | `dispatcher.c/h` | 环形队列 + 表驱动分发 | 7 个函数 |
| 传感器 | `sensor.c/h` | 模拟传感器线程 | 1 个函数 |
| 网络服务器 | `server.c/h` | epoll TCP + 客户端管理 | 3 个函数 |
| JSON 协议 | `json_proto.c/h` | 序列化/反序列化 | 4 个函数 |
| 配置管理 | `config.c/h` | 文件解析 + 默认值 | 3 个函数 |
| 系统监控 | `netlink_monitor.c/h` | /proc 读取 + 系统信息采集 | 2 个函数 |
| 日志 | `logger.c/h` | 带时间戳的日志输出 | 3 个函数 |
| 主程序 | `main.c` | 模块组装 + 信号处理 | — |

### 2.2 事件系统模块（dispatcher.c/h）

**2.2.1 事件类型枚举**

```c
typedef enum {
    TEMP_READING,         // 温度读数，value 为 ×10 整型
    HUMID_READING,        // 湿度读数，value 为百分比整型
    LIGHT_READING,        // 光照读数，value 为 lux
    PRESSURE_READING,     // 气压读数，value 为 hPa
    GAS_READING,          // 燃气读数，value 为 ppm
    SYS_INFO_READING,     // v2.0 新增：系统运行信息
    MAX_EVENT_TYPE
} EventType;

// v2.0 维护 event_type -> 位掩码位 的映射
// TEMP_READING(0) → bit 0, HUMID_READING(1) → bit 1, ..., SYS_INFO_READING(5) → bit 5
```

**2.2.2 事件结构体**

```c
typedef struct {
    EventType type;       // 事件类型（5 种传感器 + 1 种系统信息）
    int       value;      // 整型读数（温度 ×10）
    time_t    timestamp;  // 采集时间，Unix 秒
} Event;
```

**2.2.3 回调函数类型**

```c
typedef void (*event_handler)(Event *ev, void *ctx);
// ev:  事件指针（只读）
// ctx: 注册时透传的上下文（main.c 用 NULL）
```

**2.2.4 分发器表项**

```c
typedef struct {
    EventType     type;       // 感兴趣的事件类型
    event_handler handler;    // 回调函数指针
    void         *ctx;        // 回调的第二个参数
} HandlerEntry;
// 数组容量: MAX_HANDLERS = 16
// 查找方式: 线性扫描 O(n)，n ≤ 6（事件类型数量），无需优化
```

**2.2.5 事件队列**

```c
#define QUEUE_CAP 128

typedef struct {
    Event events[QUEUE_CAP];  // 环形缓冲区
    int   head;               // 出队位置
    int   tail;               // 入队位置
    int   size;               // 当前元素数
    void *lock;               // pthread_mutex_t *（void* 避免头文件依赖）
    void *cond;               // pthread_cond_t *
} EventQueue;
```

**2.2.6 API 接口**

```c
/* 分发器 */
void dispatcher_init(Dispatcher *d);
void dispatcher_register(Dispatcher *d, EventType type, event_handler h, void *ctx);
void dispatcher_dispatch(Dispatcher *d, Event *ev);

/* 队列 (生产者-消费者) */
void queue_init(EventQueue *q);
void queue_push(EventQueue *q, Event *ev);       // 生产者调用，满则丢弃
int  queue_pop(EventQueue *q, Event *ev);        // 消费者调用，空则阻塞等待
void queue_wakeup(EventQueue *q);                // 广播唤醒（退出时）
void queue_destroy(EventQueue *q);               // 释放锁和条件变量

/* 工具 */
const char* event_name(EventType t);             // 枚举 → 字符串
```

**2.2.7 queue_push 行为**

```
lock → 检查 size<CAP → 写入 events[tail] → tail=(tail+1)%CAP → size++
    → cond_signal → unlock → return
    满时: printf 丢弃警告 → unlock → return（非阻塞，不影响传感器线程）
```

**2.2.8 queue_pop 行为**

```
lock → while(size==0) cond_wait（释放锁、等信号）
    → 收到信号后检查: g_stop? → unlock return 0（退出）
    → 有数据: 拷贝 events[head] → head=(head+1)%CAP → size--
    → unlock → return 1
```

---

### 2.3 传感器模块（sensor.c/h）

**2.3.1 配置结构体**

```c
typedef struct {
    char      name[32];       // 传感器名称（中文，用于日志）
    EventType event_type;     // 产生的事件类型
    int       min_val;        // 读数范围下限
    int       max_val;        // 读数范围上限
    int       interval_ms;    // 采集间隔（毫秒）
    EventQueue *queue;        // 写入哪个队列
} SensorConfig;
```

**2.3.2 API**

```c
void* sensor_thread(void *arg);
// arg → SensorConfig*
// 循环: while(!g_stop) { usleep(interval_ms) → read_sensor() → queue_push() }
// 返回值: NULL
```

**2.3.3 读值算法**

```c
static int read_sensor(int min, int max) {
    return min + rand() % (max - min + 1);
}
// 均匀分布随机数，范围 [min, max]
```

---

### 2.4 JSON 协议模块（json_proto.c/h）— v2.0 新增

这是 v2.0 的核心新增模块，提供零依赖的 JSON 序列化与反序列化。

**2.4.1 设计约束**

```
┌──────────────────────────────────────────────────┐
│  不引入第三方库，实现约 300 行                      │
│  解析器只需要 JSON 的子集：                        │
│    ✅ 对象 {}、数组 []、字符串 "..."、数字、布尔    │
│    ❌ 嵌套对象/数组（平层级）、unicode \uXXXX       │
│  错误处理：格式错误 → 返回明确错误码，不崩溃        │
└──────────────────────────────────────────────────┘
```

**2.4.2 错误码**

```c
typedef enum {
    JSON_OK = 0,              // 成功
    JSON_ERR_FORMAT,          // 格式错误（缺少引号/花括号）
    JSON_ERR_OVERFLOW,        // 缓冲区溢出
    JSON_ERR_UNKNOWN_CMD,     // 不识别的命令
    JSON_ERR_MISSING_FIELD,   // 缺少必要字段
    JSON_ERR_BAD_VALUE,       // 字段值类型/格式不对
} JsonErr;
```

**2.4.3 序列化 API（value → JSON 字符串）**

```c
/*
 * 将传感器事件序列化为 JSON 行（以 \n 结尾）
 * 格式: {"event":"temp","value":25.5,"unit":"°C","time":1753861200,"raw":255}\n
 *
 * 参数:
 *   buf     - 输出缓冲区（调用方分配，≥ JSON_BUF_SIZE 字节）
 *   size    - 缓冲区大小
 *   ev_type - 事件类型
 *   value   - 整型读数
 *   ts      - 时间戳
 * 返回:
 *   >=0     - 写入的字节数（不含 \0）
 *   -1      - 缓冲区不够（应调大 JSON_BUF_SIZE）
 */
int json_serialize_event(char *buf, size_t size,
                         EventType ev_type, int value, time_t ts);

/*
 * 序列化系统信息
 * 格式: {"event":"sysinfo","cpu_pct":23.5,"mem_used_mb":128,...}\n
 */
int json_serialize_sysinfo(char *buf, size_t size,
                           float cpu_pct, int mem_total_kb,
                           int mem_free_kb, int procs, time_t ts);

/*
 * 序列化响应消息
 * 格式: {"result":"PONG"}\n     /   {"result":"ERR","reason":"..."}\n
 */
int json_serialize_result(char *buf, size_t size,
                          const char *result, const char *reason);

/*
 * 序列化 LIST 响应的 data 数组头部
 * 格式: {"result":"LIST","data":[
 * 调用方随后逐个拼 events，最后手动追加 "]}\n"
 * 这是为了不引入 cJSON 那种递归序列化而选择的折中
 */
int json_serialize_list_begin(char *buf, size_t size);

/*
 * 向已有缓冲区追加一个 event 对象（用于 LIST 响应）
 * 自动添加前导逗号（首次调用不添加，通过 *count 判断）
 */
int json_serialize_list_item(char *buf, size_t size, EventType t,
                             int value, time_t ts, int *count);
```

**2.4.4 反序列化 API（JSON 字符串 → C 结构体）**

```c
/*
 * 从一行 JSON 中提取命令
 * 输入: {"cmd":"SUB","type":["temp","humi"]}
 * 输出: cmd → "SUB", types → ["temp","humi",NULL], count → 2
 *
 * 参数:
 *   line  - 一行 JSON（已去掉 \n）
 *   cmd   - 输出，命令名字符串（≥16 bytes）
 *   types - 输出，类型字符串数组（每个 ≥16 bytes）
 *   count - 输出，types 数组有效元素数
 * 返回:
 *   JSON_OK(0)      - 成功
 *   JSON_ERR_*       - 解析失败
 */
JsonErr json_parse_cmd(const char *line,
                        char *cmd, size_t cmd_size,
                        char types[][16], int max_types, int *count);
// cmd 可选值: "SUB" "PING" "LIST" "CLIENTS"
// 对于 PING/LIST/CLIENTS: count=0, types 不使用
// 对于 SUB: count=订阅的传感器数量, types[0..count-1] 为传感器名
```

**2.4.5 解析器实现思路**

```
json_parse_cmd() 算法：
Step 1: 跳过空白 → 检查首字符 '{'
Step 2: 找 "cmd" 键 → 提取值字符串
Step 3: 如果是 SUB 命令 → 找 "type" 键 → 提取数组元素
Step 4: 验证闭合的 } 存在

使用简单的指针扫描，不递归：
  - 字符串提取: 找开引号 " → 拷贝到闭引号，处理 \"
  - 数组提取: 找 [ → 逐元素拷贝字符串到 types[] → 遇到 ] 结束
  - 错误恢复: 遇到出界的 } ] " → 返回对应错误码
```

---

### 2.5 配置管理模块（config.c/h）

**2.5.1 配置结构体**

```c
typedef struct {
    char name[32];
    int  min_val;
    int  max_val;
    int  interval_ms;
} SensorCfgItem;

#define MAX_SENSORS 8

typedef struct {
    /* 传感器列表 */
    SensorCfgItem sensors[MAX_SENSORS];
    int           sensor_count;

    /* v1.0 已有 */
    int           server_port;
    int           max_clients;

    /* v2.0 新增 */
    int           heartbeat_timeout;   // 心跳超时秒数，默认 30
    int           sysmon_interval_ms;  // 系统信息采集间隔，默认 5000
} AppConfig;
```

**2.5.2 API**

```c
void config_set_defaults(AppConfig *cfg);
// 先设默认值（3 个传感器、端口 8888、超时 30s、间隔 5s）
// 再调用 config_load() 用文件覆盖

int  config_load(const char *path, AppConfig *cfg);
// 逐行解析 gateway.conf，键值对格式
// 返回 0 成功，-1 失败（文件不存在也返回 -1，但不影响默认值）

void config_print(const AppConfig *cfg);
// 启动时打印全量配置
```

**2.5.3 配置文件格式**

```ini
# gateway.conf
server.port = 8888
server.max_clients = 64
server.heartbeat_timeout = 30
sysmon.interval_ms = 5000

sensor[0].name = TempSensor
sensor[0].min = 220
sensor[0].max = 380
sensor[0].interval = 2000

sensor[1].name = HumiSensor
sensor[1].min = 30
sensor[1].max = 90
sensor[1].interval = 3000

sensor[2].name = LightSensor
sensor[2].min = 50
sensor[2].max = 800
sensor[2].interval = 5000

sensor[3].name = PressureSensor
sensor[3].min = 980
sensor[3].max = 1050
sensor[3].interval = 4000

sensor[4].name = GasSensor
sensor[4].min = 0
sensor[4].max = 100
sensor[4].interval = 5000
```

---

### 2.6 网络服务器模块（server.c/h）— v2.0 重写

这是变更最大的模块。

**2.6.1 客户端结构体（v2.0 新增）**

```c
typedef uint32_t sub_mask_t;

#define SUB_TEMP      (1U << 0)
#define SUB_HUMI      (1U << 1)
#define SUB_LIGHT     (1U << 2)
#define SUB_PRESSURE  (1U << 3)
#define SUB_GAS       (1U << 4)
#define SUB_SYSINFO   (1U << 5)
#define SUB_ALL       ((1U << 6) - 1)    // 低 6 位全 1
#define SUB_NONE      0U

#define MAX_CLIENTS   64
#define HEARTBEAT_TIMEOUT 30

typedef struct {
    int         fd;              // socket 文件描述符
    sub_mask_t  sub_mask;        // 订阅位掩码（默认 SUB_ALL）
    time_t      last_heartbeat;  // 最后一次收到 PING 的时间
    bool        active;          // 此槽位是否被占用
} Client;

typedef struct {
    Client       clients[MAX_CLIENTS];
    int          count;
    pthread_mutex_t lock;        // 保护 clients[] 和 count
} ClientList;
```

**2.6.2 全局状态**

```c
// server.c 内部静态变量
static ClientList    g_clients;        // 客户端表（锁保护）
static int           g_epfd;           // epoll 实例 fd（主线程独享）
static int           g_listen_fd;      // 监听 socket
static int           g_heartbeat_to;   // 心跳超时秒数（来自配置）

// 最新传感器值（锁保护）
static int g_last_temp, g_last_humi, g_last_light;
static int g_last_pressure, g_last_gas;
static float g_last_cpu_pct;
static pthread_mutex_t g_data_lock;
```

**2.6.3 API**

```c
/*
 * 启动服务器（异步）
 * 创建监听 socket → 设非阻塞 → bind → listen → 创建 epoll
 * → 起一个 pthread 跑 server_loop()
 *
 * port: 监听端口
 * heartbeat_timeout: 心跳超时秒数
 */
void server_start(int port, int heartbeat_timeout);

/*
 * 传感器数据到达时调用（由 Dispatcher 回调触发）
 * 1. 锁 g_data_lock → 更新最新值
 * 2. 拼 JSON → 遍历 g_clients → 位掩码过滤 → write
 * 3. 解锁
 */
void server_on_sensor(EventType type, int value);

/*
 * 系统信息到达时调用
 */
void server_on_sysinfo(float cpu_pct, int mem_total_kb,
                       int mem_free_kb, int procs);
```

**2.6.4 服务器主循环结构**

```c
static void *server_loop(void *arg) {
    // 1. socket() + bind() + listen()
    // 2. epoll_create1(0)
    // 3. EPOLL_CTL_ADD listen_fd
    //
    // 4. 事件循环:
    //    while (!g_stop) {
    //        n = epoll_wait(epfd, events, MAX_EVENTS, 1000);
    //
    //        // 4a. 检查心跳超时（遍历 clients[] 检查 last_heartbeat）
    //        check_heartbeats();
    //
    //        // 4b. 处理网络事件
    //        for (i = 0; i < n; i++) {
    //            if (fd == listen_fd) → accept + add_client + EPOLL_CTL_ADD
    //            else                 → handle_client(fd)
    //        }
    //    }
    //
    // 5. close(listen_fd) + close(epfd) → return NULL
}
```

**2.6.5 命令路由表**

```c
// 命令字符串 → 函数指针 的映射
typedef JsonErr (*cmd_handler_t)(int client_fd, const char cmd_types[][16], int count);

typedef struct {
    const char    *name;
    cmd_handler_t  handler;
} CmdEntry;

static CmdEntry cmd_table[] = {
    {"SUB",     handle_sub},
    {"PING",    handle_ping},
    {"LIST",    handle_list},
    {"CLIENTS", handle_clients},
    {NULL, NULL}
};
```

**2.6.6 客户端增减操作**

```c
// 入列: lock → 找空位或末尾 → 写 Client{fd, SUB_ALL, now, true} → count++ → unlock
static void add_client(int fd);

// 出列: lock → 线性查找 fd → 最后一个元素覆盖此位 → count-- → unlock
// 调用方负责 close(fd) 和 EPOLL_CTL_DEL
static void remove_client(int fd);

// 心跳检查: lock → 遍历 clients → if (now - last > timeout) remove_client
// 在每次 epoll_wait 返回后调用
static void check_heartbeats(void);
```

---

### 2.7 系统监控模块（netlink_monitor.c/h）— v2.0 新增

**2.7.1 数据结构**

```c
typedef struct {
    float cpu_pct;          // CPU 使用百分比（0.0–100.0）
    int   mem_total_kb;     // 总内存（KB）
    int   mem_free_kb;      // 空闲内存（KB）
    int   mem_used_mb;      // 已用内存（MB，派生字段）
    int   procs;            // 进程数
} SysInfo;

typedef struct {
    /* 上一次 CPU 采样值（用于 delta 计算） */
    uint64_t prev_user;
    uint64_t prev_nice;
    uint64_t prev_system;
    uint64_t prev_idle;
    uint64_t prev_iowait;

    /* 配置 */
    int interval_ms;          // 采集间隔
    EventQueue *queue;        // 写入哪个事件队列
} SysMonConfig;
```

**2.7.2 API**

```c
/*
 * 采集一次系统信息
 * 从 /proc/stat、/proc/meminfo、/proc/loadavg 读取
 * CPU 使用率用 delta 算法
 */
SysInfo sysmon_collect(SysMonConfig *cfg);

/*
 * 系统监控线程入口
 * 循环: while(!g_stop) {
 *     info = sysmon_collect()
 *     ev.type = SYS_INFO_READING
 *     queue_push(&ev)    // info 暂存在 ev.value 中不方便 → 改用单独通道
 *     usleep(interval_ms)
 * }
 *
 * 注意: sysinfo 含多个字段，Event.value 只能存一个 int
 * 解决: 系统信息用独立推送通道 server_on_sysinfo()，不走 EventQueue
 */
void* sysmon_thread(void *arg);
```

**2.7.3 CPU Delta 算法**

```
1. 读取 /proc/stat 第一行 "cpu user nice system idle iowait ..."
2. 计算:
   total_curr = user+system+nice+idle+iowait
   total_prev = prev_user+prev_system+prev_nice+prev_idle+prev_iowait
   total_delta = total_curr - total_prev
   idle_delta  = idle - prev_idle
   cpu_pct     = (total_delta - idle_delta) * 100.0 / total_delta

3. 保存当前值到 prev_*
4. 第一次调用时 prev_* 全为 0 → 返回 0.0（无历史数据）
```

**2.7.4 /proc 文件解析函数**

```c
// 从 /proc/stat 解析 CPU 行
static int parse_proc_stat(uint64_t *user, uint64_t *system,
                           uint64_t *idle, uint64_t *iowait,
                           uint64_t *nice);

// 从 /proc/meminfo 解析 MemTotal / MemFree
static int parse_proc_meminfo(int *total_kb, int *free_kb);

// 从 /proc/loadavg 解析 1 分钟负载
static int parse_proc_loadavg(float *load1m);

// 统计 /proc 下数字子目录数 → 进程总数
static int count_processes(void);
```

---

### 2.8 主程序模块（main.c）

**2.8.1 main() 启动流程**

```
main()
  │
  ├─ signal(SIGINT, on_signal)          // Ctrl+C → g_stop=1
  ├─ srand(time(NULL))
  ├─ config_set_defaults(&cfg)          // 默认值
  ├─ config_load("gateway.conf", &cfg)  // 文件覆盖
  ├─ config_print(&cfg)
  │
  ├─ server_start(cfg.server_port, cfg.heartbeat_timeout)  // 起 TCP 线程
  │
  ├─ queue_init(&queue)                 // 事件队列
  ├─ dispatcher_init(&disp)             // 分发器
  ├─ dispatcher_register(..., on_temp, ...)   // 注册 5 个传感器回调
  ├─ dispatcher_register(..., on_humi, ...)
  ├─ ...                                // 注册 1 个系统信息回调
  │
  ├─ 创建 N 个 sensor_thread            // 配置文件驱动
  ├─ 创建 1 个 sysmon_thread            // v2.0 新增
  │
  ├─ while (!g_stop) {                  // 主事件循环
  │     queue_pop(&queue, &ev)          // 阻塞等待
  │     dispatcher_dispatch(&disp, &ev) // 回调 server_on_sensor()
  │  }
  │
  ├─ 等待所有线程 join
  ├─ queue_destroy / log_close
  └─ return 0
```

**2.8.2 回调函数表**

```c
// 每个传感器回调做的事情一样，这是函数指针的典型应用
// v2.0 不再逐个手写 on_temp / on_humi，而是用工厂函数

/*
 * 工厂函数：创建特定类型的回调
 * type: TEMP_READING / HUMID_READING / ...
 * 返回: 闭包式的回调函数（但 C 没有闭包，通过 ctx 传 type）
 */
event_handler make_sensor_callback(EventType type);

// 或者更简单地：把 5 个几乎一样的回调函数统一成一个
// on_sensor(ev, ctx) → ctx 里存 EventType → switch 派发
```

---

## 3. 线程模型

### 3.1 线程全景

```
┌──────────────────────────────────────────────────────────────┐
│  main 线程          ┌──────────────────┐                     │
│  while(!g_stop) {   │  传感器线程 ×N    │                    │
│    queue_pop()      │  while(!g_stop) {│                    │
│    dispatch()       │    usleep()      │                    │
│  }                  │    rand()        │    ← 只写队列       │
│                     │    queue_push()  │                    │
│                     │  }               │                    │
│                     └──────────────────┘                    │
│                                                              │
│  ┌─────────────────────┐   ┌──────────────────┐            │
│  │  Server 线程         │   │  Sysmon 线程     │            │
│  │  while(!g_stop) {   │   │  while(!g_stop) {│            │
│  │    epoll_wait(1s)   │   │    read /proc    │ ← 读 /proc  │
│  │    check_heartbeat  │   │    push_to_server │            │
│  │    accept + read    │   │    usleep(5s)    │            │
│  │  }                  │   │  }               │            │
│  └─────────────────────┘   └──────────────────┘            │
└──────────────────────────────────────────────────────────────┘

线程数 = 1(main) + 1(server) + 1(sysmon) + N(sensors, N=3~5)
最坏情况: 8 线程
```

### 3.2 各线程职责

| 线程 | 阻塞点 | 写入 | 读取 | 退出条件 |
|------|--------|------|------|---------|
| main | queue_pop() (条件变量) | — | EventQueue | g_stop=1 |
| sensor×N | usleep() | EventQueue | — | g_stop=1 |
| server | epoll_wait() | client fd, g_data_lock | client fd, g_clients | g_stop=1 |
| sysmon | usleep() | 直调 server_on_sysinfo() | /proc | g_stop=1 |

### 3.3 线程间通信总结

```
sensor×N  ──queue_push────→  EventQueue  ──queue_pop──→  main
                                                              main ──dispatcher_dispatch──→ 回调──→ server_on_sensor()
sysmon    ──直调──────────→  server_on_sysinfo()
client    ──write(read)────→  server──→ 解析命令→ 修改 sub_mask
```

---

## 4. 锁策略

### 4.1 锁清单

| 锁 | 保护对象 | 类型 | 持有者 |
|----|---------|------|--------|
| `q->lock` | EventQueue (events[], head, tail, size) | mutex + cond | sensor 线程(push) / main 线程(pop) |
| `g_clients.lock` | ClientList (clients[], count) | mutex | server 线程 |
| `g_data_lock` | g_last_temp/humi/light/pressure/gas | mutex | main 回调 / server 读LIST |

### 4.2 锁顺序（避免死锁）

```
g_data_lock → g_clients.lock   ← 如果两个都要拿，必须按这个顺序
q->lock                         ← 独立于上面两个，不产生嵌套
```

### 4.3 锁的持有时间

| 操作 | 锁 | 持有时间 |
|------|----|---------|
| queue_push | q->lock | 一次 memcpy + 指针更新，< 1μs |
| queue_pop | q->lock | 拿一个元素 + 指针更新，< 1μs |
| 广播到所有客户端 | g_clients.lock | for loop ×64，每个 write ~10μs，总计 < 1ms |
| 更新最新传感器值 | g_data_lock | 一次赋值，< 0.1μs |

**关键设计：广播时只持有 g_clients.lock，不持有 g_data_lock。**
每个 write 是非阻塞的（fd 设了 O_NONBLOCK），不会导致锁持有时间膨胀。

---

## 5. JSON 协议规范

（详见第 2.4 节 API 及 REQUIREMENTS.md FR-04 节）

---

## 6. 订阅机制

### 6.1 位掩码映射

```
EventType         →  位位置    →  掩码常量
──────────────────────────────────────────────
TEMP_READING(0)   →  bit 0     →  SUB_TEMP      (1)
HUMID_READING(1)  →  bit 1     →  SUB_HUMI      (2)
LIGHT_READING(2)  →  bit 2     →  SUB_LIGHT     (4)
PRESSURE_READING(3)→ bit 3     →  SUB_PRESSURE  (8)
GAS_READING(4)   →  bit 4     →  SUB_GAS       (16)
SYS_INFO_READING(5)→ bit 5     →  SUB_SYSINFO   (32)
```

### 6.2 广播过滤逻辑

```c
// server_on_sensor() 中:
int bit = (int)type;  // EventType 的枚举值 = 位位置
sub_mask_t m = 1U << bit;
pthread_mutex_lock(&g_clients.lock);
for (int i = 0; i < g_clients.count; i++) {
    if (g_clients.clients[i].sub_mask & m) {
        write(g_clients.clients[i].fd, json_buf, len);
    }
}
pthread_mutex_unlock(&g_clients.lock);
```

### 6.3 订阅命令处理

```
客户端发: {"cmd":"SUB","type":["temp","humi"]}
           ↓ json_parse_cmd()
  cmd ← "SUB", types[] ← ["temp","humi"], count ← 2
           ↓ handle_sub()
  遍历 types[]:
    "temp" → mask |= SUB_TEMP
    "humi" → mask |= SUB_HUMI
  更新 client->sub_mask = mask
```

---

## 7. 心跳检测

### 7.1 参数

| 参数 | 默认值 | 含义 |
|------|--------|------|
| HEARTBEAT_TIMEOUT | 30 秒 | 超过此时间没收到 PING 就断开 |
| epoll_wait 超时 | 1000 毫秒 | 保证每秒检查一次心跳 |
| 客户端发送间隔 | 10 秒 | 客户端自己控制（协议约定） |

### 7.2 状态机

```
        客户端连上
           │
           ▼
      last_heartbeat = time(NULL)
           │
    ┌──────┴──────────┐
    ▼                  │
 正常状态               │
    │                  │
    ├── 收到 PING ──→ last_heartbeat = time(NULL)
    │
    └── 30s 超时 ──→ 移除客户端 (EPOLL_CTL_DEL + close)
```

### 7.3 check_heartbeats() 实现

```c
static void check_heartbeats(void) {
    time_t now = time(NULL);
    pthread_mutex_lock(&g_clients.lock);
    for (int i = 0; i < g_clients.count; i++) {
        if (now - g_clients.clients[i].last_heartbeat > g_heartbeat_to) {
            int fd = g_clients.clients[i].fd;
            printf("[server] client %d heartbeat timeout, closing\n", fd);
            epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, NULL);
            close(fd);
            // 从数组移除
            g_clients.clients[i] = g_clients.clients[g_clients.count - 1];
            g_clients.clients[i].fd = g_clients.clients[g_clients.count - 1].fd;
            g_clients.count--;
            i--;
        }
    }
    pthread_mutex_unlock(&g_clients.lock);
}
```

---

## 8. 系统监控模块

（详见第 2.7 节）

---

## 9. 内核模块

### 9.1 模块结构

```
gw_info/
├── hello.c          # 内核模块源码 (~100 行)
├── Makefile         # 内核模块 Makefile
└── README.md        # 编译/测试说明
```

### 9.2 实现细节

```c
// hello.c
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/mm.h>

#define PROC_NAME "gateway/info"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cao Bin");
MODULE_DESCRIPTION("Gateway kernel info provider");

// /proc/gateway/info 的 show 回调
static int gw_show(struct seq_file *m, void *v) {
    seq_printf(m, "kernel_version: %s\n", init_utsname()->release);
    seq_printf(m, "total_processes: %d\n", nr_processes());
    seq_printf(m, "free_pages: %lu\n", nr_free_pages());
    return 0;
}

static int gw_open(struct inode *inode, struct file *file) {
    return single_open(file, gw_show, NULL);
}

static const struct proc_ops gw_fops = {
    .proc_open = gw_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

static int __init gw_init(void) {
    proc_create(PROC_NAME, 0444, NULL, &gw_fops);
    printk(KERN_INFO "[gw_info] module loaded, /proc/%s created\n", PROC_NAME);
    return 0;
}

static void __exit gw_exit(void) {
    remove_proc_entry(PROC_NAME, NULL);
    printk(KERN_INFO "[gw_info] module unloaded\n");
}

module_init(gw_init);
module_exit(gw_exit);
```

### 9.3 Makefile

```makefile
obj-m += gw_info.o
gw_info-objs := hello.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 使用示例：
#   make KDIR=/path/to/buildroot/output/build/linux-xxx
```

---

## 10. 关键时序图

### 10.1 传感器数据推送（一条数据的生命周期）

```
 SensorThread    EventQueue     MainThread    Dispatcher    ServerThread    Client
     │                │              │             │              │            │
     │ usleep(2s)     │              │             │              │            │
     │ rand(220,380)  │              │             │              │            │
     │───────────────→│              │             │              │            │
     │ queue_push()   │              │             │              │            │
     │                │─cond_signal─→│             │              │            │
     │                │              │ queue_pop() │              │            │
     │                │              │────────────→│              │            │
     │                │              │             │ dispatch()   │            │
     │                │              │             │ 找 TEMP     │            │
     │                │              │             │───回调──────→│            │
     │                │              │             │             │            │
     │                │              │             │    server_on_sensor()   │
     │                │              │             │    1. lock(g_data)      │
     │                │              │             │    2. 更新 g_last_temp  │
     │                │              │             │    3. unlock(g_data)    │
     │                │              │             │    4. json_serialize()  │
     │                │              │             │    5. broadcast()       │
     │                │              │             │────────────────────────→│
     │                │              │             │    write(json_buf)      │
     │                │              │             │                        │
     │ (下一轮循环)    │              │             │              │         │
```

### 10.2 客户端连接与命令交互

```
  新Client          Server                 已注册Client
     │                 │                       │
     │──TCP connect──→│                        │
     │                 │ accept()               │
     │                 │ set_nonblocking()      │
     │                 │ EPOLL_CTL_ADD          │
     │                 │ add_client()           │
     │                 │  sub=ALL, hb=now       │
     │                 │ push_snapshot()────────→│ (最新全量)
     │←──JSON data─────│                       │
     │                 │                       │
     │──{"cmd":"SUB",  │                       │
     │   "type":["temp"]}→                     │
     │                 │ json_parse_cmd()       │
     │                 │  → cmd="SUB"          │
     │                 │  → types=["temp"]     │
     │                 │ handle_sub()           │
     │                 │  mask = SUB_TEMP       │
     │                 │                       │
     │ (之后只收到 temp)│                       │
     │←──{"event":"temp",...}                 │
     │                 │                       │
     │──{"cmd":"PING"}→│                       │
     │                 │ handle_ping()          │
     │←──{"result":"PONG"}                    │
```

### 10.3 优雅退出时序

```
   SIGINT ──→ g_stop = 1
                │
    ┌───────────┼───────────┬──────────────┐
    ▼           ▼           ▼              ▼
 传感器线程   sysmon线程   Server线程    Main线程
 退出循环     退出循环    退出循环      退出 while
    │           │           │              │
    │           │           │     queue_wakeup() (广播)
    │           │           │              │
    │           │           │     queue_pop 收到唤醒
    │           │           │     发现 g_stop → return
    ▼           ▼           ▼              ▼
pthread_join  pthread_join  join        执行清理
                                          ├─ queue_destroy()
                                          ├─ fclose(log)
                                          └─ printf("goodbye\n")
```

---

## 11. 错误处理策略

### 11.1 策略矩阵

| 错误类型 | 处理方式 | 示例 |
|---------|---------|------|
| socket 创建失败 | perror + return（server 线程退出） | socket() 返回 -1 |
| bind 失败 | perror + close + return | 端口被占用 |
| accept 失败 | perror + continue（不影响其他客户端） | 资源耗尽 |
| write 断开 | 从列表移除 + close + continue | 客户端关闭 |
| read 断开 | return -1 → 上层移除客户端 | 客户端崩溃 |
| JSON 解析失败 | 回错误 JSON + 不崩溃 | 客户端发了乱码 |
| 队列满 | printf 警告 + 丢弃（非阻塞） | 消费太慢 |
| /proc 文件读取失败 | 返回上次值 + perror | 内核版本不兼容 |

### 11.2 原则

1. **不崩溃** — 任何单个客户端/MQTT/传感器的失败不影响整体
2. **不阻塞** — 所有 fd 非阻塞，锁持有时间 < 1ms
3. **可观测** — 所有错误都通过 printf 输出（生产环境改为 syslog）
4. **优雅退出** — SIGINT 后所有资源释放，Valgrind 零泄漏

---

## 12. 构建系统设计

### 12.1 目录结构

```
gateway/
├── main.c                # 入口
├── server.c / .h         # epoll TCP 服务器
├── sensor.c / .h         # 传感器线程
├── dispatcher.c / .h     # 事件队列 + 分发器
├── config.c / .h         # 配置文件解析
├── json_proto.c / .h     # JSON 协议（v2.0 新增）
├── netlink_monitor.c / .h # 系统监控（v2.0 新增）
├── logger.c / .h         # 日志
├── Makefile              # 主 Makefile
├── gateway.conf          # 配置文件模板
├── gw_info/              # 内核模块（独立编译）
│   ├── hello.c
│   ├── Makefile
│   └── README.md
├── DESIGN.md             # 本文件
├── REQUIREMENTS.md       # 需求文档
└── README.md             # 部署说明
```

### 12.2 Makefile 设计

```makefile
CC     ?= gcc
CFLAGS := -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -I.
LDFLAGS:= -lpthread -lm

SRCS   := main.c server.c sensor.c dispatcher.c config.c \
          json_proto.c netlink_monitor.c logger.c
OBJS   := $(SRCS:.c=.o)
TARGET := gateway

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

cross: CC = aarch64-linux-gnu-gcc
cross: all

.PHONY: all clean cross
```

---

## 13. 面试追问预设

| 问题 | 你的回答要点 |
|------|------------|
| **为什么不用 MQTT？** | 网关定位是"Linux 系统能力展示"，裸 TCP + 手写 JSON 可以展示 epoll/非阻塞 IO/协议设计。MQTT 引入 libmosquitto 反而隐藏了底层细节。实际产品会用 MQTT |
| **位掩码 vs hash 表？** | O(1) 过滤，一个 AND 指令。6 个类型用 32bit，不会溢出。如果传感器 >32 个才考虑换方案 |
| **epoll ET vs LT？** | ET 只在状态变化通知一次，配合非阻塞 fd 必须读到 EAGAIN，不能少读。LT 会反复通知，更安全但性能差一点。网关选了 ET 体现"吃得消"更高难度 |
| **心跳为什么不用 TCP keepalive？** | keepalive 是全局配置 (`tcp_keepalive_intvl`)，不能精确控制超时；而且是传输层的，无法区分"网络断了"和"程序无响应"。应用层 PING/PONG 更灵活 |
| **netlink vs /proc？** | procfs 适合读静态统计信息，netlink 适合内核推送事件。CPU/内存用的是 /proc（静态采集够用），热插拔设备用 netlink kobject_uevent |
| **为什么 /proc 而不是字符设备驱动？** | 需求是"内核传一段文本给用户态"，/proc 是最简单的方案。字符设备驱动需要注册主设备号 + mknod，代码量多 3 倍但功能一样 |
| **JSON 解析器怎么处理恶意输入？** | 缓冲区固定大小，逐字符扫描不递归，深度限制 1 层。遇到超长字符串截断返回溢出码，不崩溃 |
| **如果传感器从 5 个扩到 50 个会有什么问题？** | 1) 位掩码 32bit 不够 → 换 uint64_t 或 struct bitset；2) 每个传感器独立线程过多 → 线程池；3) 广播 O(N×M) → 按订阅分组，减少遍历 |
| **死锁遇到过吗？怎么排查？** | g_data_lock 和 g_clients.lock 的锁顺序是固定的（data→clients）。排查时如果怀疑死锁：`pstack <pid>` 看每个线程卡在哪。gdb attach → `thread apply all bt` |
| **CPU 使用率算法的精度？** | 两次 /proc/stat 取差值（delta），精度取决于采样间隔。5 秒间隔误差 < 0.5%。大于 top 命令逐秒刷新的精度，但胜在"无中间商"——自己采集自己算，代码透明 |

---

*设计定稿：2026-07-30 | 版本：v2.0*
