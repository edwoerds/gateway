#include "netlink_monitor.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

extern volatile sig_atomic_t g_stop;

/* ==================================================================
 * CPU delta 状态（static，线程独占）
 * ================================================================ */

static uint64_t s_prev_total = 0;
static uint64_t s_prev_idle  = 0;
static bool     s_cpu_first  = true;

/* ==================================================================
 * 工具：从 /proc 文件中按 key 匹配行，提取数值
 * ================================================================ */

static int read_proc_int(const char *path, const char *key)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[256];
    int val = -1;
    size_t klen = strlen(key);

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, klen) == 0) {
            const char *p = line + klen;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            val = atoi(p);
            break;
        }
    }
    fclose(fp);
    return val;
}

/* ==================================================================
 * CPU 使用率 — delta 算法
 *
 * /proc/stat 第一行格式：
 *   cpu  user  nice  system  idle  iowait  irq  softirq  steal
 * cpu_pct = (total_delta - idle_delta) / total_delta × 100
 * 首次采集返回 0.0（无历史数据）
 * ================================================================ */

static float collect_cpu_pct(void)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0.0f;

    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    char line[256];
    float pct = 0.0f;

    if (fgets(line, sizeof(line), fp)) {
        sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               (unsigned long long*)&user,
               (unsigned long long*)&nice,
               (unsigned long long*)&system,
               (unsigned long long*)&idle,
               (unsigned long long*)&iowait,
               (unsigned long long*)&irq,
               (unsigned long long*)&softirq,
               (unsigned long long*)&steal);

        uint64_t total = user + nice + system + idle + iowait
                       + irq + softirq + steal;

        if (s_cpu_first) {
            s_cpu_first = false;
        } else {
            uint64_t dtotal = total - s_prev_total;
            uint64_t didle  = idle  - s_prev_idle;
            if (dtotal > 0) {
                pct = (float)(dtotal - didle) * 100.0f / (float)dtotal;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 100.0f) pct = 100.0f;
            }
        }
        s_prev_total = total;
        s_prev_idle  = idle;
    }
    fclose(fp);
    return pct;
}

/* ==================================================================
 * 进程数 — 从 /proc/loadavg 解析
 * 格式: "0.23 0.15 0.10 1/123 45678"
 *                 第四个字段的 "1/123" → 123 为总进程数
 * ================================================================ */

static int read_proc_count(void)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return -1;

    char line[128];
    int procs = -1;
    if (fgets(line, sizeof(line), fp)) {
        /* 找最后一个空格后的第一个数字 */
        const char *p = strrchr(line, ' ');
        if (!p) { fclose(fp); return -1; }
        p--;
        while (p > line && *p != ' ') p--;
        if (*p == ' ') p++;
        /* p 指向 "1/123" */
        const char *slash = strchr(p, '/');
        if (slash) {
            procs = atoi(slash + 1);
        }
    }
    fclose(fp);
    return procs;
}

/* ==================================================================
 * 公开接口
 * ================================================================ */

SysInfo sysmon_collect(void)
{
    SysInfo info = {0};

    /* CPU */
    info.cpu_pct = collect_cpu_pct();

    /* 内存 */
    {
        int total_kb = read_proc_int("/proc/meminfo", "MemTotal:");
        int free_kb  = read_proc_int("/proc/meminfo", "MemFree:");
        if (total_kb > 0 && free_kb > 0) {
            info.mem_total_kb = total_kb;
            info.mem_free_kb  = free_kb;
        }
    }

    /* 进程数 */
    {
        int p = read_proc_count();
        if (p > 0) info.procs = p;
    }

    return info;
}

void *sysmon_thread(void *arg)
{
    SysMonConfig *cfg = (SysMonConfig *)arg;
    if (!cfg || !cfg->queue) return NULL;

    printf("[sysmon] thread started (interval=%dms)\n",
              cfg->interval_ms);

    while (!g_stop) {
        SysInfo info = sysmon_collect();

        /* 打包成事件发布 */
        Event ev;
        ev.type     = SYS_INFO_READING;
        ev.value    = (int)(info.cpu_pct * 100);  /* 23.45 → 2345 */
        ev.timestamp = time(NULL);

        queue_push(cfg->queue, &ev);

        printf("[sysmon] CPU:%.1f%%  Mem:%d/%dMB  Procs:%d\n",
                  info.cpu_pct,
                  info.mem_free_kb / 1024, info.mem_total_kb / 1024,
                  info.procs);

        usleep(cfg->interval_ms * 1000);
    }

    printf("[sysmon] thread stopped\n");
    return NULL;
}
