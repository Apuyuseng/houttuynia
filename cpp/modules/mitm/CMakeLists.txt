# mw_mitm ← crates/mwine-mitm（HTTPS 中间人 CA 与证书存储；OpenSSL）
# ca.*：rcgen → OpenSSL EVP/X509 等价实现；store.*：明文入库复用 mw_capture 的
# SQLite Store（对应 Rust 侧依赖 mwine-capture），故链接 mw_capture。
file(GLOB SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_mitm STATIC ${SRC})
target_include_directories(mw_mitm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(OpenSSL REQUIRED)
target_link_libraries(mw_mitm PUBLIC mw_common mw_capture OpenSSL::SSL OpenSSL::Crypto)
