#ifndef CONFIG_H
#define CONFIG_H
/* 单个传感器配置项 */
typedef struct{
    char name[32];//传感器名字
    int min_val;//读书最小值
    int max_val;//读书最大值
    int interval_ms;//采集时间间隔
}SensorCfgItem;
#define MAX_SENSORS 8//定义最多支持的8个传感器


//整个应用的配置
typedef struct{
    SensorCfgItem sensor[MAX_SENSORS];
    int sensor_count;//实际配置了几个传感器
    int server_port;//tcp服务端口
    int max_clients;//最大客户端数
}AppConfig;


  /*
   * 从文件加载配置
   * path: 配置文件路径（如 "gateway.conf"）
   * cfg:  输出参数，解析结果填这里
   * 返回: 0 成功，-1 失败
   */
int config_load(const char*path,AppConfig *cfg);
/*
   * 加载带默认值的配置：先设默认值，再用文件覆盖
   * 文件不存在时只用默认值，程序不会崩
   */
void config_set_defaults(AppConfig *cfg);
/* 打印配置（启动时看一眼配了啥） */
void config_print(AppConfig *cfg);
#endif