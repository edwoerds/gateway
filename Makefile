CC = gcc   # 编译器用 gcc
CFLAGS = -Wall -Wextra -O2# 编译选项：打开所有警告 + 二级优化
LIBS = -lpthread# 链接 pthread 库

TARGET = gateway# 最终生成的可执行文件名
OBJS = main.o dispatcher.o sensor.o server.o config.o

all: $(TARGET)# 默认目标（只输入 make 时执行）

$(TARGET): $(OBJS)# 链接：把 .o 文件链接成可执行文件
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c # 编译：每个 .c 文件编译成对应的 .o 文件
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)# 编译并运行
	./$(TARGET)

clean:# 清理：删掉生成的文件
	rm -f $(TARGET) $(OBJS)
	
.PHONY: all clean run # 声明这些目标是伪目标（不是文件名）