# mw_scrcpy ← crates/mwine-scrcpy（协议层 + VideoToolbox 解码；player 不移植）
# 对外头文件以 "scrcpy/xxx.hpp" 形式引用（暴露 modules/ 为包含目录，
# 避免与消费方 audio/input 模块内的同名文件 audio.hpp/scrcpy.hpp 冲突）
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_scrcpy STATIC ${SRC})
target_include_directories(mw_scrcpy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mw_scrcpy PUBLIC mw_common mw_adb)
if(APPLE)
  find_library(COREFOUNDATION CoreFoundation REQUIRED)
  find_library(COREVIDEO CoreVideo REQUIRED)
  find_library(VIDEOTOOLBOX VideoToolbox REQUIRED)
  find_library(COREMEDIA CoreMedia REQUIRED)
  target_link_libraries(mw_scrcpy PUBLIC ${COREFOUNDATION} ${COREVIDEO} ${VIDEOTOOLBOX} ${COREMEDIA})
endif()
