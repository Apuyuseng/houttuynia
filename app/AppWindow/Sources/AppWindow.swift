import AppKit
import CoreGraphics

// MARK: - 渲染视图（接收帧数据 + 发送输入事件）

class AndroidView: NSView {
    var on_touch: ((Int, Int, Int) -> Void)? = nil  // (action, x, y)
    private var cg_image: CGImage?
    private var frame_count = 0
    private var last_fps_time = Date()

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fill(bounds)

        if let img = cg_image {
            // 等比缩放居中
            let img_aspect = CGFloat(img.width) / CGFloat(img.height)
            let view_aspect = bounds.width / bounds.height
            var draw_rect = bounds
            if img_aspect > view_aspect {
                let h = bounds.width / img_aspect
                draw_rect = NSRect(x: 0, y: (bounds.height - h) / 2, width: bounds.width, height: h)
            } else {
                let w = bounds.height * img_aspect
                draw_rect = NSRect(x: (bounds.width - w) / 2, y: 0, width: w, height: bounds.height)
            }
            ctx.interpolationQuality = .medium
            ctx.draw(img, in: draw_rect)
        }
    }

    func update_frame(_ rgba: [UInt8], width: Int, height: Int) {
        // RGBA → CGImage
        var bgra = rgba
        for i in stride(from: 0, to: bgra.count, by: 4) {
            bgra.swapAt(i, i + 2)
        }

        let color_space = CGColorSpaceCreateDeviceRGB()
        let bitmap_info = CGImageAlphaInfo.premultipliedLast.rawValue
        let provider = CGDataProvider(data: Data(bgra) as CFData)

        if let provider = provider,
           let img = CGImage(
               width: width, height: height,
               bitsPerComponent: 8, bitsPerPixel: 32,
               bytesPerRow: width * 4,
               space: color_space,
               bitmapInfo: CGBitmapInfo(rawValue: bitmap_info),
               provider: provider,
               decode: nil, shouldInterpolate: false,
               intent: .defaultIntent) {
            cg_image = img
            needsDisplay = true
            frame_count += 1
            let now = Date()
            if now.timeIntervalSince(last_fps_time) >= 1 {
                let fps = Double(frame_count) / now.timeIntervalSince(last_fps_time)
                window?.title = String(format: "%.0f fps", fps)
                frame_count = 0
                last_fps_time = now
            }
        }
    }

    // MARK: - 鼠标事件 → 触屏

    override func mouseDown(with event: NSEvent) {
        let pos = convert(event.locationInWindow, from: nil)
        let action = 0  // TOUCH_DOWN
        on_touch?(action, Int(pos.x), Int(bounds.height - pos.y))  // 翻转 Y
    }

    override func mouseDragged(with event: NSEvent) {
        let pos = convert(event.locationInWindow, from: nil)
        let action = 2  // TOUCH_MOVE
        on_touch?(action, Int(pos.x), Int(bounds.height - pos.y))
    }

    override func mouseUp(with event: NSEvent) {
        let pos = convert(event.locationInWindow, from: nil)
        let action = 1  // TOUCH_UP
        on_touch?(action, Int(pos.x), Int(bounds.height - pos.y))
    }

    // 坐标转换：视图坐标 → Android 坐标
    private func to_android_coords(_ x: Int, _ y: Int) -> (Int, Int) {
        guard let img = cg_image else { return (x, y) }
        let scale_x = CGFloat(img.width) / bounds.width
        let scale_y = CGFloat(img.height) / bounds.height
        return (Int(CGFloat(x) * scale_x), Int(CGFloat(y) * scale_y))
    }
}

// MARK: - 应用窗口

class AppWindowController: NSObject, NSWindowDelegate {
    var window: NSWindow?
    var view: AndroidView?
    var on_close: (() -> Void)? = nil

    func create_window(title: String, width: CGFloat, height: CGFloat) {
        let rect = NSRect(x: 200, y: 200, width: width, height: height)
        let style: NSWindow.StyleMask = [.titled, .closable, .miniaturizable, .resizable]

        window = NSWindow(contentRect: rect, styleMask: style, backing: .buffered, defer: false)
        window?.title = title
        window?.delegate = self
        window?.backgroundColor = .black
        window?.minSize = NSSize(width: 300, height: 400)
        window?.center()

        view = AndroidView(frame: NSRect(x: 0, y: 0, width: width, height: height))
        view?.autoresizingMask = [.width, .height]
        window?.contentView = view
        window?.makeKeyAndOrderFront(nil)

        // 让窗口出现在最前
        NSApp.activate(ignoringOtherApps: true)
    }

    func windowWillClose(_ notification: Notification) {
        on_close?()
    }
}

// MARK: - 主入口

let args = CommandLine.arguments
guard args.count >= 3 else {
    print("用法: AppWindow <package> <grpc_port>")
    exit(1)
}
let package = args[1]
let grpc_port = Int16(args[2]) ?? 8555
let app_name = args.count > 3 ? args[3] : package

let app = NSApplication.shared
app.setActivationPolicy(.accessory)  // 不显示 Dock 图标

let controller = AppWindowController()
controller.create_window(title: app_name, width: 405, height: 720)  // 手机比例

// 截图轮询（调用 mwine-core REST API 获取帧数据）
var timer: Timer?
timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { _ in
    // 从 mwine-core REST API 获取截图
    let url = URL(string: "http://127.0.0.1:7788/api/screenshot?display=0")!
    let task = URLSession.shared.dataTask(with: url) { data, _, _ in
        guard let d = data else { return }
        if let json = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
           let b64 = json["data"] as? String,
           let w = json["width"] as? Int,
           let h = json["height"] as? Int,
           let raw = Data(base64Encoded: b64) {
            DispatchQueue.main.async {
                controller.view?.update_frame([UInt8](raw), width: w, height: h)
            }
        }
    }
    task.resume()
}

// 触屏注入（通过 mwine-core REST API）
controller.view?.on_touch = { action, x, y in
    let url = URL(string: "http://127.0.0.1:7788/api/touch?action=\(action)&x=\(x)&y=\(y)")!
    var req = URLRequest(url: url)
    req.httpMethod = "POST"
    URLSession.shared.dataTask(with: req).resume()
}

// 关闭窗口 = 退出
controller.on_close = {
    timer?.invalidate()
    NSApp.terminate(nil)
}

app.run()
