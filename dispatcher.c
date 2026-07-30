    #include "dispatcher.h"   // 自己的头文件
    #include <stdio.h>        // printf
    #include <pthread.h>      // mutex / condvar
    #include <stdlib.h>     // malloc
    #include <signal.h>
  //事件类型转字符串
    const char *event_name(EventType t){
    switch (t)
        {
        case TEMP_READING: return "TEMP_READING";
        case HUMID_READING: return "HUMID_READING";//就是根据 enum 值返回对应的字符串，方便打印日志。
        case LIGHT_READING: return "LIGHT_READING";
        
        default:            return "UNKNOWN";
        }
    }

  //初始化
    void dispatcher_init(Dispatcher *d){
        d->count=0;
    }

  //注册
    void dispatcher_register(Dispatcher *d, EventType type,event_handler h, void *ctx){
        if(d->count>=MAX_HANDLERS){
        printf("[dispatcher] handler table full!\n");
        return;
        }
        int i = d->count++;                  // i = 当前空位，count++
        d->entries[i].type= type;        // 存类型
        d->entries[i].handler= h;           // 存函数
        d->entries[i].ctx = ctx;         // 存上下文指针
    }


    //分发事件
    void dispatcher_dispatch(Dispatcher *d, Event *ev) {
        for (int i = 0; i < d->count; i++) {       // 遍历注册表
            if (d->entries[i].type == ev->type) {   // 找到匹配的
                d->entries[i].handler(ev, d->entries[i].ctx);      //TEMP_READING= 0//HUMID_READING = 1//LIGHT_READING= 2//MAX_EVENT_TYPE =3，用这个来数组越界检查
                return;
             }
        }
    }


    //队列初始化
    void queue_init(EventQueue *q){
        q->head=q->tail=q->size=0;// 1. head、tail、size 全部归零
        q->lock=malloc(sizeof(pthread_mutex_t));// 2. 给互斥锁分配内存
        q->cond=malloc(sizeof(pthread_cond_t));// 3. 给条件变量分配内存
        pthread_mutex_init((pthread_mutex_t*)q->lock,NULL);// 4. 初始化互斥锁
        pthread_cond_init((pthread_cond_t*)q->cond, NULL);// 5. 初始化条件变量
    }


    //队列入队
    void queue_push(EventQueue *q, Event *ev){
        // 1. 加锁 —— 一次只能一个人操作队列
        pthread_mutex_lock((pthread_mutex_t*)q->lock);
        // 2. 队列满了？丢弃
        if(q->size>=QUEUE_CAP){
            printf("[queue] full! discarding event %s\n", event_name(ev->type));
            pthread_mutex_unlock((pthread_mutex_t*)q->lock);//解锁再 return！
            return;
        }
        // 3. 把事件拷贝到队尾
        q->events[q->tail]=*ev;// 结构体赋值，把 ev 的内容拷进数组
        q->tail=(q->tail+1)%QUEUE_CAP;//位置镇后一，%cap保证环形操作
        q->size++;
        pthread_cond_signal((pthread_cond_t*)q->cond);// 6. 通知消费者：有数据了！
         // 7. 解锁
         pthread_mutex_unlock((pthread_mutex_t*)q->lock);
    }

    //队列出队
    int queue_pop(EventQueue *q, Event *ev){
        //1.lock
        pthread_mutex_lock((pthread_mutex_t*)q->lock);
        while(q->size<=0)// 队列空了？等着（同时释放锁）
        {
            pthread_cond_wait((pthread_cond_t*)q->cond,(pthread_mutex_t*)q->lock);
            extern volatile sig_atomic_t g_stop;
            if (g_stop) {
            pthread_mutex_unlock((pthread_mutex_t*)q->lock);
            return 0;  // 返回 0 表示"被停止"
          }
        }
        
        //3. 拷贝队头到 ev
        *ev=q->events[q->head];
         // 4. 头指针后移（环形）
        q->head=(q->head+1)%QUEUE_CAP;
        // 5. 元素个数 -1
        q->size--;
        // 6. 解锁
        pthread_mutex_unlock((pthread_mutex_t*)q->lock);
        return 1;// 表示成功拿到事件
   // 事件要发到哪个队列？ 
    }

    //摧毁队列
    void queue_destroy(EventQueue *q){
         // 1. 销毁互斥锁
        pthread_mutex_destroy((pthread_mutex_t*)q->lock);
        //2. 销毁条件变量
        pthread_cond_destroy((pthread_cond_t*)q->cond);
        free(q->lock);
        free(q->cond);
    }

    void queue_wakeup(EventQueue*q){
        pthread_cond_broadcast((pthread_cond_t*)q->cond);
        // 广播唤醒所有在 cond_wait 里睡着的线程
        // 它们醒后会检查 while(size==0) → 发现还是空 → 再睡
        // 但如果 stop=1 了，主循环会 break，不再 pop
    }
