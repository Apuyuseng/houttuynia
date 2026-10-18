# mw_config ← crates/mwine-config（VmConfig 持久化）
file(GLOB MW_CONFIG_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_config STATIC ${MW_CONFIG_SRC})
target_include_directories(mw_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mw_config PUBLIC mw_common)
