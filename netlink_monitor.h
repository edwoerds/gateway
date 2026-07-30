#ifndef NETLINK_MONITOR_H
#define NETLINK_MONITOR_H

/*
 * 系统监控模块
 *
 * 通过读取 /proc 文件系统采集系统运行状态：
 *   - /proc/stat   → CPU 使用率（delta 算法）
 *   - /proc/meminfo → 内存总量/空闲
 *   - /proc/loadavg → 进程数
 *
 * 采集结果通过事件队列以 SYS_INFO_READING 事件类型发送。
 */

#include "dispatcher.h"
#include <stdint.h>

typedef struct {
    float cpu_pct;          /* CPU 使用率 0.0~100.0 */
    int   mem_total_kb;     /* 总内存 KB */
    int   mem_free_kb;      /* 空闲内存 KB */
    int   procs;            /* 进程总数 */
} SysInfo;

/* 系统监控配置 */
typedef struct {
    int         interval_ms;     /* 采集间隔（毫秒） */
    EventQueue *queue;           /* 写入哪个事件队列 */
} SysMonConfig;

/*
 * 采集一次系统信息
 * 返回当前系统快照（CPU、内存、进程数）
 */
SysInfo sysmon_collect(void);

/*
 * 系统监控线程入口
 * 循环采集 → 写入事件队列
 */
void* sysmon_thread(void *arg);

#endif /* NETLINK_MONITOR_H */
