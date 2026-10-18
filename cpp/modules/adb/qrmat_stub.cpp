// 非 macOS 弱实现：出码配对二维码矩阵不可用（返回空矩阵，调用方降级为
// 展示载荷文本；本项目产品目标平台为 macOS）。
#include "qrmat.hpp"

namespace mw::adb {

QVector<QString> qrMatrixRows(const QString&)
{
    return {};
}

bool qrMatrixSelfTest()
{
    return false;
}

} // namespace mw::adb
