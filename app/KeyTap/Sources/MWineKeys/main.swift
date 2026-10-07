// MWineKeys：键盘→屏幕映射注入器
// 用法: MWineKeys <keymap.json> [adb路径]
// keymap.json: [{"key":"w","type":"tap","x":800,"y":1200,"interval":0},
//               {"key":"s","type":"swipe","x":800,"y":1500,"x2":800,"y2":900,"duration":300}]
// 需要「辅助功能」权限（AXIsProcessTrusted）。

import Foundation
import CoreGraphics
import AppKit

struct Binding: Codable {
    let key: String          // 小写字符或 "space"/"up"/"shift" 等
    let type: String         // tap | swipe | hold
    let x: Double
    let y: Double
    var x2: Double?
    var y2: Double?
    var duration: Int?       // swipe 毫秒
    var interval: Int?       // hold 重复间隔毫秒
}

var bindings: [String: Binding] = [:]
var adbPath = "adb"
var shellStdin: Pipe?
var shellProcess: Process?
var heldKeys = Set<String>()          // 物理按住的绑定键
var holdTimers: [String: Timer] = [:]

func log(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

// ---------- adb 持久 shell（低延迟 input 注入） ----------
func startShell() {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: adbPath)
    p.arguments = ["-s", "emulator-5554", "shell"]
    let inPipe = Pipe()
    let outPipe = Pipe()
    p.standardInput = inPipe
    p.standardOutput = outPipe
    p.standardError = outPipe
    do {
        try p.run()
        shellProcess = p
        shellStdin = inPipe
        log("[keys] adb shell 就绪")
    } catch {
        log("[keys] adb shell 启动失败: \(error)")
        exit(2)
    }
}

func inject(_ cmd: String) {
    guard let pipe = shellStdin else { return }
    pipe.fileHandleForWriting.write((cmd + "\n").data(using: .utf8)!)
}

func performTap(_ b: Binding) {
    inject(String(format: "input tap %.0f %.0f", b.x, b.y))
}

func performSwipe(_ b: Binding) {
    let dur = b.duration ?? 300
    inject(String(format: "input swipe %.0f %.0f %.0f %.0f %d",
                  b.x, b.y, b.x2 ?? b.x, b.y2 ?? b.y, dur))
}

// ---------- 键名归一 ----------
func keyName(from event: CGEvent) -> String? {
    let flags = event.flags
    let keyCode = Int(event.getIntegerValueField(.keyboardEventKeycode))
    // 方向/功能键表
    // ↑ 字典字面量键冲突风险，z 用 16
    var map: [Int: String] = [
        36: "return", 48: "tab", 49: "space", 51: "delete", 53: "escape",
        122: "f1", 120: "f2", 99: "f3", 118: "f4", 96: "f5", 97: "f6",
        98: "f7", 100: "f8", 101: "f9", 109: "f10", 103: "f11", 111: "f12",
        123: "left", 124: "right", 125: "down", 126: "up",
        0: "a", 11: "b", 8: "c", 2: "d", 14: "e", 3: "f", 5: "g", 4: "h",
        38: "j", 40: "k", 37: "l", 46: "m", 45: "n", 31: "o", 35: "p",
        12: "q", 13: "r", 15: "s", 17: "t", 32: "u", 34: "v", 9: "w",
        33: "x", 30: "y", 16: "z",
        18: "1", 19: "2", 20: "3", 21: "4", 22: "5", 23: "6", 25: "7", 26: "8", 28: "9", 29: "0",
    ]
    if flags.contains(.maskShift) { /* shift 组合暂不映射 */ }
    return map[keyCode] ?? (keyCode >= 18 && keyCode <= 29 ? "\(keyCode)" : nil)
}

// ---------- hold（按住连点） ----------
func startHold(_ name: String, _ b: Binding) {
    guard !heldKeys.contains(name) else { return }
    heldKeys.insert(name)
    performTap(b)
    let interval = max(40, b.interval ?? 120)
    let t = Timer(timeInterval: Double(interval) / 1000.0, repeats: true) { _ in
        if heldKeys.contains(name) {
            performTap(b)
        }
    }
    RunLoop.main.add(t, forMode: .common)
    holdTimers[name] = t
}

func stopHold(_ name: String) {
    heldKeys.remove(name)
    if let t = holdTimers.removeValue(forKey: name) {
        t.invalidate()
    }
}

