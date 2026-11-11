#!/usr/bin/env bash
# valgrind 内存泄漏检测：把网关跑在 valgrind 下，打一轮混合负载，再优雅退出看泄漏报告
# 前置：sudo apt install valgrind
# 用法：./valgrind_test.sh

cd "$(dirname "$0")"

# 清理残留进程和临时文件
pkill -x gateway 2>/dev/null; pkill -x sensor_sim 2>/dev/null; pkill -x socat 2>/dev/null
sleep 1
rm -f gateway.db gateway.db-wal gateway.db-shm /tmp/gw_tty_write /tmp/gw_tty_read

# 1. 虚拟串口对
socat pty,raw,echo=0,link=/tmp/gw_tty_write pty,raw,echo=0,link=/tmp/gw_tty_read >/dev/null 2>&1 &
SOCAT_PID=$!
sleep 1

# 2. 传感器模拟器
./sensor_sim /tmp/gw_tty_write 1 >/dev/null 2>&1 &
SIM_PID=$!
sleep 1

# 3. 网关跑在 valgrind 下，日志写到 /tmp/vg.log
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
    --log-file=/tmp/vg.log ./gateway >/tmp/gw.log 2>&1 &
GW_PID=$!
sleep 3

# 4. 打一轮混合负载：查询 + 控制 + 订阅（尽量覆盖所有路径）
printf "latest\nhistory 5\ncontrol relay1 on\ncontrol relay1 off\nexit\n" | ./client >/dev/null 2>&1

# 5. 发 SIGINT 触发优雅退出，让 valgrind 输出最终泄漏报告
kill -INT $GW_PID 2>/dev/null
wait $GW_PID 2>/dev/null

# 6. 清理
kill $SIM_PID $SOCAT_PID 2>/dev/null

# 7. 展示结果
echo "===== valgrind 泄漏摘要 ====="
grep -A5 "LEAK SUMMARY" /tmp/vg.log
echo ""
echo "===== 关键：definitely lost 应为 0 ====="
grep "definitely lost" /tmp/vg.log
echo ""
echo "完整报告：/tmp/vg.log"
