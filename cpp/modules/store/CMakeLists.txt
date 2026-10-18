# mw_store ← crates/mwine-store（豌豆荚 / F-Droid 在线应用商店）
file(GLOB MW_STORE_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_store STATIC ${MW_STORE_SRC})
target_include_directories(mw_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# mw_app：下载完成的包名核对走 app 模块的 ZipArchive/parseManifest（见 store.cpp 注释）
target_link_libraries(mw_store PUBLIC mw_common mw_app)
