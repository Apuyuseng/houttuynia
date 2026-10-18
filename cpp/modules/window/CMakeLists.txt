# mwine-window：独立应用窗口（Qt 播放器）。
# scrcpy 会话（mw_scrcpy）→ VideoToolbox 解码 → QOpenGLWidget NV12 渲染；
# 输入经 scrcpy 控制通道注入；音频经 mw_audio（flac→ffmpeg→CoreAudio）。
# VideoToolbox 仅 macOS 可用；其他平台跳过本目标（等待跨平台解码器）。
#
# ===== 跨平台移植清单（解开下面的 APPLE 门卫前逐项补齐）=====
#   1. 视频解码：videoLoop 的 VideoToolboxDecoder → 平台硬解或 FFmpeg 软解
#      （交付协议 Nv12Image 不变；decoder.hpp 在 mw_scrcpy）
#   2. 音频输出：sessionUp 的 CoreAudioBridge → WASAPI/PulseAudio 等
#   3. 窗口外观：chrome.hpp 已三平台就位（mac/win/stub），按需补实现
#   4. Qt 层（渲染/输入/底部功能条/主题跟随/几何记忆）无平台分支，直接可用
if(NOT APPLE)
    message(STATUS "window 模块依赖 VideoToolbox（仅 macOS），跳过")
    return()
endif()

find_package(Qt6 REQUIRED COMPONENTS Gui Widgets OpenGL OpenGLWidgets)

file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.hpp)
# 窗口外观按平台挑实现（chrome.hpp 接口跨平台；调用方无平台分支）。
# GLOB 会收进全部 chrome_*.cpp——先剔除再按平台挑一个，避免符号重复定义
list(FILTER SRC EXCLUDE REGEX "chrome_")
set(CHROME_SRC chrome_mac.mm)
if(WIN32)
    set(CHROME_SRC chrome_win.cpp)
elseif(NOT APPLE)
    set(CHROME_SRC chrome_stub.cpp)
endif()
enable_language(OBJCXX)
add_executable(mwine-window ${SRC} ${CHROME_SRC})
if(APPLE)
    set_source_files_properties(chrome_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()
foreach(m scrcpy adb audio input)
    target_include_directories(mwine-window PRIVATE ${CMAKE_SOURCE_DIR}/modules/${m})
endforeach()
target_include_directories(mwine-window PRIVATE ${CMAKE_SOURCE_DIR}/common ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mwine-window PRIVATE
    mw_common mw_adb mw_scrcpy mw_audio mw_input Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Widgets)
