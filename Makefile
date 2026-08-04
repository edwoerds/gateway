# ============================================================
# 网关项目 — 多目录构建 Makefile
# 目录结构：src/<模块>/xxx.c  →  build/<模块>/xxx.o  →  gateway
#   core/   主循环 + 分发器
#   net/    TCP 服务器 + netlink 系统监控
#   proto/  JSON 协议
#   sensor/ 传感器
#   config/ 配置文件
# ============================================================

# 交叉编译开关：默认本机 gcc；
# 交叉编译时用 make CROSS_COMPILE=aarch64-linux-gnu-
CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc

# -MMD -MP 编译时自动生成头文件依赖，头文件改了对应 .o 自动重编
CFLAGS  := -Wall -Wextra -std=c11 -D_GNU_SOURCE -O2 -Iinc
LDFLAGS := -lpthread -lm

SRC_DIR   := src
BUILD_DIR := build

# 各模块源文件列表（\ 是续行符，每行一个模块的源文件）
SRCS := \
	$(SRC_DIR)/core/main.c \
	$(SRC_DIR)/core/dispatcher.c \
	$(SRC_DIR)/net/server.c \
	$(SRC_DIR)/net/netlink_monitor.c \
	$(SRC_DIR)/proto/json_proto.c \
	$(SRC_DIR)/sensor/sensor.c \
	$(SRC_DIR)/config/config.c

# src/xxx.c → build/xxx.o（保留子目录层级，不同模块的同名文件不冲突）
OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

# 依赖文件 .d（-MMD 生成，-include 引入）
DEPS := $(OBJS:.o=.d)

TARGET := gateway

.PHONY: all run clean

all: $(TARGET)

# 通用编译规则：$< = 源文件，$@ = 目标文件
# 先 mkdir 出 build/<模块>/ 目录再编译，保证子目录产物各归其位
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# 引入头文件依赖（- 开头：.d 文件还不存在时不报错）
-include $(DEPS)
