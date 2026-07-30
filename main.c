#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
#include "dispatcher.h"
#include "sensor.h"
#include "server.h"
#include "config.h"
#include "netlink_monitor.h"

volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
    printf("\n[gateway] shutting down...\n");
}

/* ==================================================================
 * 传感器事件回调
 * ================================================================ */

static void on_temperature(Event *ev, void *ctx)
{
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 🌡️  Temperature: %d.%d°C\n",
           t->tm_hour, t->tm_min, t->tm_sec,
           ev->value / 10, ev->value % 10);
    server_on_sensor(TEMP_READING, ev->value);
}

static void on_humidity(Event *ev, void *ctx)
{
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 💧  Humidity: %d%%\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    server_on_sensor(HUMID_READING, ev->value);
}

static void on_light(Event *ev, void *ctx)
{
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] ☀️   Light: %d lux\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    server_on_sensor(LIGHT_READING, ev->value);
}

static void on_pressure(Event *ev, void *ctx)
{
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 🌀 Pressure: %d hPa\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    server_on_sensor(PRESSURE_READING, ev->value);
}

static void on_gas(Event *ev, void *ctx)
{
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 🔥 Gas: %d ppm\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    server_on_sensor(GAS_READING, ev->value);
}

/* ==================================================================
 * 系统信息回调（v2.0 新增）
 * ================================================================ */

static void on_sysinfo(Event *ev, void *ctx)
{
    (void)ctx;
    /* ev->value 存的是 cpu_pct × 100 */
    float cpu = ev->value / 100.0f;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] ⚙️  CPU:%.1f%%\n",
           t->tm_hour, t->tm_min, t->tm_sec, cpu);
    server_on_sensor(SYS_INFO_READING, ev->value);
}

/* ==================================================================
 * 主函数
 * ================================================================ */

int main()
{
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)time(NULL));

    AppConfig cfg;
    config_set_defaults(&cfg);
    config_load("gateway.conf", &cfg);
    config_print(&cfg);

    /* 启动 TCP 服务器（v2.0：增加心跳超时参数） */
    server_start(cfg.server_port, cfg.heartbeat_timeout);

    printf("\n🏠 Smart Home Gateway v2.0 Starting...\n\n");

    /* 事件队列 + 分发器 */
    EventQueue queue;
    queue_init(&queue);

    Dispatcher disp;
    dispatcher_init(&disp);
    dispatcher_register(&disp, TEMP_READING,     on_temperature, NULL);
    dispatcher_register(&disp, HUMID_READING,    on_humidity,    NULL);
    dispatcher_register(&disp, LIGHT_READING,    on_light,       NULL);
    dispatcher_register(&disp, PRESSURE_READING, on_pressure,    NULL);
    dispatcher_register(&disp, GAS_READING,      on_gas,         NULL);
    dispatcher_register(&disp, SYS_INFO_READING, on_sysinfo,     NULL);

    /* 启动传感器线程 */
    #define MAX_SENSOR_THREADS 8
    pthread_t sensor_tids[MAX_SENSOR_THREADS];

    for (int i = 0; i < cfg.sensor_count && i < MAX_SENSOR_THREADS; i++) {
        EventType et;
        if      (strcmp(cfg.sensors[i].name, "TempSensor") == 0)
            et = TEMP_READING;
        else if (strcmp(cfg.sensors[i].name, "HumiSensor") == 0)
            et = HUMID_READING;
        else if (strcmp(cfg.sensors[i].name, "LightSensor") == 0)
            et = LIGHT_READING;
        else if (strcmp(cfg.sensors[i].name, "PressureSensor") == 0)
            et = PRESSURE_READING;
        else
            et = GAS_READING;

        SensorConfig sc = {
            .name        = "",
            .event_type  = et,
            .min_val     = cfg.sensors[i].min_val,
            .max_val     = cfg.sensors[i].max_val,
            .interval_ms = cfg.sensors[i].interval_ms,
            .queue       = &queue
        };
        strcpy(sc.name, cfg.sensors[i].name);
        pthread_create(&sensor_tids[i], NULL, sensor_thread, &sc);
    }

    /* 启动系统监控线程（v2.0 新增） */
    SysMonConfig sm_cfg = {
        .interval_ms = cfg.sysmon_interval_ms,
        .queue       = &queue,
    };
    pthread_t sysmon_tid;
    pthread_create(&sysmon_tid, NULL, sysmon_thread, &sm_cfg);

    int sensor_count = cfg.sensor_count;
    printf("=== Sensors started, waiting for data... ===\n");
    printf("   Press Ctrl+C to stop\n\n");

    /* 主事件循环 */
    while (!g_stop) {
        Event ev;
        if (queue_pop(&queue, &ev) == 0) break;
        dispatcher_dispatch(&disp, &ev);
    }

    printf("[gateway] cleaning up...\n");

    /* 等待线程退出 */
    for (int i = 0; i < sensor_count && i < MAX_SENSOR_THREADS; i++)
        pthread_join(sensor_tids[i], NULL);

    g_stop = 1;
    queue_wakeup(&queue);

    queue_destroy(&queue);
    printf("[gateway] goodbye!\n");
    return 0;
}
