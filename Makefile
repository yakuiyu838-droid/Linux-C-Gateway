# 编译器与选项
CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude -Ithird_party
LDLIBS = -lm    # cJSON 用到 sqrt/fabs，链接需要 libm

# 网关：排除自带 main() 的独立程序（模拟器/客户端/压测客户端）
SRCS = $(filter-out src/sensor_sim.c src/client.c src/stress.c, $(wildcard src/*.c)) $(wildcard third_party/*.c)
# 把 .c 替换成 .o 目标文件
OBJS = $(SRCS:.c=.o)

TARGET = gateway
SIM    = sensor_sim
CLIENT = client
STRESS = stress

# 默认编译全部四个可执行文件
all: $(TARGET) $(SIM) $(CLIENT) $(STRESS)

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

# 压测客户端（独立程序，不用 cJSON，只需要协议组帧/拆帧）
$(STRESS): src/stress.c src/proto.c
	$(CC) $(CFLAGS) src/stress.c src/proto.c -o $@ $(LDLIBS) -lpthread

# 自动推导规则：把所有的 .c 编译成 .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(SIM) $(CLIENT) $(STRESS)
