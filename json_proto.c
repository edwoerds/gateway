#include "json_proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ==================================================================
 * 事件类型 ↔ 字符串映射
 * ================================================================ */

static const char *event_type_str(EventType t)
{
    switch (t) {
        case TEMP_READING:     return "temp";
        case HUMID_READING:    return "humi";
        case LIGHT_READING:    return "light";
        case PRESSURE_READING: return "pressure";
        case GAS_READING:      return "gas";
        case SYS_INFO_READING: return "sysinfo";
        default:               return "unknown";
    }
}

/* 温度显示值：×10 整数 → 浮点字符串 */
static void format_temp(char *buf, size_t size, int raw)
{
    int integral = raw / 10;
    int frac     = raw % 10;
    if (frac < 0) { integral--; frac += 10; }
    snprintf(buf, size, "%d.%d", integral, frac);
}

/* ==================================================================
 * 序列化
 * ================================================================ */

int json_serialize_event(char *buf, size_t size,
                         EventType type, int value, time_t ts)
{
    const char *name = event_type_str(type);
    const char *unit = "";
    int divisor = 1;

    switch (type) {
        case TEMP_READING:  unit = "°C";  divisor = 10; break;
        case HUMID_READING: unit = "%";   break;
        case LIGHT_READING: unit = "lux"; break;
        case PRESSURE_READING: unit = "hPa"; break;
        case GAS_READING:   unit = "ppm"; break;
        default: break;
    }

    char val_str[32];
    if (type == SYS_INFO_READING) {
        /* SYS_INFO 有专用序列化函数，这里仅用于 LIST 快照 */
        snprintf(val_str, sizeof(val_str), "%d", value);
    } else if (divisor > 1) {
        format_temp(val_str, sizeof(val_str), value);
    } else {
        snprintf(val_str, sizeof(val_str), "%d", value);
    }

    return snprintf(buf, size,
        "{\"event\":\"%s\",\"value\":%s,\"unit\":\"%s\",\"time\":%ld}\n",
        name, val_str, unit, (long)ts);
}

int json_serialize_sysinfo(char *buf, size_t size,
                           float cpu_pct, int mem_total_kb,
                           int mem_free_kb, int procs, time_t ts)
{
    int mem_used_mb = (mem_total_kb - mem_free_kb) / 1024;
    int mem_total_mb = mem_total_kb / 1024;

    return snprintf(buf, size,
        "{\"event\":\"sysinfo\",\"cpu_pct\":%.1f,"
        "\"mem_used_mb\":%d,\"mem_total_mb\":%d,"
        "\"procs\":%d,\"time\":%ld}\n",
        cpu_pct, mem_used_mb, mem_total_mb, procs, (long)ts);
}

int json_serialize_result(char *buf, size_t size,
                          const char *result, const char *reason)
{
    if (reason && strlen(reason) > 0) {
        return snprintf(buf, size,
            "{\"result\":\"%s\",\"reason\":\"%s\"}\n",
            result, reason);
    }
    return snprintf(buf, size, "{\"result\":\"%s\"}\n", result);
}

int json_serialize_list_begin(char *buf, size_t size)
{
    return snprintf(buf, size, "{\"result\":\"LIST\",\"data\":[");
}

int json_serialize_list_item(char *buf, size_t size,
                             EventType t, int value, time_t ts, int *count)
{
    char tmp[256];
    int n = json_serialize_event(tmp, sizeof(tmp), t, value, ts);
    if (n < 0) return -1;

    int written = 0;
    if (*count > 0) {
        written = snprintf(buf, size, ",%s", tmp);
    } else {
        written = snprintf(buf, size, "%s", tmp);
    }
    (*count)++;
    return written;
}

int json_serialize_list_end(char *buf, size_t size)
{
    return snprintf(buf, size, "]}\n");
}

/* ==================================================================
 * 反序列化：解析 JSON 命令
 *
 * 支持的格式：
 *   {"cmd":"SUB","type":["temp","humi"]}
 *   {"cmd":"PING"}
 *   {"cmd":"LIST"}
 *   {"cmd":"CLIENTS"}
 *
 * 采用指针扫描，不递归，无反斜杠转义处理。
 * ================================================================ */

/*
 * 跳过空白字符
 */
static const char *skip_space(const char *p)
{
    while (p && *p && (unsigned char)*p <= ' ') p++;
    return p;
}

/*
 * 查找字符串值：在 JSON 中找到 "key":" 后提取直到闭引号
 * json:  当前扫描位置
 * key:   要查找的 key 名字（不带引号）
 * out:   输出缓冲区
 * out_size: 缓冲区大小
 * 返回: 找到值后的下一个扫描位置，NULL 失败
 */
static const char *extract_string_value(const char *json, const char *key,
                                        char *out, size_t out_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return NULL;

    p += strlen(pattern);          /* 跳过 "key":" */
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return p;                      /* 返回闭引号后的位置 */
}

/*
 * 提取字符串数组：在 JSON 中找到 "type":["a","b"] 后提取元素
 * json:  当前扫描位置
 * out:   二维字符数组
 * max:   最大提取数
 * count: 输出，实际提取数
 * 返回: 数组后的位置，NULL 失败
 */
static const char *extract_string_array(const char *json, const char *key,
                                        char out[][16], int max, int *count)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);

    const char *p = strstr(json, pattern);
    if (!p) return NULL;

    p += strlen(pattern);          /* 跳过 "type":[ */
    *count = 0;

    while (*p && *p != ']' && *count < max) {
        p = skip_space(p);
        if (*p != '"') break;
        p++;                       /* 跳过开引号 */
        int i = 0;
        while (*p && *p != '"' && i < 15) {
            out[*count][i++] = *p++;
        }
        out[*count][i] = '\0';
        (*count)++;
        if (*p == '"') p++;        /* 跳过闭引号 */
        p = skip_space(p);
        if (*p == ',') {
            p++;
            p = skip_space(p);
        }
    }
    return p;
}

int json_parse_cmd(const char *line,
                   CmdType *cmd,
                   char types[][16], int max_types, int *count)
{
    if (!line || !cmd) return -1;

    *cmd = CMD_UNKNOWN;
    if (count) *count = 0;

    /* 跳过空白，检查第一个 { */
    const char *p = skip_space(line);
    if (*p != '{') return -1;

    /* 提取 "cmd": 值 */
    char cmd_str[16] = "";
    p = extract_string_value(p, "cmd", cmd_str, sizeof(cmd_str));
    if (!p) return -1;

    /* 匹配命令 */
    if      (strcmp(cmd_str, "SUB")    == 0) *cmd = CMD_SUB;
    else if (strcmp(cmd_str, "PING")   == 0) *cmd = CMD_PING;
    else if (strcmp(cmd_str, "LIST")   == 0) *cmd = CMD_LIST;
    else if (strcmp(cmd_str, "CLIENTS")== 0) *cmd = CMD_CLIENTS;
    else return -1;

    /* SUB 命令还要提取 type 数组 */
    if (*cmd == CMD_SUB && types && count) {
        extract_string_array(p, "type", types, max_types, count);
    }

    return 0;
}
