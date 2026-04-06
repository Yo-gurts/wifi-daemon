# wifi-daemon 设计文档（基于当前实现）

## 1. 文档范围

本文档基于仓库当前代码实现梳理 `wifi-daemon` 的架构与行为，覆盖：

- 进程模型与模块职责
- IPC 协议与命令语义
- 与 `wpa_supplicant` 的交互流程
- 错误处理与状态边界
- 现有限制与演进建议

对应代码：

- `src/wifi_daemon.c`
- `src/wpa_ctrl_compat.c`
- `include/proto/wifi_proto.h`

## 2. 系统目标

`wifi-daemon` 提供一个本地 Unix Domain Socket 服务，为上层进程（如 UI/业务进程）封装常见 Wi-Fi 操作：

- 开关网卡
- 扫描启动与扫描结果读取
- 连接指定 SSID（可带密码）
- 断开连接
- 忘记已保存网络

核心定位：**将上层逻辑与 `wpa_supplicant` 控制接口解耦**，并以简化文本协议提供稳定接入点。

## 3. 非目标（当前实现）

- 不提供并发多客户端处理
- 不提供权限鉴权
- 不做 DHCP/IP 层配置
- 不支持 BSSID 定向连接、企业认证（802.1X）等高级特性

## 4. 运行与依赖

### 4.1 运行环境依赖

- `wpa_supplicant` 已启动且控制接口可用：
  - `WLAN_CTRL_PATH=/var/run/wpa_supplicant/wlan0`
- 系统存在 `ifconfig` 命令（用于 `SET_ENABLED`）
- daemon 监听 Unix Socket：
  - `WIFI_SOCKET_PATH=/tmp/aicam_wifi.sock`

### 4.2 构建

- 交叉编译工具链由 `Makefile` 指定（可覆写 `CROSS_COMPILE`）
- 可执行文件输出：`bin/wifi-daemon`

## 5. 高层架构

```text
+------------------+        Unix Stream Socket        +-------------------+
| Client Process   |  <----------------------------->  | wifi-daemon       |
| (UI/Service)     |      /tmp/aicam_wifi.sock        | (single process)  |
+------------------+                                   +---------+---------+
                                                                  |
                                                                  | wpa_ctrl_request()
                                                                  v
                                                        +-------------------+
                                                        | wpa_supplicant    |
                                                        | ctrl iface (UDS)  |
                                                        +-------------------+
```

### 5.1 进程模型

- 单进程、多线程：主线程处理客户端请求，事件线程监听 wpa_supplicant 事件
- 主循环 `accept()` 新连接后，读取一次请求并同步处理，随后关闭连接
- 事件线程通过 `wpa_ctrl_attach()` 订阅事件，收到 `CTRL-EVENT-SCAN-RESULTS` 时更新扫描缓存
- 扫描结果缓存带互斥锁保护，支持 scan_id 机制追踪结果新鲜度

### 5.2 模块职责

1. IPC 层（`wifi_daemon.c`）
- 监听 socket
- 解析命令（Tab/换行分隔）
- 返回文本响应

2. Wi-Fi 控制层（`wifi_daemon.c`）
- 通过 `run_cmd()` 转发命令给 `wpa_supplicant`
- 封装扫描、连接、断开、忘记网络等逻辑

3. `wpa_ctrl` 兼容层（`wpa_ctrl_compat.c`）
- 实现 `wpa_ctrl_open/request/close` 等最小接口
- 使用 `AF_UNIX + SOCK_DGRAM` 对接 `wpa_supplicant` 控制口

## 6. 数据与状态

### 6.1 关键结构

- `known_network_t`
  - `network_id`
  - `ssid`
  - `flags`

用于缓存 `LIST_NETWORKS` 解析结果，辅助判断：
- 目标 SSID 是否已保存
- 哪个网络当前连接（`[CURRENT]`）

### 6.2 全局状态

