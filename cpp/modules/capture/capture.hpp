// mw::capture ← crates/mwine-capture（eBPF 明文旁挂流量采集）
//
// 分层（按源文件对应）：
//   events.hpp      ：ecapture 文本事件流增量解析（长度分帧）；
//   hpack.hpp       ：HPACK 解码器（H2 头部，动态表跨块保持）；
//   flow.hpp        ：连接/事件重组为 HTTP flow（H1 多消息、H2 多流、gzip/deflate）；
//   store.hpp       ：SQLite 持久化与筛选查询（双表拆分 + 懒解析详情）；
//   session.hpp     ：采集会话编排——本机子进程模式（start）与设备模式
//                     （startOnDevice：ADB root/推 ecapture/tail -f/包名富化/
//                     WebView 旁观）+ webview-export；
//   pcap_decrypt.hpp：pcap + SSLKEYLOGFILE 宿主侧 TLS 解密（AES-GCM/ChaCha20、
//                     TLS1.3/1.2、TCP 重组）——WebView/Cronet 密文旁路。
//
// daemon 集成方只 include 本文件：
//   mw::capture::Session session;
//   session.start(serial, { .packages = {...} });          // 本机子进程模式
//   session.startOnDevice(serial, { .packages = {...} });  // 设备模式（ADB 编排）
//   auto flows = session.queryFlows({ .limit = 200 });
//   auto dir = session.exportWebviewBundle();              // WebView 旁观包导出
//
// 与 Rust 版的残留差异（设备行为需真机/模拟器环境，单测覆盖纯逻辑面）：
//   - Rust 的 pcap_decrypt 自测用 rustls 真实握手构造密文；C++ 侧等价换用
//     OpenSSL（memory-BIO 握手 + keylog 回调），管线一致。
#pragma once

#include "events.hpp"
#include "flow.hpp"
#include "hpack.hpp"
#include "pcap_decrypt.hpp"
#include "session.hpp"
#include "store.hpp"

// 契约类型位置：
//   mw::capture::TargetSpec / mw::capture::Session     → session.hpp
//   mw::capture::FlowsQuery / mw::capture::Store       → store.hpp
//   mw::capture::Flow / FlowSummary / Reassembler /
//   EventParser / ExchangeView / …                      → flow.hpp / events.hpp
//   mw::capture::KeyLog / PcapPacket / TcpSeg / TcpStreamBuf /
//   TcpConn / DecryptedStream / decryptPcap / …         → pcap_decrypt.hpp
