CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude -Ithird_party

# 目标名
TARGET = gateway

# 源文件
SRCS = src/main.c third_party/cJSON.c third_party/sqlite3.c

# 编译规则
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: clean