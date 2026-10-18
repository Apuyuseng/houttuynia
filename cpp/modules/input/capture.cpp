// 捕获辅助实现 ← crates/mwine-input/src/capture.rs
// Apple：CGWindowList / CGAssociateMouseAndMouseCursorPosition（C API 直译）
// 非 Apple：no-op（TODO）
#include "capture.hpp"

#include <QByteArray>

#ifdef __APPLE__

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

namespace mw::input::capture {

namespace {

constexpr uint32_t kCGWindowListOptionOnScreenOnly = 1 << 0;
constexpr uint32_t kCFStringEncodingUTF8 = 0x08000100;

bool nameContains(CFStringRef str, const char* needle)
{
    char buf[256];
    if (!CFStringGetCString(str, buf, sizeof(buf), kCFStringEncodingUTF8))
        return false;
    const QByteArray hay = QByteArray(buf);
    return hay.contains(needle); // 子串搜索（字节级足够：窗口名 ASCII 前缀 + 型号后缀）
}

struct WindowKeys {
    CFStringRef owner = nullptr;
    CFStringRef layer = nullptr;
    CFStringRef bounds = nullptr;
    ~WindowKeys()
    {
        if (owner)
            CFRelease(owner);
        if (layer)
            CFRelease(layer);
        if (bounds)
            CFRelease(bounds);
    }
    bool init()
    {
        owner = CFStringCreateWithCString(nullptr, "kCGWindowOwnerName", kCFStringEncodingUTF8);
        layer = CFStringCreateWithCString(nullptr, "kCGWindowLayer", kCFStringEncodingUTF8);
        bounds = CFStringCreateWithCString(nullptr, "kCGWindowBounds", kCFStringEncodingUTF8);
        return owner && layer && bounds;
    }
};

/// 窗口 layer == 0（普通窗口，排除菜单/Dock 等系统层）判定；无 layer 字段按 0 处理
bool isLayer0(CFDictionaryRef dict, CFStringRef layerKey)
{
    CFTypeRef layerRef = CFDictionaryGetValue(dict, layerKey);
    if (layerRef == nullptr)
        return true;
    long long layer = 0;
    // kCFNumberSInt64Type 取系统 CFNumber.h 枚举值（本地 int 常量无法隐式转 enum）
    if (CFNumberGetValue(static_cast<CFNumberRef>(layerRef), kCFNumberSInt64Type, &layer)
        && layer != 0)
        return false;
    return true;
}

std::optional<Rect> scanWindows(CFArrayRef list)
{
    WindowKeys keys;
    if (!keys.init())
        return std::nullopt;
    std::optional<Rect> found;
    double bestArea = 0.0;
    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; ++i) {
        CFDictionaryRef dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        if (dict == nullptr)
            continue;
        // owner 名包含 qemu-system（qemu-system-aarch64 / x86_64）
        CFTypeRef owner = CFDictionaryGetValue(dict, keys.owner);
        if (owner == nullptr || !nameContains(static_cast<CFStringRef>(owner), "qemu-system"))
            continue;
        // 只认 Layer 0（普通窗口，排除菜单/遮罩层）
        if (!isLayer0(dict, keys.layer))
            continue;
        CFTypeRef bounds = CFDictionaryGetValue(dict, keys.bounds);
        if (bounds == nullptr)
            continue;
        CGRect rect{};
        if (!CGRectMakeWithDictionaryRepresentation(static_cast<CFDictionaryRef>(bounds), &rect))
            continue;
        // qemu 会有侧栏/工具窗等小子窗——主窗口 = 面积最大者
        const double area = rect.size.width * rect.size.height;
        if (area > bestArea) {
            bestArea = area;
            found = Rect{ rect.origin.x, rect.origin.y, rect.size.width, rect.size.height };
        }
    }
    return found;
}

bool firstLayer0OwnerIsQemu(CFArrayRef list)
{
    WindowKeys keys;
    if (!keys.init())
        return false;
    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; ++i) {
        CFDictionaryRef dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        if (dict == nullptr)
            continue;
        if (!isLayer0(dict, keys.layer))
            continue; // 菜单/Dock 等系统层不算
        // 第一个 layer 0 窗口 = z 序最顶的普通窗口
        CFTypeRef owner = CFDictionaryGetValue(dict, keys.owner);
        return owner != nullptr && nameContains(static_cast<CFStringRef>(owner), "qemu-system");
    }
    return false;
}

} // namespace

void associateMouseCursor(bool connected)
{
    CGAssociateMouseAndMouseCursorPosition(connected);
}

std::optional<Rect> emulatorWindowFrame()
{
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (list == nullptr)
        return std::nullopt;
    const std::optional<Rect> result = scanWindows(list);
    CFRelease(list);
    return result;
}

bool emulatorFrontmost()
{
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (list == nullptr)
        return false;
    const bool front = firstLayer0OwnerIsQemu(list);
    CFRelease(list);
    return front;
}

} // namespace mw::input::capture

#else // !__APPLE__

// 非 Apple 平台：编译排除（TODO：接入对应平台的窗口/光标 API）
namespace mw::input::capture {

void associateMouseCursor(bool) {}

std::optional<Rect> emulatorWindowFrame()
{
    return std::nullopt;
}

bool emulatorFrontmost()
{
    return false;
}

} // namespace mw::input::capture

#endif
