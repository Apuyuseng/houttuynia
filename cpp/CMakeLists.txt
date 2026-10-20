# mwine C++ 化工作区：按业务模块静态库 + 最终守护进程（mwine-cored）
# 依赖：Qt6（Core/Network，UI 已依赖）+ zstd（镜像导入）+ sqlite3（流量落库）
cmake_minimum_required(VERSION 3.19)
project(mwine-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

find_path(ZSTD_INCLUDE_DIR zstd.h)
find_library(ZSTD_LIBRARY NAMES zstd)
find_library(SQLITE3_LIBRARY NAMES sqlite3)
find_library(ZLIB_LIBRARY NAMES z)

add_subdirectory(common)

# 业务模块（目录存在即纳入；每模块自带 CMakeLists，源码 GLOB 自动收录）
# MWINE_HEADLESS=ON（NAS/服务器无头构建）只保留守护进程依赖的模块：
# 桌面专属模块（独立窗口/音频/输入捕获/CLI/游戏键位引擎）不参与编译。
option(MWINE_HEADLESS "仅构建守护进程所需模块（NAS/无头服务器）" OFF)
if(MWINE_HEADLESS)
    set(MWINE_MODULES adb config keymap app store supervisor capture)
else()
    set(MWINE_MODULES adb config keymap app store supervisor capture qemu mitm scrcpy audio input cli window)
endif()
foreach(m ${MWINE_MODULES})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/${m}/CMakeLists.txt")
        add_subdirectory(modules/${m})
    endif()
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/daemon/CMakeLists.txt")
    add_subdirectory(daemon)
endif()

# 模块自测（模块内每个 *_test.cpp 各注册一个测试目标）
enable_testing()
foreach(m ${MWINE_MODULES} daemon)
    file(GLOB MWINE_MODULE_TESTS "${CMAKE_SOURCE_DIR}/modules/${m}/*_test.cpp")
    foreach(t ${MWINE_MODULE_TESTS})
        get_filename_component(tname ${t} NAME_WE)
        add_executable(mw_${m}_${tname} ${t})
        target_link_libraries(mw_${m}_${tname} PRIVATE mw_${m})
        add_test(NAME ${m}_${tname} COMMAND mw_${m}_${tname})
    endforeach()
    # 兼容既有命名：无后缀的主测试目标（adb_test/devices_test 等直接以带后缀名注册）
endforeach()
