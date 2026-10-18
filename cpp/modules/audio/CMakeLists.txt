# mw_audio ← crates/mwine-audio（CoreAudio 路由 + scrcpy 音频）
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_audio STATIC ${SRC})
target_include_directories(mw_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# AudioStream::emit 与 Qt 的 emit 宏冲突（qtmetamacros.h 默认 #define emit）；
# emit 关键字是纯语法糖（Q_EMIT 仍可用），全局禁用无副作用。
target_compile_definitions(mw_audio PUBLIC QT_NO_EMIT)
target_link_libraries(mw_audio PUBLIC mw_common mw_adb mw_scrcpy)
if(APPLE)
  find_library(COREAUDIO CoreAudio REQUIRED)
  find_library(AUDIOUNIT AudioUnit REQUIRED)
  find_library(COREFOUNDATION CoreFoundation REQUIRED)
  target_link_libraries(mw_audio PUBLIC ${COREAUDIO} ${AUDIOUNIT} ${COREFOUNDATION})
endif()
