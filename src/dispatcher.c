#include "dispatcher.h"
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <signal.h>

extern volatile sig_atomic_t g_stop;

/* 事件类型 → 字符串（日志用） */
const char *event_name(EventType t)
{
    switch (t) {
    case TEMP_READING:     return "TEMP_READING";
    case HUMID_READING:    return "HUMID_READING";
    case LIGHT_READING:    return "LIGHT_READING";
    case PRESSURE_READING: return "PRESSURE_READING";
    case GAS_READING:      return "GAS_READING";
    case SYS_INFO_READING: return "SYS_INFO_READING";
    default:               return "UNKNOWN";
    }
}

/* 初始化分发器 */
void dispatcher_init(Dispatcher *d)
{
    d->count = 0;
}

/* 注册回调：事件类型 → 处理函数 */
void dispatcher_register(Dispatcher *d, EventType type,
                         event_handler h, void *ctx)
{
    if (d->count >= MAX_HANDLERS) {
        printf("[dispatcher] handler table full!\n");
        return;
    }
    int i = d->count++;
    d->entries[i].type    = type;
    d->entries[i].handler = h;
    d->entries[i].ctx     = ctx;
}

/* 分发：遍历注册表，找到匹配的事件类型并调用回调 */
void dispatcher_dispatch(Dispatcher *d, Event *ev)
{
    for (int i = 0; i < d->count; i++) {
        if (d->entries[i].type == ev->type) {
            d->entries[i].handler(ev, d->entries[i].ctx);
            return;
        }
    }
}

/* 初始化事件队列（环形缓冲 + mutex + condvar） */
void queue_init(EventQueue *q)
{
    q->head = q->tail = q->size = 0;
    q->lock = malloc(sizeof(pthread_mutex_t));
    q->cond = malloc(sizeof(pthread_cond_t));
    pthread_mutex_init((pthread_mutex_t *)q->lock, NULL);
    pthread_cond_init((pthread_cond_t *)q->cond, NULL);
}

/* 入队（生产者）。队列满则丢弃，不阻塞生产者 */
void queue_push(EventQueue *q, Event *ev)
{
    int dropped = 0;

    pthread_mutex_lock((pthread_mutex_t *)q->lock);
    if (q->size >= QUEUE_CAP) {
        dropped = 1;
    } else {
        q->events[q->tail] = *ev;
        q->tail = (q->tail + 1) % QUEUE_CAP;
        q->size++;
        pthread_cond_signal((pthread_cond_t *)q->cond);
    }
    pthread_mutex_unlock((pthread_mutex_t *)q->lock);

    /* 日志放锁外，避免锁内做 IO */
    if (dropped)
        printf("[queue] full! discarding event %s\n", event_name(ev->type));
}

/* 出队（消费者）。空则睡眠等待；g_stop 置位时返回 0 */
int queue_pop(EventQueue *q, Event *ev)
{
    pthread_mutex_lock((pthread_mutex_t *)q->lock);
    while (q->size <= 0) {
        pthread_cond_wait((pthread_cond_t *)q->cond,
                          (pthread_mutex_t *)q->lock);
        if (g_stop) {
            pthread_mutex_unlock((pthread_mutex_t *)q->lock);
            return 0;   /* 被停止 */
        }
    }
    *ev = q->events[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->size--;
    pthread_mutex_unlock((pthread_mutex_t *)q->lock);
    return 1;
}

/* 摧毁队列 */
void queue_destroy(EventQueue *q)
{
    pthread_mutex_destroy((pthread_mutex_t *)q->lock);
    pthread_cond_destroy((pthread_cond_t *)q->cond);
    free(q->lock);
    free(q->cond);
}

/* 广播唤醒所有等待的消费者 */
void queue_wakeup(EventQueue *q)
{
    pthread_cond_broadcast((pthread_cond_t *)q->cond);
}
