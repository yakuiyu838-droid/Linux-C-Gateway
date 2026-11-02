import struct
import json
import os
import socket
import time

# 封装打包函数（和你的 C 语言 proto_pack 完全一样）
def pack_frame(msg_type, json_str):
    payload = json_str.encode('utf-8')
    length = len(payload)
    # Magic(2B: 0xA5 0x5A) + Version(1B: 0x01) + Type(1B) + Length(2B大端)
    header = struct.pack('!BBBBH', 0xA5, 0x5A, 0x01, msg_type, length)
    frame = header + payload
    # 计算校验和 (累加和 % 256)
    checksum = sum(frame) % 256
    return frame + struct.pack('B', checksum)

# 封装拆包函数（用于解析网关返回的数据）
def unpack_frame(data):
    if len(data) < 7: return None
    magic1, magic2, version, msg_type, length = struct.unpack('!BBBBH', data[:6])
    if magic1 != 0xA5 or magic2 != 0x5A:
        return None
    payload = data[6:6+length].decode('utf-8')
    return msg_type, payload

def test_serial():
    print("\n--- [测试 1] 模拟串口发数据 ---")
    tty_path = "/tmp/gw_tty_write" # 注意这里是写端
    if not os.path.exists(tty_path):
        print(f"❌ 找不到 {tty_path}，请确认 socat 是否运行！")
        return

    # 模拟传感器发 JSON
    sensor_data = {"temp": 28.5, "hum": 65.0}
    json_str = json.dumps(sensor_data)
    frame = pack_frame(1, json_str) # Type=1 代表传感器数据

    with open(tty_path, 'wb') as f:
        f.write(frame)
        f.flush()
    print(f"✅ 成功向串口写入二进制帧 (Hex): {frame.hex()}")
    print(f"   Payload 是: {json_str}")

def test_network():
    print("\n--- [测试 2] 模拟网络客户端查数据 ---")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8888))
    print("✅ 成功连接到网关 8888 端口")

    # 模拟客户端发送查询请求
    req_data = {"cmd": "get_latest"}
    json_str = json.dumps(req_data)
    frame = pack_frame(2, json_str) # Type=2 代表查询请求

    s.send(frame)
    print(f"✅ 发送查询指令: {json_str}")

    # 接收网关返回的数据
    res = s.recv(4096)
    parsed = unpack_frame(res)
    if parsed:
        msg_type, payload = parsed
        print(f"✅ 收到网关响应! Type={msg_type}, 解析后的 JSON 数据: {payload}")
    else:
        print("❌ 解析响应失败，可能没收到数据或数据被截断")
    s.close()

if __name__ == "__main__":
    test_serial()
    time.sleep(1) # 稍微等一下让网关存入数据库
    test_network()