#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* 去掉字符串首尾空白（C 标准库没有 trim，嵌入式经常要自己写） */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (*s == '\0')
        return s;

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        end--;
    *(end + 1) = '\0';
    return s;
}

/* 空行或注释行（# 或 // 开头）→ 返回 1 */
static int is_blank_or_comment(const char *line)
{
    while (*line && isspace((unsigned char)*line))
        line++;
    return (*line == '\0' || *line == '#' ||
            (*line == '/' && *(line + 1) == '/'));
}

/* 设置默认值 */
void config_set_defaults(AppConfig *cfg)
{
    cfg->sensor_count       = 3;
    cfg->server_port        = 8888;
    cfg->max_clients        = 64;
    cfg->heartbeat_timeout  = 30;     /* v2.0 */
    cfg->sysmon_interval_ms = 5000;   /* v2.0 */

    strcpy(cfg->sensors[0].name, "TempSensor");
    cfg->sensors[0].min_val     = 220;
    cfg->sensors[0].max_val     = 380;
    cfg->sensors[0].interval_ms = 2000;

    strcpy(cfg->sensors[1].name, "HumiSensor");
    cfg->sensors[1].min_val     = 30;
    cfg->sensors[1].max_val     = 90;
    cfg->sensors[1].interval_ms = 3000;

    strcpy(cfg->sensors[2].name, "LightSensor");
    cfg->sensors[2].min_val     = 50;
    cfg->sensors[2].max_val     = 800;
    cfg->sensors[2].interval_ms = 5000;
}

/* 解析一行 "key = value"。返回 0=成功，-1=跳过 */
static int parse_line(const char *line, char *key, int key_size,
                      char *val, int val_size)
{
    const char *eq = strchr(line, '=');
    if (!eq)
        return -1;

    /* 提取 key（= 之前）并 trim */
    size_t klen = (size_t)(eq - line);
    if (klen >= (size_t)key_size)
        klen = key_size - 1;
    memcpy(key, line, klen);
    key[klen] = '\0';
    memmove(key, trim(key), strlen(key) + 1);

    /* 提取 value（= 之后）并 trim */
    strncpy(val, eq + 1, val_size - 1);
    val[val_size - 1] = '\0';
    memmove(val, trim(val), strlen(val) + 1);
    return 0;
}

/* 从文件加载配置（增量覆盖：只修改文件里出现的项） */
int config_load(const char *path, AppConfig *cfg)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[config] %s not found, using defaults\n", path);
        return -1;
    }

    printf("[config] loading %s ...\n", path);

    char line[256], key[64], val[64];
    int lineno = 0;

    while (fgets(line, sizeof(line), fp)) {
        lineno++;

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';

        if (is_blank_or_comment(line))
            continue;

        if (parse_line(line, key, sizeof(key), val, sizeof(val)) < 0) {
            printf("[config] line %d: cannot parse, skipped\n", lineno);
            continue;
        }

        if (strcmp(key, "server.port") == 0) {
            cfg->server_port = atoi(val);
        } else if (strcmp(key, "server.max_clients") == 0) {
            cfg->max_clients = atoi(val);
        } else if (strcmp(key, "server.heartbeat_timeout") == 0) {
            cfg->heartbeat_timeout = atoi(val);
        } else if (strcmp(key, "sysmon.interval_ms") == 0) {
            cfg->sysmon_interval_ms = atoi(val);
        } else if (strncmp(key, "sensor", 6) == 0) {
            /* 解析 sensor[N].xxx */
            const char *field = key + 6;
            int idx, pos = 0;
            char rest[32];
            if (sscanf(field, "[%d].%31s%n", &idx, rest, &pos) >= 2 && pos > 0) {
                if (idx < 0 || idx >= MAX_SENSORS) {
                    printf("[config] line %d: sensor[%d] out of range\n",
                           lineno, idx);
                    continue;
                }
                if (strcmp(rest, "name") == 0)
                    snprintf(cfg->sensors[idx].name,
                             sizeof(cfg->sensors[idx].name),
                             "%.31s", val);
                else if (strcmp(rest, "min") == 0)
                    cfg->sensors[idx].min_val = atoi(val);
                else if (strcmp(rest, "max") == 0)
                    cfg->sensors[idx].max_val = atoi(val);
                else if (strcmp(rest, "interval") == 0)
                    cfg->sensors[idx].interval_ms = atoi(val);
                else
                    printf("[config] line %d: unknown field '%s'\n",
                           lineno, rest);

                if (idx + 1 > cfg->sensor_count)
                    cfg->sensor_count = idx + 1;
            } else {
                printf("[config] line %d: bad key '%s'\n", lineno, key);
            }
        } else {
            printf("[config] line %d: unknown key '%s'\n", lineno, key);
        }
    }

    fclose(fp);
    printf("[config] loaded %d sensors, port=%d\n",
           cfg->sensor_count, cfg->server_port);
    return 0;
}

/* 打印配置 */
void config_print(const AppConfig *cfg)
{
    printf("--- Config ---\n");
    printf("  Server port        : %d\n", cfg->server_port);
    printf("  Max clients        : %d\n", cfg->max_clients);
    printf("  Heartbeat timeout  : %ds\n", cfg->heartbeat_timeout);
    printf("  Sysmon interval    : %dms\n", cfg->sysmon_interval_ms);
    printf("  Sensors (%d):\n", cfg->sensor_count);
    for (int i = 0; i < cfg->sensor_count; i++) {
        printf("    [%d] %-12s  min=%4d  max=%4d  interval=%4dms\n",
               i, cfg->sensors[i].name,
               cfg->sensors[i].min_val,
               cfg->sensors[i].max_val,
               cfg->sensors[i].interval_ms);
    }
    printf("----------------\n");
}
