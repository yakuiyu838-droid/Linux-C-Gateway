# 编译器与选项
CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude -Ithird_party
LDLIBS = -lm    # cJSON 用到 sqrt/fabs，链接需要 libm

# 网关：排除 sensor_sim.c（独立模拟器）和 client.c（独立客户端），两者都自带 main()
SRCS = $(filter-out src/sensor_sim.c src/client.c, $(wildcard src/*.c)) $(wildcard third_party/*.c)
# 把 .c 替换成 .o 目标文件
OBJS = $(SRCS:.c=.o)

TARGET = gateway
SIM    = sensor_sim
CLIENT = client

# 默认编译全部三个可执行文件
all: $(TARGET) $(SIM) $(CLIENT)

# 网关主程序（epoll + 串口 + 网络 + 线程池 + 协议）
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# 传感器模拟器（独立程序，单独编译，不混进 gateway）
# 需要链接 proto.c（组帧）和 cJSON.c（构造 JSON payload）
$(SIM): src/sensor_sim.c src/proto.c third_party/cJSON.c
	$(CC) $(CFLAGS) src/sensor_sim.c src/proto.c third_party/cJSON.c -o $@ $(LDLIBS)

# CLI 查询/订阅客户端（独立程序）
$(CLIENT): src/client.c src/proto.c third_party/cJSON.c
	$(CC) $(CFLAGS) src/client.c src/proto.c third_party/cJSON.c -o $@ $(LDLIBS)

# 自动推导规则：把所有的 .c 编译成 .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(SIM) $(CLIENT)
