# mwine-cored ← crates/mwine-core（REST 守护进程：路由 + 终端 + 商店 API）
file(GLOB MW_DAEMON_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
list(REMOVE_ITEM MW_DAEMON_SRC ${CMAKE_CURRENT_SOURCE_DIR}/iconfetch.cpp) # 子进程工具，不进守护
add_executable(mwine-cored ${MW_DAEMON_SRC})
target_include_directories(mwine-cored PRIVATE
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/modules/adb
    ${CMAKE_SOURCE_DIR}/modules/config
    ${CMAKE_SOURCE_DIR}/modules/keymap
    ${CMAKE_SOURCE_DIR}/modules/app
    ${CMAKE_SOURCE_DIR}/modules/store
    ${CMAKE_SOURCE_DIR}/modules/supervisor
    ${CMAKE_SOURCE_DIR}/modules/capture)
target_link_libraries(mwine-cored PRIVATE
    mw_common mw_adb mw_config mw_keymap mw_app mw_store mw_supervisor mw_capture)

# 图标抓取子进程：与守护隔离（解析器在守护并发环境下有堆损坏史，
# 隔离后最坏情况是单个图标缺失，守护本体不受影响）
add_executable(mwine-iconfetch iconfetch.cpp)
target_include_directories(mwine-iconfetch PRIVATE
    ${CMAKE_SOURCE_DIR}/common ${CMAKE_SOURCE_DIR}/modules/adb ${CMAKE_SOURCE_DIR}/modules/app)
find_package(Qt6 REQUIRED COMPONENTS Gui)
target_link_libraries(mwine-iconfetch PRIVATE mw_common mw_adb mw_app Qt6::Gui)
