#ifndef JSON_PROTO_H
#define JSON_PROTO_H

#include "dispatcher.h"
#include <stddef.h>

/*
 * JSON 协议模块
 *
 * 服务器↔客户端通信使用 JSON 格式，零依赖手写实现。
 *
 * 服务器→客户端（推送）：
 *   {"event":"temp","value":25.5,"unit":"°C","time":1234567890}
 *   {"event":"sysinfo","cpu_pct":23.5,"mem_used_mb":128,"procs":42}
 *
 * 客户端→服务器（命令）：
 *   {"cmd":"SUB","type":["temp","humi"]}
 *   {"cmd":"PING"}
 *   {"cmd":"LIST"}
 *
 * 服务器→客户端（响应）：
 *   {"result":"PONG"}
 *   {"result":"ERR","reason":"..."}
 */

/* ========== 序列化（value → JSON 字符串） ========== */

#define JSON_BUF_SIZE 512

/*
 * 序列化传感器事件
 * buf: 输出缓冲区（≥ JSON_BUF_SIZE）
 * 返回: 写入字节数，-1 失败
 */
int json_serialize_event(char *buf, size_t size,
                         EventType type, int value, time_t ts);

/*
 * 序列化系统信息
 */
int json_serialize_sysinfo(char *buf, size_t size,
                           float cpu_pct, int mem_total_kb,
                           int mem_free_kb, int procs, time_t ts);

/*
 * 序列化响应消息
 * result: "PONG" / "ERR"
 * reason: result 为 "ERR" 时的原因（可为 NULL）
 */
int json_serialize_result(char *buf, size_t size,
                          const char *result, const char *reason);

/*
 * 序列化 LIST 快照的起始部分
 * 返回后调用方手动拼 ",...]}" 结尾
 */
int json_serialize_list_begin(char *buf, size_t size);

/*
 * 向 LIST 响应追加一个事件项
 * count: 当前已追加的项数（首次传 0）
 */
int json_serialize_list_item(char *buf, size_t size,
                             EventType t, int value, time_t ts, int *count);

/*
 * LIST 响应收尾："]}" 并保证字符串完整
 */
int json_serialize_list_end(char *buf, size_t size);

/* ========== 反序列化（JSON 字符串 → 命令） ========== */

typedef enum {
    CMD_UNKNOWN,
    CMD_SUB,        /* 订阅 */
    CMD_PING,       /* 心跳 */
    CMD_LIST,       /* 查看快照 */
    CMD_CLIENTS,    /* 查看在线客户端数 */
} CmdType;

/* 解析客户端的 JSON 命令行
 * line: 输入 JSON 字符串（以 \0 结尾）
 * cmd:  输出，命令类型
 * types:输出，订阅的传感器名数组（CMD_SUB 时有效）
 * count:输出，types 数组长度
 *
 * 返回: 0 成功，-1 解析失败
 */
int json_parse_cmd(const char *line,
                   CmdType *cmd,
                   char types[][16], int max_types, int *count);

#endif /* JSON_PROTO_H */