- `g_stop`：退出标志（SIGINT/SIGTERM 设置）
- `g_enabled`：WiFi 期望启用标志（由 `SET_ENABLED`/`CONNECT` 维护），用于控制事件线程与自愈逻辑
- `g_ctrl`：到 `wpa_supplicant` 的控制连接（惰性初始化，可复用）
- `g_ctrl_ev`：到 `wpa_supplicant` 的事件监控连接（用于异步接收扫描结果）
- `g_scan_mutex`：`g_scan_cache` 的保护互斥锁
- `g_ctrl_mutex`：`g_ctrl`/`g_ctrl_ev` 的保护互斥锁
- `g_scan_cache`：扫描结果缓存
- `g_scan_valid`：扫描结果是否有效
- `g_scan_id`：递增的扫描 ID，客户端可判断结果是否更新

### 6.3 连接状态判定

`CONNECT` 成功的判据：

- `STATUS` 输出中同时包含：
  - `wpa_state=COMPLETED`
  - `ssid=`
  - 目标 SSID 子串

轮询超时：20 秒（200ms 间隔）

## 7. IPC 协议设计

### 7.1 传输

- Unix Domain Socket（`SOCK_STREAM`）
- 路径：`/tmp/aicam_wifi.sock`
- 单次请求-响应模型（每连接处理一条命令）

### 7.2 编码

- 请求：文本行，字段用 `\t` 分隔，末尾 `\n`
- 响应：文本行，字段用 `\t` 分隔，末尾 `\n`

### 7.3 命令与响应

0. `GET_STATUS`
- 行为：
  - 通过 `ioctl(SIOCGIFFLAGS)` 检查 `wlan0` 是否 `IFF_UP`，得到 `enabled`
  - 仅在 `enabled=1` 时发送 `STATUS` 给 `wpa_supplicant` 判定连接态
  - 仅在已连接时读取 RSSI
- 成功：`OK\tSTATUS\t<enabled>\t<connected>\t<rssi_dbm>`
  - `enabled`: `0` 表示接口不存在或未 UP，`1` 表示接口已 UP
  - `connected`: `1` 表示 `wpa_state=COMPLETED` 且存在 `ssid=`
  - `rssi_dbm`: 已连接时返回实时 RSSI，未连接为 `-1`

1. `SET_ENABLED\t0|1`
- 行为：`ifconfig wlan0 down|up`，启用时同时打开 wpa_ctrl 连接
- 成功：`OK\tSTATE`
- 失败：`ERR\tIF_DOWN` / `ERR\tIF_UP` / `ERR\tCTRL_OPEN`

2. `SCAN_START`
- 行为：转发 `SCAN`，递增 scan_id 使缓存失效
- 成功：`OK\tSCAN_STARTED\t<scan_id>`
- 失败：`ERR\tSCAN_START`

3. `SCAN_GET`
- 行为：
  - 返回缓存的扫描结果（由事件线程异步更新）
  - 查询 `LIST_NETWORKS` 判断 saved/connected 状态
  - 按信号强度降序排列
- 成功头：`OK\tSCAN\t<scan_id>`
- 数据行：`AP\t<ssid>\t<signal>\t<secured>\t<saved>\t<connected>`
- 结束：`END`
- 失败：`ERR\tSCAN_GET`

字段说明：
- `scan_id`：递增整数，客户端可判断结果是否为自己发起的扫描产生
- `secured`: flags 中包含 `WPA|WEP|SAE` 则为 `1`，否则 `0`
- `saved`: SSID 出现在 `LIST_NETWORKS` 则为 `1`
- `connected`: 对应 network flags 含 `[CURRENT]` 则为 `1`

4. `CONNECT\t<ssid>\t[password]`
- 行为：
  - 若 SSID 未保存：`ADD_NETWORK` + `SET_NETWORK ssid` + `SET_NETWORK psk/key_mgmt`
  - `SELECT_NETWORK`
  - `SAVE_CONFIG`
  - 轮询 `STATUS` 等待连接完成
- 成功：`OK\tCONNECTED`
- 失败：
  - `ERR\tEINVAL`
  - `ERR\tADD_NETWORK`
  - `ERR\tSET_SSID`
  - `ERR\tSET_PSK`
  - `ERR\tSELECT_NETWORK`
  - `ERR\tCONNECT_TIMEOUT`

5. `DISCONNECT`
- 行为：`DISCONNECT`
- 成功：`OK\tDISCONNECTED`
- 失败：`ERR\tDISCONNECT`

