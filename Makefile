CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -O2
LIBS = -lpthread -lm

TARGET = gateway
OBJS = main.o dispatcher.o sensor.o server.o config.o \
       json_proto.o netlink_monitor.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean run