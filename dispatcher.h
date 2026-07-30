#ifndef DISPATCHER_H
#define DISPATCHER_H

#include <time.h>

typedef enum {
    TEMP_READING,     // 温度读数
    HUMID_READING,    // 湿度读数
    LIGHT_READING,    // 光照读数
    PRESSURE_READING, // 气压读数
    GAS_READING,      // 燃气读数
    MAX_EVENT_TYPE
} EventType;

const char* event_name(EventType t);

typedef struct {
    EventType type;
    int       value;      // 读数（温度×10避免浮点）
    time_t    timestamp;
} Event;

typedef void (*event_handler)(Event *ev, void *ctx);

typedef struct {
    EventType     type;
    event_handler handler;
    void         *ctx;
} HandlerEntry;

#define MAX_HANDLERS 16

typedef struct {
    HandlerEntry entries[MAX_HANDLERS];
    int          count;
} Dispatcher;

void dispatcher_init(Dispatcher *d);
void dispatcher_register(Dispatcher *d, EventType type, event_handler h, void *ctx);
void dispatcher_dispatch(Dispatcher *d, Event *ev);

#define QUEUE_CAP 128

typedef struct {
    Event events[QUEUE_CAP];
    int   head, tail, size;
    void *lock;
    void *cond;
} EventQueue;

void queue_init(EventQueue *q);
void queue_push(EventQueue *q, Event *ev);
int  queue_pop(EventQueue *q, Event *ev);
void queue_wakeup(EventQueue *q);
void queue_destroy(EventQueue *q);

#endif
