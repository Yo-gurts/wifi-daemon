# wifi-daemon

独立 Wi-Fi Daemon 仓库（示例骨架）。

- `src/wifi_daemon.c`: Unix Socket daemon
- `include/proto/wifi_proto.h`: 协议常量
- `tests/unit`: CMocka 单元测试
- `tests/integration`: Shell 集成测试

## Build

```bash
make
```

## Test

```bash
make integration-test
# 如果环境有 cmocka
make unit-test
```
