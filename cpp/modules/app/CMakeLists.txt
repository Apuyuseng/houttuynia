# mw_app ← crates/mwine-app（APK 解析 / 安装 / 桌面壳生成）
# 默认 ADB 通道走 mw_adb 协议栈（protocol.hpp）
file(GLOB MW_APP_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_app STATIC ${MW_APP_SRC})
target_include_directories(mw_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ../adb)
target_link_libraries(mw_app PUBLIC mw_common mw_adb)
