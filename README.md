# 智能家居网关模拟器

> 嵌入式 Linux 项目 | 纯 C | epoll TCP 服务器 | 多线程传感器模拟

## 概述

多传感器数据采集网关，模拟 5 种环境传感器（温度、湿度、光照、气压、燃气），通过 epoll TCP 服务器实时推送给多客户端。

**v2.0 新增能力：**
- JSON 协议通信（取代 v1.0 纯文本）
- 订阅过滤：客户端按需订阅传感器类型
- 心跳检测：PING/PONG 机制，30 秒超时断开
- 系统监控：通过 /proc 采集 CPU/内存/进程数，作为第 6 种"传感器"推送
- 内核模块：/proc/gateway/info 暴露内核态信息

**代码量：** ~2000 行 C（含内核模块 ~2100 行）

---

## 构建

```bash
# 用户态程序
make clean && make
./gateway
```

```bash
# 内核模块（需 Buildroot / 目标板 kernel headers）
cd gw_info && make
insmod gw_info.ko
```

---

## JSON 协议

### 服务器 → 客户端（推送）

```json
{"event":"temp","value":26.5,"unit":"°C","time":1690790400}
{"event":"humi","value":60,"unit":"%","time":1690790400}
{"event":"light","value":320,"unit":"lux","time":1690790400}
{"event":"pressure","value":1013,"unit":"hPa","time":1690790400}
{"event":"gas","value":42,"unit":"ppm","time":1690790400}
{"event":"sysinfo","cpu_pct":23.5,"mem_used_mb":128,"mem_total_mb":512,"procs":42,"time":1690790400}
```

### 客户端 → 服务器（命令）

```json
{"cmd":"SUB","type":["temp","humi"]}     // 订阅指定传感器
{"cmd":"SUB","type":[]}                  // 取消所有订阅
{"cmd":"PING"}                            // 心跳
{"cmd":"LIST"}                            // 当前全量快照
{"cmd":"CLIENTS"}                         // 在线客户端数
```

### 服务器 → 客户端（响应）

```json
{"result":"PONG"}
{"result":"ERR","reason":"unknown command"}
{"result":"LIST","data":[
  {"event":"temp","value":26.5,"unit":"°C","time":1690790400},
  {"event":"humi","value":60,"unit":"%","time":1690790400}
]}
{"result":"CLIENTS","count":3}
```

---

## 测试

```bash
# 终端 1：启动网关
./gateway

# 终端 2：nc 连接
nc 127.0.0.1 8888

# 然后输入：
{"cmd":"SUB","type":["temp","humi"]}
{"cmd":"PING"}
{"cmd":"LIST"}

# 你会看到实时 JSON 数据推送
```

---

## 配置

编辑 `gateway.conf`：

```ini
server.port = 8888
server.max_clients = 64
server.heartbeat_timeout = 30    # 心跳超时秒数
sysmon.interval_ms = 5000        # 系统监控采集间隔
sensor[N].name = TempSensor
sensor[N].min = 220
sensor[N].max = 380
sensor[N].interval = 2000
```

---

## 架构

```
传感器线程 ×N ──push──▶ EventQueue ──pop──▶ Dispatcher ──回调──▶ server_on_sensor()
                                                                    │
                                 客户端A ◀── write ── sub_mask 过滤 ◀── JSON 序列化
                                 客户端B ◀── write ── sub_mask 过滤
                                 客户端C ── skip（掩码不匹配）
```

| 模块 | 职责 |
|------|------|
| `sensor.c` | 传感器模拟线程（温度/湿度/光照/气压/燃气） |
| `dispatcher.c` | 事件队列 + 表驱动分发 |
| `server.c` | epoll TCP 服务器、订阅过滤、心跳检测 |
| `config.c` | 配置文件解析 |
| `json_proto.c` | JSON 序列化/反序列化（零依赖） |
| `netlink_monitor.c` | /proc 系统监控采集 |
| `gw_info/hello.c` | 内核模块（/proc/gateway/info） |

---

## 内核模块

```bash
cd gw_info
# 在 Buildroot 环境指定内核目录
make KDIR=/path/to/buildroot/output/build/linux-custom
insmod gw_info.ko
cat /proc/gateway/info
# 输出示例：
#   kernel: 5.10.0
#   processes: 42
#   free_pages: 123456
#   uptime_secs: 3600
rmmod gw_info
```

---

## GitHub

- 仓库：https://github.com/edwoerds/gateway
- 蓝牙 AI 音箱（配套项目）：https://github.com/edwoerds/ai-speaker
