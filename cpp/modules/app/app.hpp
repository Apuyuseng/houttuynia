// mwine-app：Android 应用管理（APK 解析 / 安装 / 桌面 .app 壳）。
// crates/mwine-app 的 C++ 移植 + mwine-cli 的桌面壳生成面（shellgen）。
//
// 职责：APK 安装/卸载/启动/列表/图标与元数据提取。APK 本地解析（包名/版本/
// 标签/图标）为纯 C++ 实现（zip + 二进制 AndroidManifest），**不依赖 Android SDK**；
// aapt2 存在时作为可选增强路径（标签/自适应图标更准）。
//
// 已知边界（v1）：android:label/icon 为资源引用（@string/@mipmap）时，解析需要
// resources.arsc 完整解析，v1 未实现——此时 label 置空 / icon 缺席（None 降级语义），
// 由 UI 用包名/默认图标兜底。
//
// 注：crypto_scan 属 mwine-core 转换组，不在本模块。
#pragma once

#include "apk.hpp"
#include "badging.hpp"
#include "dumpsys.hpp"
#include "manager.hpp"
#include "manifest.hpp"
#include "mock.hpp"
#include "shellgen.hpp"
#include "types.hpp"
