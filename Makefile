# 编译器与选项
CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude -Ithird_party

# 找到所有的 .c 文件（自动去 src 和 third_party 找）
# 排除 sensor_sim.c：它自带 main()，是独立程序，不能混进 gateway
SRCS = $(filter-out src/sensor_sim.c, $(wildcard src/*.c)) $(wildcard third_party/*.c)
# 把 .c 替换成 .o 目标文件
OBJS = $(SRCS:.c=.o)

TARGET = gateway
SIM = sensor_sim

# 最终链接（把所有 .o 拼成可执行文件）
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

# 传感器模拟器（独立程序，单独编译，不混进 gateway）
$(SIM): src/sensor_sim.c
	$(CC) $(CFLAGS) src/sensor_sim.c -o $@

# 自动推导规则：把所有的 .c 编译成 .o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(SIM)