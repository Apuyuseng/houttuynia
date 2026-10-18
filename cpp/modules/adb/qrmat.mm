// macOS 实现：CoreImage CIQRCodeGenerator 生成 + 位图采样出矩阵；
// 自检用 CIDetector 解码回读（防方向/镜像回归）。
#include "qrmat.hpp"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <vector>

#include <QVector>

namespace mw::adb {

namespace {

// 行缓冲 RGB 位图（QR 输出纯黑白，取 R 通道即可判定模块明暗）
std::vector<uint8_t> renderToBitmap(CGImageRef cg, size_t& w, size_t& h)
{
    w = CGImageGetWidth(cg);
    h = CGImageGetHeight(cg);
    std::vector<uint8_t> buf(w * h * 4, 0);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx =
        CGBitmapContextCreate(buf.data(), w, h, 8, w * 4, cs, kCGImageAlphaNoneSkipLast);
    CGColorSpaceRelease(cs);
    if (!ctx)
        return {};
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cg);
    CGContextRelease(ctx);
    return buf;
}

} // namespace

QVector<QString> qrMatrixRows(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    NSData* data = [NSData dataWithBytes:utf8.constData() length:utf8.size()];
    CIFilter* filter = [CIFilter filterWithName:@"CIQRCodeGenerator"];
    if (filter == nil)
        return {};
    [filter setValue:data forKeyPath:@"inputMessage"];
    [filter setValue:@"M" forKeyPath:@"inputCorrectionLevel"];
    CIImage* image = filter.outputImage;
    if (image == nil)
        return {};
    const CGRect extent = [image extent];
    if (extent.size.width < 1 || extent.size.height < 1)
        return {};

    static CIContext* ciCtx = nil;
    if (ciCtx == nil)
        ciCtx = [CIContext contextWithOptions:nil]; // 软件渲染，无 GPU 依赖
    CGImageRef cg = [ciCtx createCGImage:image fromRect:extent];
    if (cg == nullptr)
        return {};

    size_t w = 0, h = 0;
    const auto buf = renderToBitmap(cg, w, h);
    CGImageRelease(cg);
    if (buf.empty())
        return {};

    // CoreImage 输出自带静区（实测 1 模块）：按黑模块包围盒裁出纯符号矩阵
    size_t x0 = w, x1 = 0, y0 = h, y1 = 0;
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            if (buf[(y * w + x) * 4] < 128) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < x0 || y1 < y0)
        return {};
    const size_t sw = x1 - x0 + 1, sh = y1 - y0 + 1;
    if (sw != sh)
        return {};

    QVector<QString> rows;
    rows.reserve(int(sh));
    for (size_t y = y0; y <= y1; ++y) {
        QString row;
        row.reserve(int(sw));
        for (size_t x = x0; x <= x1; ++x)
            row.append(QLatin1Char(buf[(y * w + x) * 4] < 128 ? '1' : '0'));
        rows.append(row);
    }
    return rows;
}

bool qrMatrixSelfTest()
{
    const QString payload =
        QStringLiteral("WIFI:T:ADB;S:studio-Ab3dE5fG7h;P:K2mN4pQ6rS;;");
    const QVector<QString> rows = qrMatrixRows(payload);
    if (rows.isEmpty())
        return false;
    const int n = rows.size();

    // 加 4 模块静区、放大 8 倍后交给 CIDetector 解码回读
    const int scale = 8, quiet = 4;
    const size_t dim = size_t(n + quiet * 2) * scale;
    std::vector<uint8_t> buf(dim * dim * 4, 255);
    for (int r = 0; r < n; ++r) {
        for (int c = 0; c < n; ++c) {
            if (rows.at(r).at(c) != QLatin1Char('1'))
                continue;
            for (int sy = 0; sy < scale; ++sy) {
                uint8_t* line = &buf[((size_t(quiet + r) * scale + sy) * dim
                                      + size_t(quiet + c) * scale) * 4];
                for (int sx = 0; sx < scale; ++sx) {
                    line[sx * 4] = 0;
                    line[sx * 4 + 1] = 0;
                    line[sx * 4 + 2] = 0;
                }
            }
        }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef bmp = CGBitmapContextCreate(
        buf.data(), dim, dim, 8, dim * 4, cs, kCGImageAlphaNoneSkipLast);
    CGColorSpaceRelease(cs);
    if (bmp == nullptr)
        return false;
    CGImageRef cg = CGBitmapContextCreateImage(bmp);
    CGContextRelease(bmp);
    if (cg == nullptr)
        return false;

    CIImage* image = [CIImage imageWithCGImage:cg];
    CIDetector* detector = [CIDetector detectorOfType:CIDetectorTypeQRCode
                                              context:nil
                                              options:nil];
    NSArray* features = detector == nil
        ? nil
        : [detector featuresInImage:image];
    CGImageRelease(cg);
    for (CIFeature* f in features) {
        if ([f isKindOfClass:[CIQRCodeFeature class]]) {
            CIQRCodeFeature* qr = static_cast<CIQRCodeFeature*>(f);
            return QString::fromNSString(qr.messageString) == payload;
        }
    }
    return false;
}

} // namespace mw::adb
