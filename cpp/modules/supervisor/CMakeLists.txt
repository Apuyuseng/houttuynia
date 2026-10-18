# mw_supervisor ← crates/mwine-supervisor（VM 生命周期 / 镜像导入拉取 / 快照）
file(GLOB MW_SUPERVISOR_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_supervisor STATIC ${MW_SUPERVISOR_SRC})
target_include_directories(mw_supervisor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${ZSTD_INCLUDE_DIR})
target_link_libraries(mw_supervisor PUBLIC mw_common mw_adb ${ZSTD_LIBRARY})
