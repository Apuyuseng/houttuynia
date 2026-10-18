# mw_adb ← crates/mwine-adb（纯 Rust ADB smart-socket 客户端）
file(GLOB MW_ADB_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
# 二维码矩阵：macOS 用系统 CoreImage（ObjC++），其他平台弱实现
#（qrmat_stub.cpp 与 qrmat.mm 二选一，避免符号重复）
if(APPLE)
    list(REMOVE_ITEM MW_ADB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/qrmat_stub.cpp")
    enable_language(OBJCXX)
    list(APPEND MW_ADB_SRC qrmat.mm)
    set_source_files_properties(qrmat.mm PROPERTIES LANGUAGE OBJCXX)
endif()
add_library(mw_adb STATIC ${MW_ADB_SRC})
target_include_directories(mw_adb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mw_adb PUBLIC mw_common)
if(APPLE)
    target_link_libraries(mw_adb PUBLIC "-framework CoreImage"
                                        "-framework CoreGraphics"
                                        "-framework Foundation")
endif()
