# mw_qemu ← crates/mwine-qemu（qemu/emulator 进程管理与 console）
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_qemu STATIC ${SRC})
target_include_directories(mw_qemu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mw_qemu PUBLIC mw_common mw_adb)
