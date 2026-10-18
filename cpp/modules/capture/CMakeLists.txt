# mw_capture ← crates/mwine-capture（eBPF 流量采集：sqlite 落库 + 查询）
file(GLOB MW_CAPTURE_SRC CONFIGURE_DEPENDS *.cpp *.hpp)
add_library(mw_capture STATIC ${MW_CAPTURE_SRC})
target_include_directories(mw_capture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# OpenSSL：Crypto=pcap TLS 解密（AES-GCM/ChaCha20-Poly1305/HKDF，见 pcap_decrypt.cpp）；
# SSL=capture_test 的真实 TLS1.3/1.2 握手 roundtrip 用例（随 PUBLIC 传导至测试目标）
find_package(OpenSSL REQUIRED)
# mw_adb：设备模式编排（root/推 ecapture/tail -f/包名富化，见 session.cpp）
target_link_libraries(mw_capture PUBLIC mw_common mw_adb ${SQLITE3_LIBRARY} OpenSSL::SSL OpenSSL::Crypto)
