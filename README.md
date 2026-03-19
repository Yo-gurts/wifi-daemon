# wifi-daemon

独立 Wi-Fi Daemon 仓库（示例骨架）。

- `src/wifi_daemon.c`: Unix Socket daemon
- `include/proto/wifi_proto.h`: 协议常量
- `tests/unit`: CMocka 单元测试
- `tests/integration`: Shell 集成测试

## 编译

使用 CMake 进行构建，支持本地编译和交叉编译。

### x86_64 本地编译

```bash
mkdir -p build/x86_64
cd build/x86_64
cmake -DCMAKE_TOOLCHAIN_FILE=../../toolchains/x86_64-linux-gnu.cmake ../..
make
```

### ARM 交叉编译

```bash
mkdir -p build/arm-none-linux-musleabihf
cd build/arm-none-linux-musleabihf
cmake -DCMAKE_TOOLCHAIN_FILE=../../toolchains/arm-none-linux-musleabihf.cmake \
      -DTOOLCHAIN_ROOT_DIR=/path/to/toolchain ../..
make
file bin/wifi-daemon  # 应显示 ARM 架构
```

### 支持的架构

- `x86_64-linux-gnu`
- `arm-none-linux-musleabihf`
- `aarch64-linux-gnu`
- `aarch64-buildroot-linux-gnu`
- `riscv64-unknown-linux-gnu`
- `riscv64-unknown-linux-musl`
- `arm-none-linux-gnueabihf`
- `arm-cvitek-linux-uclibcgnueabihf`
- `arm-linux-gnueabihf`

## 测试

```bash
# 集成测试
bash tests/integration/test_ipc.sh

# 单元测试（需要 cmocka）
# ARM: 使用交叉编译工具链
# x86_64: gcc -o /tmp/test_proto tests/unit/test_proto.c -lcmocka && /tmp/test_proto
```
