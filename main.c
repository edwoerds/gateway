#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "dispatcher.h"
#include "sensor.h"
#include "server.h"
#include <signal.h>
#include "config.h"
#include "logger.h"

volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
    printf("\n[gateway] shutting down...\n");
}

static void on_temperature(Event *ev, void *ctx) {
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 🌡️  Temperature: %d.%d°C\n",
           t->tm_hour, t->tm_min, t->tm_sec,
           ev->value / 10, ev->value % 10);
    log_write(LOG_INFO, "🌡️  Temperature: %d.%d°C",
              ev->value / 10, ev->value % 10);
    server_on_sensor(TEMP_READING, ev->value);
}

static void on_humidity(Event *ev, void *ctx) {
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 💧  Humidity: %d%%\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    log_write(LOG_INFO, "💧  Humidity: %d%%", ev->value);
    server_on_sensor(HUMID_READING, ev->value);
}

static void on_light(Event *ev, void *ctx) {
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] ☀️   Light: %d lux\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    log_write(LOG_INFO, "☀️   Light: %d lux", ev->value);
    server_on_sensor(LIGHT_READING, ev->value);
}

static void on_pressure(Event *ev, void *ctx) {
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 🌀 Pressure: %d hPa\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    log_write(LOG_INFO, "🌀 Pressure: %d hPa", ev->value);
    server_on_sensor(PRESSURE_READING, ev->value);
}

static void on_gas(Event *ev, void *ctx) {
    (void)ctx;
    struct tm *t = localtime(&ev->timestamp);
    printf("[%02d:%02d:%02d] 🔥 Gas: %d ppm\n",
           t->tm_hour, t->tm_min, t->tm_sec, ev->value);
    log_write(LOG_INFO, "🔥 Gas: %d ppm", ev->value);
    server_on_sensor(GAS_READING, ev->value);
}

int main() {
    signal(SIGINT, on_signal);
    srand((unsigned)time(NULL));

    AppConfig cfg;
    config_set_defaults(&cfg);
    config_load("gateway.conf", &cfg);
    config_print(&cfg);
    log_init(cfg.log_file);
    server_start(cfg.server_port);

    printf("🏠 Smart Home Gateway Starting...\n\n");

    EventQueue queue;
    queue_init(&queue);

    Dispatcher disp;
    dispatcher_init(&disp);
    dispatcher_register(&disp, TEMP_READING,     on_temperature, NULL);
    dispatcher_register(&disp, HUMID_READING,    on_humidity,    NULL);
    dispatcher_register(&disp, LIGHT_READING,    on_light,       NULL);
    dispatcher_register(&disp, PRESSURE_READING, on_pressure,    NULL);
    dispatcher_register(&disp, GAS_READING,      on_gas,         NULL);

#define MAX_SENSOR_THREADS 8
    pthread_t sensor_tids[MAX_SENSOR_THREADS];
    SensorConfig sensor_cfgs[MAX_SENSOR_THREADS];

    for (int i = 0; i < cfg.sensor_count && i < MAX_SENSOR_THREADS; i++) {
        EventType et;
        if (strcmp(cfg.sensors[i].name, "TempSensor") == 0)
            et = TEMP_READING;
        else if (strcmp(cfg.sensors[i].name, "HumiSensor") == 0)
            et = HUMID_READING;
        else if (strcmp(cfg.sensors[i].name, "LightSensor") == 0)
            et = LIGHT_READING;
        else if (strcmp(cfg.sensors[i].name, "PressureSensor") == 0)
            et = PRESSURE_READING;
        else
            et = GAS_READING;

        sensor_cfgs[i] = (SensorConfig){
            .name        = "",
            .event_type  = et,
            .min_val     = cfg.sensors[i].min_val,
            .max_val     = cfg.sensors[i].max_val,
            .interval_ms = cfg.sensors[i].interval_ms,
            .queue       = &queue
        };
        strcpy(sensor_cfgs[i].name, cfg.sensors[i].name);

        pthread_create(&sensor_tids[i], NULL, sensor_thread, &sensor_cfgs[i]);
        log_write(LOG_INFO, "[%d] %s started (range=[%d,%d], interval=%dms)",
                  i, cfg.sensors[i].name,
                  cfg.sensors[i].min_val, cfg.sensors[i].max_val,
                  cfg.sensors[i].interval_ms);
    }

    printf("=== Sensors started, waiting for data... ===\n");
    printf("   Press Ctrl+C to stop\n\n");

    while (!g_stop) {
        Event ev;
        if (queue_pop(&queue, &ev) == 0) break;
        dispatcher_dispatch(&disp, &ev);
    }

    printf("[gateway] cleaning up...\n");

    for (int i = 0; i < cfg.sensor_count && i < MAX_SENSOR_THREADS; i++) {
        pthread_join(sensor_tids[i], NULL);
    }

    log_write(LOG_INFO, "gateway stopped");
    log_close();
    queue_destroy(&queue);
    printf("[gateway] goodbye!\n");
    return 0;
}
