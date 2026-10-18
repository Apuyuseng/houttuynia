# mw_input ← crates/mwine-input + mwine-keymap engine.rs（事件捕获与注入）
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_input STATIC ${SRC})
target_include_directories(mw_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ../keymap)
# EngineCore::emit 与 Qt 的 emit 宏冲突（qtmetamacros.h 默认 #define emit）；
# emit 关键字是纯语法糖（Q_EMIT 仍可用），全局禁用无副作用。
target_compile_definitions(mw_input PUBLIC QT_NO_EMIT)
target_link_libraries(mw_input PUBLIC mw_common mw_adb mw_keymap mw_scrcpy)
if(APPLE)
  find_library(COREFOUNDATION CoreFoundation REQUIRED)
  find_library(COREGRAPHICS CoreGraphics REQUIRED)
  find_library(APPLICATIONSERVICES ApplicationServices REQUIRED)
  find_library(APPKIT AppKit REQUIRED)
  find_library(CARBON Carbon REQUIRED)
  target_link_libraries(mw_input PUBLIC ${COREFOUNDATION} ${COREGRAPHICS} ${APPLICATIONSERVICES} ${APPKIT} ${CARBON})
endif()
