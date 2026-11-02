# 智能家居网关（Smart-Gateway）

> 基于 Linux 的物联网网关：epoll 事件驱动 + 线程池架构，串口采集传感器数据、SQLite 本地存储、自定义应用层协议对外提供查询与控制。

作为 Linux 应用层实习备战的核心项目，覆盖 epoll 多路复用、线程池、串口 termios、自定义帧协议（粘包处理）、SQLite、cJSON、信号处理、Makefile/Git 等面试高频考点。

---

## 一、系统架构

```
  传感器模拟器（独立进程 sensor_sim）
        │ 周期发帧（socat 虚拟串口对）
        ▼
  ┌─────────────────────────────┐
  │       智能家居网关 gateway    │
  │                             │
  │  串口采集 ──▶ SQLite 落库     │
  │   epoll 同时管：串口 fd + TCP  │
  │   查询动作丢线程池执行         │
  └──────────────┬──────────────┘
                 │ TCP
                 ▼
        CLI 查询客户端（client）
```

---

## 二、目录结构

```
Smart-Gateway/
├── Makefile               # 编译规则
├── run.sh                 # 一键启动：虚拟串口对 + 模拟器 + 网关
├── .gitignore             # 忽略二进制 / .db / 编辑器配置
│
├── include/               # 头文件（.h）= 接口声明
│   ├── db.h               #   数据库模块接口
│   ├── serial.h           #   串口模块接口
│   └── threadpool.h       #   线程池模块接口 + 结构体
│
├── src/                   # 源码（.c）= 实现
│   ├── main.c             #   主程序：epoll 事件循环，串起所有模块
│   ├── serial.c           #   串口实现（open + termios + read）
│   ├── db.c               #   数据库实现（建表 / 插入 / 查询）
│   ├── threadpool.c       #   线程池实现（创建 / 加任务 / 销毁）
│   └── sensor_sim.c       #   传感器模拟器（独立程序，自带 main）
│
├── third_party/           # 第三方库（非自写，直接用）
│   ├── cJSON.c / cJSON.h  #   JSON 解析库
│   └── sqlite3.c / sqlite3.h  # SQLite 数据库引擎
│
├── test_pool.c            # 线程池独立测试小程序
│
└── （编译/运行产物，git 已忽略，可 make clean 清理）
    ├── gateway            #   主程序可执行文件
    ├── test_app / test_pool / *.o
    └── gateway.db / -shm / -wal   # SQLite 运行时数据库
```

---

## 三、文件说明

### 自写模块（重点掌握）

| 文件 | 作用 | 关键函数 | 面试考点 |
|---|---|---|---|
| `include/serial.h` + `src/serial.c` | 打开串口、配 115200 8N1、读数据 | `serial_init` / `serial_read` / `serial_close` | termios、设备文件 |
| `include/db.h` + `src/db.c` | 建表、存一条、查最新 | `db_init` / `db_insert_sensor` / `db_get_latest` | SQLite C 接口、WAL、busy_timeout |
| `include/threadpool.h` + `src/threadpool.c` | 固定数量 worker 线程排队干活 | `threadpool_create` / `threadpool_add_task` / `threadpool_destroy` | 线程池、条件变量、互斥锁 |
| `src/main.c` | epoll 同时管串口 fd + 网络 fd，事件分派 | `main` | epoll、ET/LT、事件驱动 |
| `src/sensor_sim.c` | 独立进程模拟传感器，周期发帧 | `main` | 独立进程、设备模拟 |
| `test_pool.c` | 线程池测试（学习辅助，非项目本体） | `main` | — |

### 第三方库（只用不改）

| 文件 | 来源 | 用途 |
|---|---|---|
| `third_party/cJSON.c/h` | cJSON | JSON 解析 / 序列化 |
| `third_party/sqlite3.c/h` | SQLite amalgamation | 嵌入式本地数据库 |

---

## 四、快速开始

> 环境：WSL2（Linux 环境，已装 gcc / socat / sqlite3）。代码在 Windows 盘 `E:\code\c-basics\Smart-Gateway`，经 `/mnt/e` 编译运行。

```bash
cd /mnt/e/code/c-basics/Smart-Gateway
./run.sh          # 一键跑通全链路，Ctrl+C 退出
```

另开一个终端查库验证：

```bash
sqlite3 gateway.db 'select * from sensor_data order by id desc limit 5;'
```

只编译不运行：`make gateway sensor_sim`；清理产物：`make clean`。

---

## 五、阶段进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0 | 仓库 + Makefile + vendor cJSON/sqlite3 | ✅ 完成 |
| Phase 1 | 串口采集 + SQLite 落库 + 全链路跑通 | ✅ 完成 |
| Phase 2 | 自定义帧协议（组帧/拆帧/粘包）+ cJSON + 查询历史 + CLI 客户端 | 🔨 进行中 |
| Phase 3 | 控制指令 + 线程池接入 + 信号优雅退出 | ⏳ 待开始 |
| Phase 4 | MQTT / 压测 / valgrind / README 打磨（可选） | ⏳ 待开始 |

---

## 六、.h 与 .c 的分工（约定）

- **`.h` 是菜单**：只放函数声明、结构体定义、`extern` 全局变量，不写实现。
- **`.c` 是后厨**：真正实现函数逻辑。
- **谁要调用别人的模块，就 `#include` 那个模块的 `.h`**（所以 `main.c` 开头有三行 include）。

新增模块时照此模式：接口放 `include/xxx.h`，实现放 `src/xxx.c`，独立程序（自带 main）仿照 `src/sensor_sim.c` 的写法。
