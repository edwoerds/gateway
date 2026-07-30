#ifndef SERVER_H
#define SERVER_H

#include "dispatcher.h"

/*
 * epoll TCP 服务器（v2.0）
 *
 * 功能：
 *   - JSON 协议通信（取代 v1.0 的纯文本 + emoji）
 *   - 订阅过滤：每个客户端维护 sub_mask，只推送订阅的传感器类型
 *   - 心跳检测：PING/PONG 机制，超时自动断开
 *   - 支持命令：SUB / PING / LIST / CLIENTS
 */

#define MAX_CLIENTS  64
#define HEARTBEAT_TO 30   /* 心跳超时秒数（默认） */

/* 订阅位掩码 */
typedef unsigned int sub_mask_t;
#define SUB_TEMP      (1u << 0)
#define SUB_HUMI      (1u << 1)
#define SUB_LIGHT     (1u << 2)
#define SUB_PRESSURE  (1u << 3)
#define SUB_GAS       (1u << 4)
#define SUB_SYSINFO   (1u << 5)
#define SUB_ALL       (SUB_TEMP | SUB_HUMI | SUB_LIGHT | \
                       SUB_PRESSURE | SUB_GAS | SUB_SYSINFO)
#define SUB_NONE      0u

/* 事件类型 → 掩码位 的映射 */
static inline sub_mask_t event_to_sub(EventType t)
{
    switch (t) {
        case TEMP_READING:     return SUB_TEMP;
        case HUMID_READING:    return SUB_HUMI;
        case LIGHT_READING:    return SUB_LIGHT;
        case PRESSURE_READING: return SUB_PRESSURE;
        case GAS_READING:      return SUB_GAS;
        case SYS_INFO_READING: return SUB_SYSINFO;
        default:               return SUB_NONE;
    }
}

void server_start(int port, int heartbeat_timeout);
void server_on_sensor(EventType type, int value);

#endif /* SERVER_H */
