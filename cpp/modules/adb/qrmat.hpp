// 二维码矩阵生成：文本 → 逐行 '0'/'1' 字符串（'1' = 黑模块；不含静区，
// 绘制方自行留白 ≥4 模块）。
// macOS 用系统 CoreImage（CIQRCodeGenerator，纠错级别 M）——零第三方依赖、
// 编码质量有保证；其他平台弱实现返回空（调用方降级为只展示文本）。
// 矩阵由 daemon 统一生成（会话与二维码同源），GUI / CLI 只负责绘制。
#pragma once

#include <QString>
#include <QVector>

namespace mw::adb {

// 编码失败 / 平台不支持 → 空 QVector。
QVector<QString> qrMatrixRows(const QString& text);

// 自检（仅 macOS）：编码一段 ADB 配对载荷 → 加静区放大 → CIDetector 解码
// 回读一致。其他平台恒 false（测试侧跳过）。
bool qrMatrixSelfTest();

} // namespace mw::adb
