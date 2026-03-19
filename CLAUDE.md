# CLAUDE.md

本文档为 Claude Code (claude.ai/code) 在本项目中工作时提供指导。

## 项目概述

wifi-daemon 是一个独立的 Wi-Fi 管理 Daemon，通过 Unix Socket 与外部通信，控制 wpa_supplicant。

## 编译命令

```bash
# ARM musl 交叉编译（默认）
mkdir -p build/arm-none-linux-musleabihf && cd build/arm-none-linux-musleabihf
cmake -DCMAKE_TOOLCHAIN_FILE=../../toolchains/arm-none-linux-musleabihf.cmake \
      -DTOOLCHAIN_ROOT_DIR=/path/to/toolchain ../..
make
file bin/wifi-daemon  # 应显示 ARM 架构
```

## IPC 接口

Unix Socket 路径：`/tmp/aicam_wifi.sock`

| 命令 | 参数 | 说明 | 返回格式 |
|------|------|------|----------|
| `GET_STATUS` | 无 | 获取 WiFi 开关状态 | `OK\tSTATUS\t<enabled>` |
| `SET_ENABLED` | `0`/`1` | 禁用/启用 WiFi | `OK\tSTATE` |
| `SCAN_START` | 无 | 开始扫描 | `OK\tSCAN_STARTED\t<scan_id>` |
| `SCAN_GET` | 无 | 获取扫描结果 | `OK\tSCAN\t<scan_id>` + AP 列表 + `END` |
| `CONNECT` | `SSID\t密码` | 连接 WiFi | `OK\tCONNECTED` |
| `DISCONNECT` | 无 | 断开连接 | `OK\tDISCONNECTED` |
| `FORGET` | `SSID` | 删除已保存网络 | `OK\tFORGOT` |

错误返回格式：`ERR\t<CODE>`

## 目录结构

```
wifi-daemon/
├── CMakeLists.txt              # CMake 构建文件
├── cmake/
│   └── FindWpaCtrl.cmake       # CMake 查找模块
├── include/
│   ├── proto/
│   │   └── wifi_proto.h         # 协议常量
│   └── third_party/
│       └── wpa_ctrl.h           # wpa_ctrl 头文件包装器
├── src/
│   ├── wifi_daemon.c           # 主程序
│   └── wpa_ctrl_compat.c       # wpa_ctrl 兼容实现
├── third_party/
│   └── wpa_ctrl/
│       ├── include/
│       │   └── wpa_ctrl.h      # wpa_ctrl 原生头文件
│       └── lib/
│           └── arm-none-linux-musleabihf/  # 架构专用库
├── toolchains/                  # CMake 工具链文件
├── tests/
│   ├── unit/                   # 单元测试
│   └── integration/            # 集成测试
└── README.md
```

## 代码规范

- **格式**：遵循 Linux 内核风格，使用 GNU11 标准
- **头文件**：使用 `#ifndef` / `#define` / `#endif` 保护
- **日志**：使用中文注释

## Git 提交信息

### 格式结构

```
type(scope): 简要描述

- 详细变更点1
- 详细变更点2
- 详细变更点3
```

### 说明

- **第一行**：`type(scope): description`
  - `type`：类型前缀，如 `feat`（新功能）、`fix`（bug 修复）、`refactor`（重构）、`chore`（构建/工具）等
  - `scope`：影响范围（可选），如 `build`、`test`、`wifi_daemon` 等
  - `description`：简要描述，用中文，不超过 50 字

- **正文**：详细变更列表
  - 以 `-` 开头，每行一个变更点
  - 描述具体做了什么（不要描述"如何做"）
  - 按逻辑顺序排列

### 示例

```
feat(wifi_daemon): 新增 Wi-Fi 扫描功能

- 实现 SCAN_START 命令，启动 wpa_supplicant 扫描
- 实现 SCAN_GET 命令，返回可用 AP 列表
- 解析 SCAN_RESULTS 输出，提取 SSID、信号强度、安全类型
```

### 注意事项

- 使用中文提交信息
- 保持提交内容紧凑易读
- 提交正文必须使用真实换行，不要把 `\n` 当普通字符写入正文
- 推荐命令：`git commit -m "fix(module): 简要描述" -m $'- 变更点1\n- 变更点2'`
- 错误示例：`git commit -m "fix(module): 简要描述" -m "- 变更点1\n- 变更点2"`（会把 `\n` 原样写进提交信息）
