cmake_minimum_required(VERSION 3.10)
project(wifi-daemon C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -ffunction-sections -fdata-sections")

# Detect architecture from toolchain or system
if(CMAKE_C_COMPILER_TARGET MATCHES "arm-none-linux-musleabihf")
    set(TARGET_ARCH "arm-none-linux-musleabihf")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/arm-none-linux-musleabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "aarch64-linux-gnu")
    set(TARGET_ARCH "aarch64-linux-gnu")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/aarch64-linux-gnu")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "aarch64-buildroot-linux-gnu")
    set(TARGET_ARCH "aarch64-buildroot-linux-gnu")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/aarch64-buildroot-linux-gnu")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "riscv64-unknown-linux-gnu")
    set(TARGET_ARCH "riscv64-unknown-linux-gnu")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/riscv64-unknown-linux-gnu")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "riscv64-unknown-linux-musl")
    set(TARGET_ARCH "riscv64-unknown-linux-musl")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/riscv64-unknown-linux-musl")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "arm-none-linux-gnueabihf")
    set(TARGET_ARCH "arm-none-linux-gnueabihf")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/arm-none-linux-gnueabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "arm-cvitek-linux-uclibcgnueabihf")
    set(TARGET_ARCH "arm-cvitek-linux-uclibcgnueabihf")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/arm-cvitek-linux-uclibcgnueabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "arm-linux-gnueabihf")
    set(TARGET_ARCH "arm-linux-gnueabihf")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/arm-linux-gnueabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "x86_64-linux-gnu")
    set(TARGET_ARCH "x86_64-linux-gnu")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/x86_64-linux-gnu")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(TARGET_ARCH "x86_64")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/x86_64")
else()
    message(WARNING "Unknown architecture: CMAKE_C_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET}, CMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}")
    set(TARGET_ARCH "unknown")
    set(WPA_CTRL_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib")
endif()

message(STATUS "Target architecture: ${TARGET_ARCH}")
message(STATUS "WPA_CTRL lib dir: ${WPA_CTRL_LIB_DIR}")

# Include directories
set(INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/include
)

# Source files
set(SOURCES
    src/wifi_daemon.c
    src/wpa_ctrl_compat.c
)

# Create executable
add_executable(wifi-daemon ${SOURCES})

# Include directories
target_include_directories(wifi-daemon PRIVATE ${INCLUDE_DIRS})

# Note: wpa_ctrl functions are provided by wpa_ctrl_compat.c
# third_party/wpa_ctrl/lib contains pre-built wpa_ctrl libraries for reference
target_link_libraries(wifi-daemon PRIVATE m pthread)

# Installation
install(TARGETS wifi-daemon DESTINATION bin)

# Test IPC client
add_executable(test_ipc ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_ipc.c)
target_include_directories(test_ipc PRIVATE ${INCLUDE_DIRS})
target_link_libraries(test_ipc PRIVATE m pthread)
install(TARGETS test_ipc DESTINATION bin)
