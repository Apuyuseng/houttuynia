# 公共基础库：所有业务模块依赖
file(GLOB_RECURSE MW_COMMON_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_common STATIC ${MW_COMMON_SRC})
target_include_directories(mw_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mw_common PUBLIC Qt6::Core Qt6::Network ${ZLIB_LIBRARY})
