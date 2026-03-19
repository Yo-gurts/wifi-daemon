# FindWpaCtrl.cmake
# CMake module to find wpa_ctrl library
#
# Usage:
#   find_package(WpaCtrl REQUIRED)
#
# Variables:
#   WPA_CTRL_FOUND - True if wpa_ctrl was found
#   WPA_CTRL_INCLUDE_DIR - Include directory for wpa_ctrl.h
#   WPA_CTRL_LIBRARY - Path to libwpa_client library
#   WPA_CTRL_LIBRARY_DIR - Directory containing the library

# Detect architecture
if(CMAKE_C_COMPILER_TARGET MATCHES "arm-none-linux-musleabihf")
    set(_WPA_CTRL_ARCH "arm-none-linux-musleabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "aarch64-linux-gnu")
    set(_WPA_CTRL_ARCH "aarch64-linux-gnu")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "aarch64-buildroot-linux-gnu")
    set(_WPA_CTRL_ARCH "aarch64-buildroot-linux-gnu")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "riscv64-unknown-linux-gnu")
    set(_WPA_CTRL_ARCH "riscv64-unknown-linux-gnu")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "riscv64-unknown-linux-musl")
    set(_WPA_CTRL_ARCH "riscv64-unknown-linux-musl")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "arm-none-linux-gnueabihf")
    set(_WPA_CTRL_ARCH "arm-none-linux-gnueabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "arm-cvitek-linux-uclibcgnueabihf")
    set(_WPA_CTRL_ARCH "arm-cvitek-linux-uclibcgnueabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "arm-linux-gnueabihf")
    set(_WPA_CTRL_ARCH "arm-linux-gnueabihf")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "x86_64-linux-gnu")
    set(_WPA_CTRL_ARCH "x86_64-linux-gnu")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(_WPA_CTRL_ARCH "x86_64")
else()
    set(_WPA_CTRL_ARCH "")
endif()

# Search for include directory
find_path(WPA_CTRL_INCLUDE_DIR wpa_ctrl.h
    PATHS
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/include
        /usr/local/include
        /usr/include
)

# Search for library
if(_WPA_CTRL_ARCH)
    find_library(WPA_CTRL_LIBRARY libwpa_client
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib/${_WPA_CTRL_ARCH}
            /usr/local/lib
            /usr/lib
    )
else()
    find_library(WPA_CTRL_LIBRARY libwpa_client
        PATHS
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wpa_ctrl/lib
            /usr/local/lib
            /usr/lib
    )
endif()

if(WPA_CTRL_LIBRARY)
    get_filename_component(WPA_CTRL_LIBRARY_DIR ${WPA_CTRL_LIBRARY} DIRECTORY)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WpaCtrl
    REQUIRED_VARS
        WPA_CTRL_INCLUDE_DIR
        WPA_CTRL_LIBRARY
)

mark_as_advanced(WPA_CTRL_INCLUDE_DIR WPA_CTRL_LIBRARY WPA_CTRL_LIBRARY_DIR)