6. `FORGET\t<ssid>`
- 行为：按 SSID 找到 network id 后 `REMOVE_NETWORK` + `SAVE_CONFIG`
- 成功：`OK\tFORGOT`
- 失败：
  - `ERR\tEINVAL`
  - `ERR\tNOT_FOUND`
  - `ERR\tFORGET`

7. 未知命令
- 返回：`ERR\tUNKNOWN_CMD`

## 8. 关键流程

### 8.1 扫描流程

1. 客户端调用 `SCAN_START`
2. daemon 转发 `SCAN`
3. 客户端稍后调用 `SCAN_GET`
4. daemon 返回 AP 列表与保存/连接标记

### 8.2 连接流程

1. 客户端发送 `CONNECT\tssid\tpassword`
2. daemon 判断是否已有 profile
3. 必要时创建 profile 并配置凭据
4. 执行 `SELECT_NETWORK`
5. 轮询 `STATUS`，20 秒内完成则返回 `OK\tCONNECTED`

### 8.3 忘记网络流程

1. 客户端发送 `FORGET\tssid`
2. daemon 从 `LIST_NETWORKS` 匹配 id
3. 执行 `REMOVE_NETWORK <id>`
4. 执行 `SAVE_CONFIG`

## 9. 错误处理策略

- 对外统一使用 `ERR\t<CODE>` 文本错误码
- 错误码列表：
  - `EINVAL`：参数无效
  - `IF_UP`/`IF_DOWN`：`ifconfig` 命令执行失败
  - `CTRL_OPEN`：wpa_ctrl 连接失败
  - `SCAN_START`/`SCAN_GET`：扫描命令失败
  - `ADD_NETWORK`/`SET_SSID`/`SET_PSK`：网络配置失败
  - `SELECT_NETWORK`：网络选择失败
  - `CONNECT_TIMEOUT`：连接超时（20秒）
  - `DISCONNECT`：断开连接失败
  - `NOT_FOUND`：要遗忘的网络不存在
  - `FORGET`：遗忘网络失败
  - `UNKNOWN_CMD`：未知命令
- socket 层异常（`accept` 等）仅打印 `perror` 并在严重场景退出主循环

## 10. 安全与可靠性评估（当前实现）

1. 鉴权缺失
- 本地任意进程若可访问 socket，即可执行 Wi-Fi 管理命令。

2. 输入转义不足
- `CONNECT` 直接将 SSID/密码拼接进 `SET_NETWORK ... "..."`。
- 若输入含引号/控制字符，可能导致命令构造异常。

3. 并发有限
- 多客户端可并发请求，但 `CONNECT`（最多 20 秒）期间会阻塞其他请求处理。

4. 状态判断较弱
- `wait_connected()` 使用子串匹配 SSID，存在误判可能（如同名前缀）。

5. 命令路径硬编码
- `wlan0`、`/var/run/wpa_supplicant/wlan0`、`ifconfig` 均写死，移植性受限。

6. 测试覆盖不足
- 现有单测仅验证 socket 常量；集成测试对真实 `wpa_supplicant` 依赖较强。

## 11. 可演进方向

1. 协议增强
- 增加 `REQ_ID` 与结构化返回（如 `key=value` 或 JSON）
- 统一错误码分层（参数错误/系统错误/下游错误）

2. 安全加固
- 启动时设置 socket 权限与属主（`chmod/chown`）
- 增加调用方鉴权（凭据、白名单 UID、辅助守护）
- SSID/密码做严格转义或改用二进制安全传参

3. 并发与时序
- 引入事件循环（`select/poll/epoll`）或 worker 模型
- 将长耗时命令改为异步任务 + 查询状态

4. 可配置化
- 将接口名、ctrl path、超时、socket path 改为启动参数或配置文件

5. 可观测性
- 增加日志等级、命令耗时、失败原因记录
- 预留 metrics 计数（命令成功率/超时率）

6. 测试体系
- 增加 `wpa_ctrl` mock，覆盖命令解析与异常路径
- 增加 CI 下可重复的伪 `wpa_supplicant` 集成测试

## 12. 当前实现结论

当前 `wifi-daemon` 是一个**轻量可用的 Wi-Fi IPC 封装骨架**，已具备基础控制闭环，但在安全性、并发性、可配置性与测试覆盖方面仍属于早期版本。若进入量产链路，建议优先完成安全加固与错误语义标准化。
