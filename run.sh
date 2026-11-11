#!/usr/bin/env bash
# 一键跑通 Phase 1 全链路：虚拟串口对 + 传感器模拟器 + 网关
# 用法：./run.sh   （Ctrl+C 退出，自动清理后台进程）

set -e

SOCAT_PID=""
SIM_PID=""

# 退出时（含 Ctrl+C）自动清理后台进程和临时软链接
cleanup() 
{
    echo ""
    echo "=== 清理中 ==="
    [ -n "$SIM_PID" ] && kill "$SIM_PID" 2>/dev/null
    [ -n "$SOCAT_PID" ] && kill "$SOCAT_PID" 2>/dev/null
    rm -f /tmp/gw_tty_write /tmp/gw_tty_read
}
trap cleanup EXIT

# 1. 编译网关 + 模拟器
make gateway sensor_sim

# 2. 用 socat 建一对虚拟串口：写一端 -> 读一端
rm -f /tmp/gw_tty_write /tmp/gw_tty_read
echo "=== 启动虚拟串口对 (socat) ==="
socat -d -d pty,raw,echo=0,link=/tmp/gw_tty_write pty,raw,echo=0,link=/tmp/gw_tty_read &
SOCAT_PID=$!
sleep 1

# 3. 启动传感器模拟器（后台，每 2 秒发一帧温度/湿度）
echo "=== 启动传感器模拟器 ==="
./sensor_sim /tmp/gw_tty_write 2 &
SIM_PID=$!

# 4. 启动网关（前台，读 /tmp/gw_tty_read，Ctrl+C 退出）
echo "=== 启动网关 ==="
echo "（另开终端查库：sqlite3 gateway.db 'select * from sensor_data order by id desc limit 5;'）"
./gateway
