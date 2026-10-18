# mwine-cli 的 C++ 版（mwine 命令；业务能力全部来自 mw_* 模块）
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_executable(mwine-cpp ${SRC})
# 产物名必须是 mwine：Qt 前端 / daemon / notify 桥都按这个名字查找
set_target_properties(mwine-cpp PROPERTIES OUTPUT_NAME mwine)
foreach(m adb config keymap app store supervisor capture qemu input scrcpy)
    target_include_directories(mwine-cpp PRIVATE ${CMAKE_SOURCE_DIR}/modules/${m})
endforeach()
# 模块间同名头（client.hpp/console.hpp）用带模块前缀的路径消歧
target_include_directories(mwine-cpp PRIVATE
    ${CMAKE_SOURCE_DIR}/common ${CMAKE_SOURCE_DIR}/daemon ${CMAKE_SOURCE_DIR}/modules)
target_link_libraries(mwine-cpp PRIVATE
    mw_common mw_adb mw_config mw_keymap mw_app mw_store mw_supervisor mw_capture mw_qemu mw_input
    mw_scrcpy)
