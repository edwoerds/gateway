#include "server.h"
#include "dispatcher.h"   // Event 类型
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>        // fcntl（设置非阻塞）
#include <sys/socket.h>   // socket / bind / listen / accept
#include <netinet/in.h>   // struct sockaddr_in
#include <arpa/inet.h>    // inet_ntoa（IP地址转字符串）
#include <sys/epoll.h>    // epoll 全家桶
#include <pthread.h>      // 线程锁
#include <signal.h>

#define MAX_CLIENTS 64
#define MAX_EVENTS 64
extern volatile sig_atomic_t g_stop;

//第2块：客户端列表结构体
typedef struct {
    int clients[MAX_CLIENTS];//已连接的客户端 socket 数组
    int count;// 当前有几个客户端
    pthread_mutex_t lock;        // 操作客户端列表要上锁（多线程安全）
}ClientList;

static ClientList g_clients;// 全局变量，存所有客户端
 // 存最新传感器值（用于 LIST 命令回复）
  static int  g_last_temp = 0;
  static int  g_last_humi = 0;
  static int  g_last_light = 0;
  static int  g_last_pressure = 0;
  static int  g_last_gas = 0;
  static pthread_mutex_t g_data_lock = PTHREAD_MUTEX_INITIALIZER;
//第3块：设置 socket 为非阻塞

