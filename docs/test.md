# 测试指南

## 硬件环境要求

- 设备已连接 Wi-Fi 网卡（wlan0）
- 设备已运行 wpa_supplicant（监听 `/var/run/wpa_supplicant/wlan0`）

## 测试步骤

### 1. 编译

```bash
# ARM 交叉编译
make CONFIG=ARM

# x86_64 本地编译
make CONFIG=X86
```

### 2. 推送程序到设备

```bash
adb.exe push build/arm-none-linux-musleabihf/wifi-daemon /mnt/data/
adb.exe push build/arm-none-linux-musleabihf/test_ipc /mnt/data/
```

### 3. 确保 wpa_supplicant 运行

```bash
adb.exe shell "wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf"
```

### 4. 启动 wifi-daemon

```bash
adb.exe shell "cd /mnt/data && killall wifi-daemon 2>/dev/null; rm -f /tmp/aicam_wifi.sock; ./wifi-daemon &"
```

### 5. 运行测试

```bash
adb.exe shell "cd /mnt/data && ./test_ipc ./wifi-daemon"
```

## 测试命令说明

test_ipc 会依次测试：

| 命令 | 功能 | 依赖 |
|------|------|------|
| SET_ENABLED 1 | 开启 Wi-Fi（ifconfig wlan0 up） | 设备有 wlan0 接口 |
| SCAN_START | 启动扫描 | wpa_supplicant 运行 |
| SCAN_GET | 获取扫描结果 | wpa_supplicant 运行 |
| DISCONNECT | 断开连接 | wpa_supplicant 运行 |
| SET_ENABLED 0 | 关闭 Wi-Fi（ifconfig wlan0 down） | 设备有 wlan0 接口 |

## 预期结果

```
=== Test 1: SET_ENABLED ===
<<< OK   STATE        # wlan0 开启成功

=== Test 2: SCAN_START ===
<<< OK   SCAN_STARTED # 扫描启动成功

=== Test 3: SCAN_GET ===
<<< OK   SCAN
AP    <ssid>  <signal>  <secured>  <saved>  <connected>
...
<<< END

=== Test 4: DISCONNECT ===
<<< OK   DISCONNECTED

=== Test 5: SET_ENABLED 0 ===
<<< OK   STATE        # wlan0 关闭成功
```

## 常见错误

| 错误 | 原因 | 解决方法 |
|------|------|----------|
| ERR IF_UP | wlan0 接口不存在或无权限 | 检查网卡配置 |
| ERR SCAN_START | wpa_supplicant 未运行 | 启动 wpa_supplicant |
| ERR SCAN_GET | wpa_supplicant 未运行 | 启动 wpa_supplicant |
| ERR DISCONNECT | wpa_supplicant 未运行 | 启动 wpa_supplicant |
