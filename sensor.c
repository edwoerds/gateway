#include<stdio.h>
#include"sensor.h"
#include<stdlib.h>
#include<unistd.h>
#include<time.h>
#include<signal.h>
extern volatile sig_atomic_t g_stop;
/* 模拟传感器读数：返回 [min, max] 之间的随机数 */
static int read_sensor(int min,int max){
    return min + rand() % (max - min + 1);
}

 /* 传感器线程：循环 采集→入队 */
void *sensor_thread(void *arg){
    SensorConfig *cfg=(SensorConfig *)arg;// 把 void* 转回实际类型
    printf("[%s] sensor started (interval=%dms, range=[%d,%d])\n",cfg->name, cfg->interval_ms, cfg->min_val, cfg->max_val);
    while(!g_stop){
        usleep(cfg->interval_ms*1000);// ① 睡到下次采集时间
        Event ev;
        ev.type=cfg->event_type;    // ② 打包事件
        ev.value=read_sensor(cfg->min_val, cfg->max_val);
        ev.timestamp=time(NULL);

        queue_push(cfg->queue,&ev);// ③ 扔进队列
    }
    free(cfg);
    return NULL;
}