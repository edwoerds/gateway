    #ifndef SENSOR_H
    #define SENSOR_H

    #include "dispatcher.h"

    typedef struct{
        char name[32];// 传感器名字（"TempSensor"等）
        EventType event_type;// 产生什么类型的事件
        int min_val;// 读数最小值
        int max_val;//读叔最大值
        int interval_ms;  // 采集间隔（毫秒）
        EventQueue* queue;// 事件要发到哪个队列？
    }SensorConfig;

    void* sensor_thread(void *arg);
    #endif // SENSOR_H