static void set_nonblocking(int fd){
    int flags=fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//第4块：向所有客户端广播数据
void server_broadcast(const char *msg){
    pthread_mutex_lock(&g_clients.lock); // ① 遍历列表前提锁
    for(int i=0;i<g_clients.count;i++)
    {
        int fd=g_clients.clients[i];
        ssize_t n=write(fd, msg, strlen(msg));// ② 逐个发
        if(n==0){
            // 客户端断开或出错 → 关闭连接，从列表移除
            close(fd);
            // 把最后一个元素移过来填补空位
            g_clients.clients[i]=g_clients.clients[g_clients.count-1];
            g_clients.count--;
            i--;// 继续检查这个位置（新移过来的）
        }
    }
    pthread_mutex_unlock(&g_clients.lock);// ③ 遍历完解锁
}

//第5块：添加新客户端到列表
static void add_client(int fd){
    pthread_mutex_lock(&g_clients.lock);

    if(g_clients.count<MAX_CLIENTS){
        g_clients.clients[g_clients.count]=fd;
        g_clients.count++;
        printf("[server] new client connected (total: %d)\n", g_clients.count);
    }else{
        //客户端曼联，拒绝连接
        printf("[server] max clients reached, reject\n");
        close(fd);
    }
    pthread_mutex_unlock(&g_clients.lock);
}

//第5.5块：移除客户端
static void remove_client(int fd,int epfd){
    // 从 epoll 监听列表移除
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    //从客户端列表移除
    pthread_mutex_lock(&g_clients.lock);
    for(int i=0;i<g_clients.count;i++){
        if(g_clients.clients[i]==fd)
        {
            g_clients.clients[i]=g_clients.clients[g_clients.count-1];
            g_clients.count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_clients.lock);
    close(fd);
    printf("[server] client disconnected (total: %d)\n", g_clients.count);
}
// 第6块：处理客户端发来的消息（比如 LIST 命令）
static int handle_client(int fd){
    char buf[1024];
    ssize_t n=read(fd,buf,sizeof(buf)-1);
    if(n<=0){
        return -1;
        // 客户端断开或出错
    }
    buf[n]='\0';//变成字符串
    //去掉末尾的黄行副
    if(buf[n-1]=='\n') buf[n-1]='\0';
    //处理命令
    if(strcmp(buf,"LIST")==0){
        //回复目前最新的传感器值
        pthread_mutex_lock(&g_data_lock);
        char reply[256];
        snprintf(reply, sizeof(reply), "🌡️  TEMP %d.%d°C 💧  HUMI %d%% ☀️   LIGHT %d lux\n",g_last_temp / 10, g_last_temp % 10,g_last_humi, g_last_light);
        ssize_t _unused = write(fd,reply,strlen(reply));
        (void)_unused;
        pthread_mutex_unlock(&g_data_lock);
    }else{
        printf("[server] unknown command: %s\n", buf);
    }
    return 0;
}

//第7块：服务器主函数server_start — epoll 主循环（核心）
//先定义端口，后面改成传参
void *server_thread_func(void *arg){
    (void)arg;
    int port=8888;

//创建socket
int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
      if (listen_fd < 0) {
          perror("[server] socket");
          return NULL;
      }
// 2.设置 SO_REUSEADDR，防止 "Address already in use"
int opt=1;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
// 3.绑定地址和端口
struct sockaddr_in addr;
memset(&addr,0,sizeof(addr));
addr.sin_family=AF_INET;
addr.sin_addr.s_addr=INADDR_ANY;
addr.sin_port=htons(port);
if(bind(listen_fd,(struct sockaddr*)&addr,sizeof(addr))<0){
    perror("[server] bind");
    close(listen_fd);
    return NULL;
}
// ④ 开始监听
if(listen(listen_fd,10)<0){
    perror("[server] listen");
    close(listen_fd);
    return NULL;
}
// ⑤ 设置监听 socket 为非阻塞
set_nonblocking(listen_fd);//新连接 accept 也用非阻塞，保持一致。
// ⑥ 创建 epoll 实例
int epfd=epoll_create1(0);
if(epfd<0){
    perror("[server] epoll_create1");
    close(listen_fd);
    return NULL;
}
// ⑦ 注册监听 socket 到 epoll
struct epoll_event ev;
ev.events=EPOLLIN;// 可读事件 + 边缘触发
//EPOLLIN：有数据可读（或新连接到来）
//EPOLLET：边缘触发（Edge Triggered） — 只在状态变化时通知一次。水平触发（默认）是只要还有数据就一直通知
ev.data.fd=listen_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);// 告诉 epoll："帮我盯着 listen_fd，有新连接了通知我"
printf("[server] listening on port %d\n", port);

// ⑧ 事件循环
struct epoll_event events[MAX_EVENTS];
while(!g_stop){
    int nfds=epoll_wait(epfd,events, MAX_EVENTS, 1000);//-1会一直等待，每1000毫秒检查一次
    // nfds = 这次有几个事件发生（-1 = 永远等，不超时）
    for(int i=0;i<nfds;i++){
        // ⑨ 监听 socket 有事件 → 新的连接来了
        if (events[i].data.fd == listen_fd) {
            struct sockaddr_in client_addr;
            socklen_t client_len=sizeof(client_addr);
            int client_fd=accept(listen_fd,(struct sockaddr*)&client_addr,&client_len);
                if(client_fd<0){
                perror("[server] accept");
                continue;
                }
            // 新客户端也设成非阻塞
            set_nonblocking(client_fd);
            // 把新客户端加入 epoll 监听
            struct epoll_event client_ev;
            client_ev.events=EPOLLIN;
            client_ev.data.fd=client_fd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &client_ev);//盯着client_fd，有数据来通知我
            // 加入全局客户端列表
            add_client(client_fd);
            }
            // ⑩ 已连接的客户端发数据来了
            else{
                // 客户端有数据或断开了
                int client_fd=events[i].data.fd;
                if(handle_client(client_fd)<0){
                    remove_client(events[i].data.fd, epfd);
                }
            }
        }  
    }
    close(listen_fd);
    close(epfd);
    return NULL;
}
    //第12块：对外接口 — main.c 调用这个启动服务器线程
void server_start(int port){
    (void)port;
    pthread_t tid;
    pthread_create(&tid, NULL, server_thread_func, NULL);
}

void server_on_sensor(EventType type,int value){
          // ① 存最新值
      pthread_mutex_lock(&g_data_lock);
      switch (type) {
          case TEMP_READING:  g_last_temp  = value; break;
          case HUMID_READING: g_last_humi  = value; break;
          case LIGHT_READING: g_last_light = value; break;
          case PRESSURE_READING: g_last_pressure = value; break;
          case GAS_READING:      g_last_gas      = value; break;
          default: break;
      }
      pthread_mutex_unlock(&g_data_lock);
            // ② 拼消息 → 广播
      char msg[128];
      switch (type) {
          case TEMP_READING:
              snprintf(msg, sizeof(msg), "🌡️  TEMP %d.%d°C\n",
                       value / 10, value % 10);
              break;
          case HUMID_READING:
              snprintf(msg, sizeof(msg), "💧  HUMI %d%%\n", value);
              break;
          case LIGHT_READING:
              snprintf(msg, sizeof(msg), "☀️   LIGHT %d lux\n", value);
              break;
          default: return;
      }
      server_broadcast(msg);
}