// ---------- 事件回调 ----------
let tapMask = (1 << CGEventType.keyDown.rawValue)
            | (1 << CGEventType.keyUp.rawValue)
            | (1 << CGEventType.flagsChanged.rawValue)

var gTap: CFMachPort?
var sawEvent = false

func eventCallback(proxy: CGEventTapProxy, type: CGEventType, event: CGEvent, refcon: UnsafeMutableRawPointer?) -> Unmanaged<CGEvent>? {
    switch type {
    case .tapDisabledByTimeout, .tapDisabledByUserInput:
        log("[tap] disabled (\(type.rawValue))，重启监听")
        if let t = gTap { CGEvent.tapEnable(tap: t, enable: true) }
        return Unmanaged.passRetained(event)
    case .keyDown:
        sawEvent = true
        let n = keyName(from: event) ?? "?"
        guard let name = keyName(from: event), let b = bindings[name] else {
            return Unmanaged.passRetained(event)
        }
        switch b.type {
        case "swipe": performSwipe(b)
        default: startHold(name, b)   // tap 也走 hold 状态机（单次）
        }
        return nil                     // 吞掉原键
    case .keyUp:
        guard let name = keyName(from: event), bindings[name] != nil else {
            return Unmanaged.passRetained(event)
        }
        stopHold(name)
        return nil
    default:
        return Unmanaged.passRetained(event)
    }
}

// ---------- main ----------
let args = CommandLine.arguments
guard args.count >= 2 else {
    print("用法: MWineKeys <keymap.json> [adb路径]")
    exit(1)
}
let keymapPath = args[1]
if args.count >= 3 { adbPath = args[2] }

// 辅助功能权限
let opts = ["AXTrustedCheckOptionPrompt": kCFBooleanTrue] as CFDictionary
if !AXIsProcessTrustedWithOptions(opts) {
    log("[keys] 需要辅助功能权限（系统设置 → 隐私与安全性 → 辅助功能）")
    // 已弹引导窗，继续等授权
    var waited = 0
    while !AXIsProcessTrusted() && waited < 120 {
        Thread.sleep(forTimeInterval: 1)
        waited += 1
    }
    guard AXIsProcessTrusted() else { exit(3) }
}

do {
    let data = FileManager.default.contents(atPath: keymapPath) ?? Data("[]".utf8)
    let list = try JSONDecoder().decode([Binding].self, from: data)
    bindings = Dictionary(uniqueKeysWithValues: list.map { ($0.key.lowercased(), $0) })
    log("[keys] 已加载 \(bindings.count) 个绑定")
    guard !bindings.isEmpty else { exit(0) }
} catch {
    log("[keys] keymap 解析失败: \(error)")
    exit(1)
}

startShell()

guard let tap = CGEvent.tapCreate(
    tap: .cgSessionEventTap,
    place: .headInsertEventTap,
    options: .defaultTap,
    eventsOfInterest: CGEventMask(tapMask),
    callback: eventCallback,
    userInfo: nil
) else {
    log("[keys] 事件监听创建失败（辅助功能权限？）")
    exit(3)
}
gTap = tap

let src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0)
CFRunLoopAddSource(CFRunLoopGetMain(), src, .commonModes)
CGEvent.tapEnable(tap: tap, enable: true)
log("[keys] 注入器运行中（Ctrl-C 退出）")
DispatchQueue.global().asyncAfter(deadline: .now() + 0.5) {
    let src = CGEventSource(stateID: .combinedSessionState)
    CGEvent(keyboardEventSource: src, virtualKey: 13, keyDown: true)?.post(tap: .cghidEventTap)
    Thread.sleep(forTimeInterval: 0.15)
    CGEvent(keyboardEventSource: src, virtualKey: 13, keyDown: false)?.post(tap: .cghidEventTap)
    log("[tap] 自检事件已发")
}
// macOS 新版：键盘监听需要「输入监控」权限（独立于辅助功能）。
// 症状：tap 创建成功但收不到事件。自检失败 → 引导授权。
DispatchQueue.global().asyncAfter(deadline: .now() + 3.0) {
    if !sawEvent {
        log("[keys] ✗ 收不到键盘事件：需要「输入监控」权限")
        log("[keys] 打开 系统设置 → 隐私与安全性 → 输入监控，勾选 MWineKeys/所在终端")
        let _ = try? NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Library/PreferencePanes/Security.prefPane"))
        exit(4)
    }
    log("[keys] ✓ 键盘监听正常")
}
CFRunLoopRun()
