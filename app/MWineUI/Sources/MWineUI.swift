import SwiftUI
import UniformTypeIdentifiers

// MARK: - 模型

struct VmStatus: Codable {
    struct VM: Codable { let running: Bool }
    let vm: VM
    let error: String?
}

struct VmConfig: Codable {
    var mode: String = "phone"
    var width: UInt32 = 1080
    var height: UInt32 = 2400
    var dpi: UInt32 = 440
    var memoryMB: UInt32 = 2048
    var sdcardPath: String = ""
    var shareDir: String = ""
    var avd: String = "MWine4"
    var proxy: String = ""
    // 专业参数
    var cpuCores: UInt32 = 0
    var gpuMode: String = ""
    var qemuExtra: String = ""
    var netspeed: String = ""
    var netdelay: String = ""
    var noSnapshot: Bool = false
    var noBootAnim: Bool = false

    enum CodingKeys: String, CodingKey {
        case mode, width, height, dpi
        case memoryMB = "memory_mb"
        case sdcardPath = "sdcard_path"
        case shareDir = "share_dir"
        case avd, proxy
        case cpuCores = "cpu_cores"
        case gpuMode = "gpu_mode"
        case qemuExtra = "qemu_extra"
        case netspeed, netdelay
        case noSnapshot = "no_snapshot"
        case noBootAnim = "no_boot_anim"
    }
}

struct AppEntry: Codable, Identifiable, Hashable {
    let package: String
    let name: String
    var id: String { package }
}
struct AppsResponse: Codable { let apps: [AppEntry] }

/// 后端返回的混合类型字段（bool/string 二选一）
struct BoolOrString: Codable {
    let bool: Bool?
    let str: String?
    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        bool = try? c.decode(Bool.self)
        str = try? c.decode(String.self)
    }
    var boolValue: Bool? { bool }
    var stringValue: String? { str }
}

struct InstalledImage: Codable, Identifiable {
    let api: String
    let display: String?
    let name: String
    let path: String
    let sizeMb: UInt?
    let rel: String?
    let inUse: Bool?
    enum CodingKeys: String, CodingKey {
        case api, display, name, path, rel
        case sizeMb = "size_mb"
        case inUse = "in_use"
    }
    var id: String { "\(api)/\(name)" }
}
struct InstalledImagesResponse: Codable { let images: [InstalledImage] }
struct OfficialImage: Codable, Identifiable {
    let path: String
    let display: String?
    let size: UInt?
    let url: String?
    var id: String { path }
}
struct OfficialImagesResponse: Codable { let images: [OfficialImage] }
struct ImageTask: Codable {
    var running: Bool?
    var done: Bool?
    var lines: [String]?
    var error: String?
    var result: String?
    var phase: String?
    var progress: UInt64?
    var progressTotal: UInt64?
    enum CodingKeys: String, CodingKey {
        case running, done, lines, error, result, phase, progress
        case progressTotal = "progress_total"
    }
}

struct PerfInfo: Codable {
    var running: Bool?
    var cpuPct: UInt?
    var uptimeS: UInt?
    var memTotalMb: UInt?
    var memAvailMb: UInt?
    var hostQemuCpu: Double?
    var hostQemuMemMb: UInt?

    enum CK: String, CodingKey {
        case running, cpu_pct, uptime_s, total_mb, available_mb, qemu_cpu_pct, qemu_mem_mb
        case mem, host
    }

    init(running: Bool?, cpuPct: UInt?, uptimeS: UInt?, memTotalMb: UInt?, memAvailMb: UInt?,
         hostQemuCpu: Double?, hostQemuMemMb: UInt?) {
        self.running = running; self.cpuPct = cpuPct; self.uptimeS = uptimeS
        self.memTotalMb = memTotalMb; self.memAvailMb = memAvailMb
        self.hostQemuCpu = hostQemuCpu; self.hostQemuMemMb = hostQemuMemMb
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CK.self)
        running = try c.decodeIfPresent(Bool.self, forKey: .running)
        cpuPct = try c.decodeIfPresent(UInt.self, forKey: .cpu_pct)
        uptimeS = try c.decodeIfPresent(UInt.self, forKey: .uptime_s)
        if let mem = try? c.nestedContainer(keyedBy: CK.self, forKey: .mem) {
            memTotalMb = try? mem.decodeIfPresent(UInt.self, forKey: .total_mb)
            memAvailMb = try? mem.decodeIfPresent(UInt.self, forKey: .available_mb)
        } else {
            memTotalMb = try? c.decodeIfPresent(UInt.self, forKey: .total_mb)
            memAvailMb = try? c.decodeIfPresent(UInt.self, forKey: .available_mb)
        }
        if let host = try? c.nestedContainer(keyedBy: CK.self, forKey: .host) {
            hostQemuCpu = try? host.decodeIfPresent(Double.self, forKey: .qemu_cpu_pct)
            hostQemuMemMb = try? host.decodeIfPresent(UInt.self, forKey: .qemu_mem_mb)
        } else {
            hostQemuCpu = try? c.decodeIfPresent(Double.self, forKey: .qemu_cpu_pct)
            hostQemuMemMb = try? c.decodeIfPresent(UInt.self, forKey: .qemu_mem_mb)
        }
    }
}

struct KeyBinding: Codable, Identifiable, Hashable {
    var id = UUID()
    var key: String
    var type: String
    var x: Double
    var y: Double
    var x2: Double?
    var y2: Double?
    var duration: Int?
    var interval: Int?
    enum CodingKeys: String, CodingKey { case key, type, x, y, x2, y2, duration, interval }
}

struct KeysResp: Codable { let bindings: [KeyBinding] }

struct ShellApp: Identifiable, Hashable {
    let url: URL
    var id: String { url.path }
    var name: String {
        let p = url.appendingPathComponent("Contents/Info.plist")
        if let dict = NSDictionary(contentsOf: p),
           let n = dict["CFBundleDisplayName"] as? String ?? dict["CFBundleName"] as? String,
           !n.isEmpty { return n }
        return url.deletingPathExtension().lastPathComponent
    }
    var pkg: String {
        let conf = url.appendingPathComponent("Contents/Resources/app.conf")
        guard let text = try? String(contentsOf: conf, encoding: .utf8) else { return "" }
        for line in text.split(separator: "\n") {
            if line.hasPrefix("package=") { return String(line.dropFirst("package=".count)) }
        }
        return ""
    }
    var icon: NSImage {
        let p = url.appendingPathComponent("Contents/Resources/AppIcon.icns")
        if FileManager.default.fileExists(atPath: p.path), let img = NSImage(contentsOf: p) { return img }
        return NSWorkspace.shared.icon(forFile: url.path)
    }
}

// MARK: - API

class MWineAPI: ObservableObject {
    @Published var vmRunning = false
    @Published var config = VmConfig()
    @Published var shells: [ShellApp] = []
    @Published var apps: [AppEntry] = []
    @Published var installedPkgs: Set<String> = []
    @Published var perf = PerfInfo(running: false, cpuPct: 0, uptimeS: 0, memTotalMb: 0,
                                    memAvailMb: 0, hostQemuCpu: 0, hostQemuMemMb: 0)
    @Published var booting = false
    @Published var installing = false
    @Published var message = ""
    @Published var configSaved = false
    @Published var keywatcherRunning = false
    var keywatcher: Process?
    // 镜像页
    @Published var installedImages: [InstalledImage] = []
    @Published var officialImages: [OfficialImage] = []
    @Published var imageTask: ImageTask?
    @Published var officialLoading = false
    private var imagePollTimer: Timer?
    func loadInstalledImages() {
        guard let u = URL(string: "\(base)/api/image/list") else { return }
        URLSession.shared.dataTask(with: u) { data, _, _ in
            guard let d = data,
                  let r = try? JSONDecoder().decode(InstalledImagesResponse.self, from: d) else { return }
            DispatchQueue.main.async { self.installedImages = r.images }
        }.resume()
    }
    func loadOfficialImages(filter: String = "", proxy: String = "") {
        officialLoading = true
        var comps = URLComponents(string: "\(base)/api/image/official")!
        var items: [URLQueryItem] = []
        if !filter.isEmpty { items.append(URLQueryItem(name: "filter", value: filter)) }
        if !proxy.isEmpty { items.append(URLQueryItem(name: "proxy", value: proxy)) }
        comps.queryItems = items.isEmpty ? nil : items
        URLSession.shared.dataTask(with: comps.url!) { data, _, _ in
            defer { DispatchQueue.main.async { self.officialLoading = false } }
            guard let d = data,
                  let r = try? JSONDecoder().decode(OfficialImagesResponse.self, from: d) else {
                DispatchQueue.main.async { self.message = "官方仓库不可达（检查网络/代理）" }
                return
            }
            DispatchQueue.main.async { self.officialImages = r.images }
        }.resume()
    }
    func importImage(path: String) {
        var req = URLRequest(url: URL(string: "\(base)/api/image/import")!)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONEncoder().encode(["path": path])
        URLSession.shared.dataTask(with: req) { _, _, _ in }.resume()
        startImagePolling()
    }
    func pullOfficial(_ target: String, proxy: String = "") {
        var req = URLRequest(url: URL(string: "\(base)/api/image/official-pull")!)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        var body: [String: String] = ["target": target]
        if !proxy.isEmpty { body["proxy"] = proxy }
        req.httpBody = try? JSONEncoder().encode(body)
        URLSession.shared.dataTask(with: req) { _, _, _ in }.resume()
        startImagePolling()
    }
    func cancelImageTask() {
        var req = URLRequest(url: URL(string: "\(base)/api/image/cancel")!)
        req.httpMethod = "POST"
        URLSession.shared.dataTask(with: req) { _, _, _ in }.resume()
    }
    func deleteImage(_ img: InstalledImage) {
        var req = URLRequest(url: URL(string: "\(base)/api/image/delete")!)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONEncoder().encode(["path": img.path])
        URLSession.shared.dataTask(with: req) { data, resp, _ in
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            // 注意：成功响应是 {"ok": true}（布尔），失败是 {"error": "..."}（字符串）
            // ——必须用 BoolOrString 混合解码，纯 [String:String] 会静默解码失败
            // 导致"删除成功但列表不刷新"（切镜像曾踩过同款坑）
            guard let d = data,
                  let r = try? JSONDecoder().decode([String: BoolOrString].self, from: d) else {
                DispatchQueue.main.async {
                    self.message = "删除失败：无法连接守护进程"
                    self.loadInstalledImages()
                }
                return
            }
            DispatchQueue.main.async {
                if let err = r["error"]?.stringValue, !err.isEmpty {
                    self.message = "删除失败：\(err)"
                    self.showImageBanner("删除失败：\(err)", ok: false)
                } else if code != 200 {
                    self.message = "删除失败（HTTP \(code)）"
                    self.showImageBanner("删除失败（HTTP \(code)）", ok: false)
                } else {
                    self.message = "已删除 \(img.display ?? img.api) \(img.name)"
                    self.showImageBanner("已删除 \(img.display ?? img.api) \(img.name)", ok: true)
                }
                self.loadInstalledImages()
            }
        }.resume()
    }
    @Published var imageBanner = ""
    @Published var imageBannerOk = true
    @Published var imageSwitching = false
    func useImage(_ img: InstalledImage, stopVm: Bool = false) {
        if !stopVm { imageSwitching = true }
        var req = URLRequest(url: URL(string: "\(base)/api/image/use")!)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.timeoutInterval = 90  // 关停(≤35s)+切换在服务端同步完成
        // 注意：stop_vm 必须是 JSON bool——发字符串 daemon 的 as_bool() 会解成 false，
        // 曾导致「确认后仍 409 → 弹框无限循环」
        struct UseBody: Codable { let path: String; let stop_vm: Bool }
        req.httpBody = try? JSONEncoder().encode(UseBody(path: img.path, stop_vm: stopVm))
        URLSession.shared.dataTask(with: req) { data, resp, _ in
            let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard let d = data,
                  let r = try? JSONDecoder().decode([String: BoolOrString].self, from: d) else {
                DispatchQueue.main.async {
                    self.imageSwitching = false
                    self.showImageBanner("切换失败：无法连接守护进程", ok: false)
                }
                return
            }
            DispatchQueue.main.async {
                self.imageSwitching = false
                if code == 409, r["needs_confirm"]?.boolValue == true {
                    // 兜底：预确认后仍 409（竞态：确认期间 VM 恰好启动）→ 再走一轮确认
                    self.pendingUse = img
                } else if let err = r["error"]?.stringValue, !err.isEmpty {
                    self.showImageBanner("切换失败：\(err)", ok: false)
                } else if code == 200 {
                    let restarted = r["restarted"]?.boolValue == true
                    self.showImageBanner(
                        restarted
                            ? "已切换到 \(img.display ?? img.api) \(img.name)（数据已重置），虚拟机正在以新镜像启动…"
                            : "已切换到 \(img.display ?? img.api) \(img.name)（数据已重置），下次启动生效",
                        ok: true)
                    self.booting = restarted   // 概览页的启动状态同步
                } else {
                    self.showImageBanner("切换失败（HTTP \(code)）", ok: false)
                }
                self.loadInstalledImages()
            }
        }.resume()
    }
    @Published var pendingUse: InstalledImage?
    @Published var pendingStopUse: InstalledImage?
    private func showImageBanner(_ text: String, ok: Bool) {
        imageBanner = text
        imageBannerOk = ok
        message = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) {
            if self.imageBanner == text { self.imageBanner = "" }
        }
    }
    private func startImagePolling() {
        imagePollTimer?.invalidate()
        imageTask = ImageTask(running: true, done: false, lines: ["任务提交…"], error: nil, result: nil)
        imagePollTimer = Timer.scheduledTimer(withTimeInterval: 1.5, repeats: true) { t in
            guard let u = URL(string: "\(self.base)/api/image/task") else { return }
            URLSession.shared.dataTask(with: u) { data, _, _ in
                guard let d = data,
                      let r = try? JSONDecoder().decode(ImageTask.self, from: d) else { return }
                DispatchQueue.main.async {
                    self.imageTask = r
                    if r.running != true {
                        t.invalidate()
                        self.loadInstalledImages()
                        if r.error?.isEmpty == false { self.message = "镜像任务失败：\(r.error!)" }
                        else if r.done == true { self.message = "镜像安装完成" }
                    }
                }
            }.resume()
        }
    }
    // 性能历史（近 90 个采样点，2s 一个 ≈ 3 分钟）
    @Published var cpuHistory: [Double] = []
    @Published var memHistory: [Double] = []

    static let shellsDir: URL = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Applications/Houttuynia")
    private var coreProcess: Process?
    private let base = "http://127.0.0.1:7788"

    init() {
        reloadShells()
        startCore()
        loadConfig()
        Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { _ in
            self.checkStatus()
        }
        // perf 轮询降载：VM 运行才拉、5s 粒度（弱机 M1 上 2s 的 adb shell 轮询有感知开销）
        Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
            if self.vmRunning { self.refreshPerf() }
        }
    }

    deinit { coreProcess?.terminate(); keywatcher?.terminate() }

    // MARK: 应用库

    func reloadShells() {
        let fm = FileManager.default
        let exclude: Set<String> = ["Houttuynia.app", "MWine.app", "Mwine Installer.app"]
        guard let entries = try? fm.contentsOfDirectory(
            at: Self.shellsDir, includingPropertiesForKeys: [.isDirectoryKey]) else { return }
        var result: [ShellApp] = []
        for e in entries where e.pathExtension == "app" && !exclude.contains(e.lastPathComponent) {
            guard fm.fileExists(atPath: e.appendingPathComponent("Contents/Resources/app.conf").path)
            else { continue }
            result.append(ShellApp(url: e))
        }
        DispatchQueue.main.async { self.shells = result.sorted { $0.name < $1.name } }
    }

    func launchShell(_ app: ShellApp) {
        NSWorkspace.shared.open(app.url)
        message = "已启动 \(app.name)"
    }

    func loadApps() {
        guard let u = URL(string: "\(base)/api/apps") else { return }
        URLSession.shared.dataTask(with: u) { data, _, _ in
            guard let d = data,
                  let r = try? JSONDecoder().decode(AppsResponse.self, from: d) else { return }
            DispatchQueue.main.async {
                self.apps = r.apps
                self.installedPkgs = Set(r.apps.map { $0.package })
            }
        }.resume()
    }

    func isInstalled(_ app: ShellApp) -> Bool {
        installedPkgs.isEmpty ? true : installedPkgs.contains(app.pkg)
    }

    func uninstall(app: ShellApp) {
        message = "卸载 \(app.name)..."
        DispatchQueue.global().async {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: Self.cliPath())
            p.arguments = ["uninstall", app.pkg]
            p.standardOutput = Pipe(); p.standardError = Pipe()
            try? p.run(); p.waitUntilExit()
            // 移除桌面壳
            try? FileManager.default.trashItem(at: app.url, resultingItemURL: nil)
            DispatchQueue.main.async {
                self.message = "✓ 已卸载 \(app.name)"
                self.reloadShells()
                self.loadApps()
            }
        }
    }

    // MARK: 守护进程

    private func startCore() {
        let u = URL(string: "\(base)/health")!
        var req = URLRequest(url: u); req.timeoutInterval = 1
        URLSession.shared.dataTask(with: req) { _, _, err in
            if err != nil { DispatchQueue.main.async { self.spawnCore() } }
        }.resume()
    }

    private func spawnCore() {
        let p = Process()
        if let path = Bundle.main.executablePath?.components(separatedBy: "/MacOS/").first?
            .appending("/MacOS/mwine-core"),
           FileManager.default.fileExists(atPath: path) {
            p.executableURL = URL(fileURLWithPath: path)
        } else {
            let dev = "/Users/\(NSUserName())/Downloads/aiproject/houttuynia/target/release/mwine-core"
            guard FileManager.default.fileExists(atPath: dev) else { return }
            p.executableURL = URL(fileURLWithPath: dev)
        }
        p.standardOutput = Pipe(); p.standardError = Pipe()
        try? p.run()
        coreProcess = p
    }

    func loadConfig() {
        guard let u = URL(string: "\(base)/api/config") else { return }
        URLSession.shared.dataTask(with: u) { data, _, _ in
            guard let d = data, let c = try? JSONDecoder().decode(VmConfig.self, from: d) else { return }
            DispatchQueue.main.async { self.config = c }
        }.resume()
    }

    func saveConfig() {
        guard let u = URL(string: "\(base)/api/config") else { return }
        var req = URLRequest(url: u)
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONEncoder().encode(config)
        URLSession.shared.dataTask(with: req) { _, resp, _ in
            let ok = (resp as? HTTPURLResponse)?.statusCode == 200
            DispatchQueue.main.async {
                self.configSaved = ok
                self.message = ok ? "✓ 配置已保存" : "✗ 保存失败"
                if ok {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.configSaved = false }
                }
            }
        }.resume()
    }

    func checkStatus() {
        guard let u = URL(string: "\(base)/api/status") else { return }
        URLSession.shared.dataTask(with: u) { data, _, _ in
            guard let d = data,
                  let s = try? JSONDecoder().decode(VmStatus.self, from: d) else { return }
            DispatchQueue.main.async {
                let was = self.vmRunning
                self.vmRunning = s.vm.running
                self.booting = false
                if s.vm.running && !was { self.loadApps() }
                if let err = s.error, !err.isEmpty, !self.vmRunning {
                    self.message = "✗ \(err)"
                }
            }
        }.resume()
    }

    func refreshPerf() {
        guard let u = URL(string: "\(base)/api/perf") else { return }
        URLSession.shared.dataTask(with: u) { data, _, _ in
            guard let d = data,
                  let p = try? JSONDecoder().decode(PerfInfo.self, from: d) else { return }
            DispatchQueue.main.async {
                self.perf = p
                if let c = p.cpuPct {
                    self.cpuHistory.append(Double(c))
                    if self.cpuHistory.count > 90 { self.cpuHistory.removeFirst() }
                }
                if let t = p.memTotalMb, t > 0, let a = p.memAvailMb {
                    let used = Double(t - a) / Double(t) * 100
                    self.memHistory.append(used)
                    if self.memHistory.count > 90 { self.memHistory.removeFirst() }
                }
                if self.vmRunning && self.apps.isEmpty { self.loadApps() }
            }
        }.resume()
    }

    func startVM() {
        booting = true
        message = "启动 Android..."
        guard let u = URL(string: "\(base)/api/vm/start") else { return }
        var req = URLRequest(url: u); req.httpMethod = "POST"
        URLSession.shared.dataTask(with: req) { _, _, _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { self.checkStatus() }
        }.resume()
    }

    func stopVM() {
        message = "正在关机（保存状态）..."
        guard let u = URL(string: "\(base)/api/vm/stop") else { return }
        var req = URLRequest(url: u); req.httpMethod = "POST"
        URLSession.shared.dataTask(with: req) { _, _, _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.checkStatus() }
        }.resume()
    }

    func screenshot() {
        guard let u = URL(string: "\(base)/api/screenshot") else { return }
        var req = URLRequest(url: u); req.httpMethod = "POST"
        message = "截屏中..."
        URLSession.shared.dataTask(with: req) { data, _, _ in
            guard let d = data,
                  let v = try? JSONDecoder().decode([String: String].self, from: d) else {
                DispatchQueue.main.async { self.message = "✗ 截屏失败" }
                return
            }
            DispatchQueue.main.async {
                if v["ok"] == "true", let path = v["path"] {
                    self.message = "✓ 已存到桌面"
                    NSWorkspace.shared.selectFile(path, inFileViewerRootedAtPath: "")
                } else {
                    self.message = "✗ \(v["error"] ?? "截屏失败")"
                }
            }
        }.resume()
    }

    func vmReset() {
        message = "正在重置虚拟机..."
        guard let u = URL(string: "\(base)/api/vm/reset") else { return }
        var req = URLRequest(url: u); req.httpMethod = "POST"
        URLSession.shared.dataTask(with: req) { data, _, _ in
            guard let d = data,
                  let v = try? JSONDecoder().decode([String: String].self, from: d) else {
                DispatchQueue.main.async { self.message = "✗ 重置失败" }
                return
            }
            DispatchQueue.main.async {
                self.message = "✓ 已重置（下次启动为全新系统）"
                self.checkStatus()
            }
        }.resume()
    }

    func identityReset() {
        guard let u = URL(string: "\(base)/api/identity/reset") else { return }
        var req = URLRequest(url: u); req.httpMethod = "POST"
        message = "重置设备标识..."
        URLSession.shared.dataTask(with: req) { data, _, _ in
            guard let d = data,
                  let v = try? JSONDecoder().decode([String: String].self, from: d) else {
                DispatchQueue.main.async { self.message = "✗ 重置失败" }
                return
            }
            DispatchQueue.main.async {
                self.message = v["ok"] == "true" ? "✓ 已重置（android_id: \(v["android_id"] ?? "")）"
                                                  : "✗ 重置失败"
            }
        }.resume()
    }

    // MARK: APK 安装

    func installAPK(_ path: String) {
        installing = true
        message = "安装中: \((path as NSString).lastPathComponent)"
        DispatchQueue.global().async {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: Self.cliPath())
            // app create --apk：安装 + 解析名/图标 + 生成桌面 .app 壳（装完即有图标）
            p.arguments = ["app", "create", "--apk", path]
            p.standardOutput = Pipe(); p.standardError = Pipe()
            do {
                try p.run()
                p.waitUntilExit()
                let ok = p.terminationStatus == 0
                DispatchQueue.main.async {
                    self.installing = false
                    self.message = ok ? "✓ 安装成功" : "✗ 安装失败"
                    self.reloadShells()
                    self.loadApps()
                    if ok { self.activateEmulator() }
                }
            } catch {
                DispatchQueue.main.async {
                    self.installing = false
                    self.message = "✗ \(error.localizedDescription)"
                }
            }
        }
    }

    func installWithAutoStart(_ path: String) {
        if vmRunning { installAPK(path); return }
        startVM()
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) {
            if self.vmRunning { self.installAPK(path) }
            else {
                DispatchQueue.main.asyncAfter(deadline: .now() + 10) {
                    if self.vmRunning { self.installAPK(path) }
                    else {
                        self.installing = false
                        self.message = "✗ Android 未就绪，安装取消"
                    }
                }
            }
        }
    }

    func activateEmulator() {
        let script = NSAppleScript(source: "tell application \"System Events\" to set frontmost of (first process whose name begins with \"qemu-system\") to true")
        var err: NSDictionary?
        script?.executeAndReturnError(&err)
    }

    static func cliPath() -> String {
        if let path = Bundle.main.executablePath?.components(separatedBy: "/MacOS/").first?
            .appending("/MacOS/mwine"),
           FileManager.default.fileExists(atPath: path) { return path }
        return "/Users/\(NSUserName())/Downloads/aiproject/houttuynia/target/release/mwine"
    }

    // MARK: 按键注入器

    func startKeywatcher(pkg: String) {
        stopKeywatcher()
        let p = Process()
        p.executableURL = URL(fileURLWithPath: Self.cliPath())
        p.arguments = ["keys", "watch", pkg]
        p.standardOutput = Pipe(); p.standardError = Pipe()
        do {
            try p.run()
            keywatcher = p
            keywatcherRunning = true
            message = "按键映射已启用（\(pkg)）"
        } catch {
            message = "✗ 注入器启动失败: \(error.localizedDescription)"
        }
    }

    func stopKeywatcher() {
        keywatcher?.terminate()
        keywatcher = nil
        keywatcherRunning = false
    }
}

// MARK: - 主窗口

struct MainWindow: View {
    @StateObject private var api = MWineAPI()
    @State private var page = 1
    @State private var showInstall = false
    @State private var showSdcardPicker = false
    @State private var showSharePicker = false

    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Houttuynia")
                    .font(.system(size: 15, weight: .bold))
                    .padding(.horizontal, 14).padding(.top, 14).padding(.bottom, 8)

                SidebarSection("虚拟机")
                SidebarItem(1, icon: "gauge", label: "概览", page: $page)
                SidebarItem(2, icon: "gearshape", label: "设置", page: $page)
                SidebarSection("应用")
                SidebarItem(3, icon: "square.grid.3x3", label: "应用库 (\(api.shells.count))", page: $page)
                SidebarItem(4, icon: "keyboard", label: "按键映射", page: $page)
                SidebarSection("监控")
                SidebarItem(5, icon: "chart.bar", label: "性能", page: $page)
                SidebarItem(6, icon: "internaldrive", label: "镜像", page: $page)

                Spacer()

                if !api.message.isEmpty {
                    Text(api.message)
                        .font(.system(size: 9))
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .padding(.horizontal, 12)
                }
                HStack(spacing: 6) {
                    Circle().fill(api.vmRunning ? .green : .secondary).frame(width: 7, height: 7)
                    Text(api.vmRunning ? "Android 运行中" : "已关机")
                        .font(.caption).foregroundStyle(.secondary)
                }
                .padding(.horizontal, 14).padding(.bottom, 12)
            }
            .frame(width: 178)
            .background(.bar)

            Divider()

            Group {
                switch page {
                case 2: SettingsPage(api: api, sdcardPicker: $showSdcardPicker, sharePicker: $showSharePicker)
                case 3: AppLibraryPage(api: api, install: { showInstall = true }, page: $page)
                case 4: KeymapPage(api: api)
                case 5: PerfPage(api: api)
                case 6: ImagePage(api: api)
                default: OverviewPage(api: api, install: { showInstall = true })
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(minWidth: 880, minHeight: 560)
        .fileImporter(isPresented: $showInstall,
                      allowedContentTypes: [UTType(filenameExtension: "apk") ?? .data],
                      onCompletion: { result in
            if case .success(let url) = result { api.installWithAutoStart(url.path) }
        })
        .onDrop(of: [.fileURL], isTargeted: nil) { providers in
            for p in providers where p.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
                p.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                    guard let data = item as? Data,
                          let url = URL(dataRepresentation: data, relativeTo: nil) else { return }
                    if url.pathExtension.lowercased() == "apk" {
                        DispatchQueue.main.async { api.installWithAutoStart(url.path) }
                    }
                }
            }
            return true
        }
    }
}

struct SidebarSection: View {
    let title: String
    init(_ t: String) { title = t }
    var body: some View {
        Text(title)
            .font(.system(size: 10, weight: .semibold))
            .foregroundStyle(.tertiary)
            .padding(.horizontal, 14).padding(.top, 8).padding(.bottom, 2)
    }
}

struct SidebarItem: View {
    let target: Int
    let icon: String
    let label: String
    @Binding var page: Int

    init(_ t: Int, icon: String, label: String, page: Binding<Int>) {
        target = t; self.icon = icon; self.label = label; _page = page
    }

    var body: some View {
        Button {
            withAnimation(.easeInOut(duration: 0.12)) { page = target }
        } label: {
            HStack(spacing: 7) {
                Image(systemName: icon).frame(width: 17)
                Text(label).font(.system(size: 13))
                Spacer()
            }
            .padding(.horizontal, 10).padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 6)
                .fill(page == target ? Color.accentColor.opacity(0.16) : Color.clear))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .padding(.horizontal, 7)
    }
}

// MARK: 概览

struct OverviewPage: View {
    @ObservedObject var api: MWineAPI
    var install: () -> Void

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                HStack(spacing: 22) {
                    Image(systemName: api.vmRunning ? "iphone.gen3.radiowaves.left.and.right" : "iphone.gen3")
                        .font(.system(size: 34))
                        .foregroundStyle(api.vmRunning ? .green : .secondary)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(api.vmRunning ? "Android 运行中" : "Android 已关机")
                            .font(.system(size: 17, weight: .semibold))
                        Text(modeText)
                            .font(.caption).foregroundStyle(.secondary)
                        Text("运行 \(formatUptime(api.perf.uptimeS ?? 0)) · CPU \(api.perf.cpuPct ?? 0)%")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    VStack(spacing: 8) {
                        Button {
                            if api.vmRunning { api.stopVM() } else { api.startVM() }
                        } label: {
                            HStack {
                                if api.booting { ProgressView().scaleEffect(0.7) }
                                Image(systemName: api.vmRunning ? "stop.fill" : "play.fill")
                                Text(api.vmRunning ? "关机" : "启动")
                            }
                            .frame(width: 108)
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(api.vmRunning ? .red : .accentColor)
                        .disabled(api.booting)

                        Button(action: install) {
                            HStack { Image(systemName: "plus"); Text("安装 APK") }.frame(width: 108)
                        }
                        .buttonStyle(.bordered)
                        .disabled(api.installing)

                        if api.vmRunning {
                            Button {
                                api.activateEmulator()
                            } label: {
                                HStack { Image(systemName: "macwindow"); Text("显示窗口") }.frame(width: 108)
                            }
                            .buttonStyle(.bordered)
                            Button {
                                api.screenshot()
                            } label: {
                                HStack { Image(systemName: "camera"); Text("截屏") }.frame(width: 108)
                            }
                            .buttonStyle(.bordered)
                        }
                    }
                }
                .padding(18)

                if api.vmRunning {
                    HStack(spacing: 12) {
                        StatCard(title: "内存可用", value: "\(api.perf.memAvailMb ?? 0) MB",
                                 sub: "共 \(api.perf.memTotalMb ?? 0) MB", icon: "memorychip")
                        StatCard(title: "宿主开销", value: String(format: "%.0f%%", api.perf.hostQemuCpu ?? 0),
                                 sub: "\(api.perf.hostQemuMemMb ?? 0) MB 内存", icon: "cpu")
                        StatCard(title: "已装应用", value: "\(api.apps.count)",
                                 sub: "壳 \(api.shells.count) 个", icon: "app.badge")
                    }
                }

                Text("拖放 APK 到本窗口即可安装")
                    .font(.caption).foregroundStyle(.tertiary)
            }
            .padding(20)
        }
    }

    var modeText: String {
        switch api.config.mode {
        case "phone": "手机模式 · 1080×2400 竖屏 · \(api.config.memoryMB)MB"
        case "tablet": "平板模式 · 2560×1600 横屏 · \(api.config.memoryMB)MB"
        default: "自定义 · \(api.config.width)×\(api.config.height) @\(api.config.dpi) · \(api.config.memoryMB)MB"
        }
    }
}

struct StatCard: View {
    let title: String; let value: String; let sub: String; let icon: String
    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: icon).font(.system(size: 20)).foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.caption2).foregroundStyle(.secondary)
                Text(value).font(.system(size: 15, weight: .semibold)).monospacedDigit()
                Text(sub).font(.caption2).foregroundStyle(.tertiary)
            }
            Spacer()
        }
        .padding(12)
        .frame(maxWidth: .infinity)
        .background(RoundedRectangle(cornerRadius: 10).fill(.background).shadow(radius: 1))
    }
}

func formatUptime(_ s: UInt) -> String {
    let h = s / 3600, m = (s % 3600) / 60
    return h > 0 ? "\(h)小时\(m)分" : "\(m)分钟"
}

// MARK: 设置

struct SettingsPage: View {
    @ObservedObject var api: MWineAPI
    @Binding var sdcardPicker: Bool
    @Binding var sharePicker: Bool
    @State private var showResetConfirm = false
    @State private var showIdentityConfirm = false

    var body: some View {
        Form {
            Section("设备形态") {
                Picker("模式", selection: $api.config.mode) {
                    Text("手机（竖屏）").tag("phone")
                    Text("平板（横屏）").tag("tablet")
                    Text("TV（电视）").tag("tv")
                    Text("自定义").tag("custom")
                }
                .pickerStyle(.segmented)
                if api.config.mode == "custom" {
                    HStack {
                        Text("尺寸")
                        TextField("宽", value: $api.config.width, format: .number.grouping(.never))
                            .textFieldStyle(.roundedBorder)
                        Text("×")
                        TextField("高", value: $api.config.height, format: .number.grouping(.never))
                            .textFieldStyle(.roundedBorder)
                        Text("@")
                        TextField("DPI", value: $api.config.dpi, format: .number.grouping(.never))
                            .textFieldStyle(.roundedBorder)
                    }
                }
                Text("运行中保存 → 屏幕立即切换；模拟器窗口尺寸下次启动完全生效。")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("性能") {
                HStack {
                    Text("内存")
                    Slider(value: Binding(
                        get: { Double(api.config.memoryMB) },
                        set: { api.config.memoryMB = UInt32(($0 / 256).rounded() * 256) }
                    ), in: 1024...8192, step: 256)
                    Text("\(api.config.memoryMB) MB").monospacedDigit()
                        .frame(width: 70, alignment: .trailing)
                }
                Text("内存档位下次启动生效（快照按档位匹配）。")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("存储（暂不可用）") {
                HStack {
                    Image(systemName: "lock.fill").foregroundStyle(.orange)
                    Text("sdcard 挂载位置")
                    Spacer()
                    Text("即将支持").font(.caption)
                        .padding(.horizontal, 8).padding(.vertical, 2)
                        .background(.orange.opacity(0.15), in: Capsule())
                        .foregroundStyle(.orange)
                }
                HStack {
                    Image(systemName: "lock.fill").foregroundStyle(.orange)
                    Text("宿主共享目录")
                    Spacer()
                    Text("即将支持").font(.caption)
                        .padding(.horizontal, 8).padding(.vertical, 2)
                        .background(.orange.opacity(0.15), in: Capsule())
                        .foregroundStyle(.orange)
                }
                Text("sdcard 挂载与宿主目录共享正在底层改造中（改用 QEMU vvfat 块设备直通），当前版本暂不可配置。")
                    .font(.caption).foregroundStyle(.secondary)
            }

            Section("专业参数") {
                DisclosureGroup("CPU / GPU / 网络") {
                    HStack {
                        Text("CPU 核数")
                        Spacer()
                        TextField("4", value: $api.config.cpuCores, format: .number.grouping(.never))
                            .textFieldStyle(.roundedBorder).frame(width: 60)
                        Stepper("", value: $api.config.cpuCores, in: 0...8).labelsHidden()
                    }
                    Text("0 = 默认 4 核。M1 建议 2~4，M3/M4 可 6~8。").font(.caption2).foregroundStyle(.secondary)
                    Picker("GPU 后端", selection: $api.config.gpuMode) {
                        Text("默认（自动）").tag("")
                        Text("host（Metal 直渲）").tag("host")
                        Text("swangle（软渲染）").tag("swangle")
                        Text("swiftshader（兼容）").tag("swiftshader_indirect")
                    }
                    Text("M1 上如遇黑屏/卡顿可尝试切换。改后需重启虚拟机。").font(.caption2).foregroundStyle(.secondary)
                    Picker("网络速度模拟", selection: $api.config.netspeed) {
                        Text("不限速").tag("")
                        Text("gsm (14.4K)").tag("gsm")
                        Text("hscsd (43.2K)").tag("hscsd")
                        Text("gprs (80K)").tag("gprs")
                        Text("edge (236K)").tag("edge")
                        Text("umts (3.9M)").tag("umts")
                        Text("lte (100M)").tag("lte")
                        Text("full (不限)").tag("full")
                    }
                    Picker("网络延迟模拟", selection: $api.config.netdelay) {
                        Text("无延迟").tag("")
                        Text("gprs (~800ms)").tag("gprs")
                        Text("edge (~400ms)").tag("edge")
                        Text("umts (~200ms)").tag("umts")
                        Text("lte (~70ms)").tag("lte")
                        Text("none (0ms)").tag("none")
                    }
                    Text("模拟弱网环境测试应用。").font(.caption2).foregroundStyle(.secondary)
                }
                DisclosureGroup("启动行为") {
                    Toggle("禁用快照（每次冷启动）", isOn: $api.config.noSnapshot)
                    Toggle("跳过启动动画", isOn: $api.config.noBootAnim)
                    Text("冷启动约 20 秒 vs 快照恢复约 5 秒。禁用快照可排查启动问题。").font(.caption2).foregroundStyle(.secondary)
                }
                DisclosureGroup("QEMU 附加参数") {
                    TextField("如 -cpu host -smp 6", text: $api.config.qemuExtra)
                        .textFieldStyle(.roundedBorder).font(.system(size: 12, design: .monospaced))
                    Text("原样追加到 QEMU 透传块（-qemu 之后），仅供高级调试。填错可能无法启动。")
                        .font(.caption2).foregroundStyle(.secondary)
                }
            }
            Section("网络与隐私") {
                TextField("HTTP 代理（如 http://127.0.0.1:7897，空 = 直连）", text: $api.config.proxy)
                    .textFieldStyle(.roundedBorder)
                Text("下次启动生效（guest 内自动指向宿主代理）。").font(.caption).foregroundStyle(.secondary)
                HStack {
                    Text("设备身份")
                    Spacer()
                    Button("重置设备标识") { showIdentityConfirm = true }
                        .disabled(!api.vmRunning)
                    Text("应用将视作新设备").font(.caption).foregroundStyle(.secondary)
                }
                HStack {
                    Text("重置虚拟机")
                    Spacer()
                    Button("抹掉全部数据…", role: .destructive) { showResetConfirm = true }
                    Text("应用与数据全清").font(.caption).foregroundStyle(.secondary)
                }
            }

            HStack {
                Spacer()
                Button { api.saveConfig() } label: {
                    Text(api.configSaved ? "✓ 已保存" : "保存配置").frame(minWidth: 100)
                }
                .buttonStyle(.borderedProminent)
                Spacer()
            }
            .padding(.vertical, 4)
        }
        .formStyle(.grouped)
        .confirmationDialog("确定重置虚拟机？所有应用与数据将被清空（不可恢复）",
                            isPresented: $showResetConfirm, titleVisibility: .visible) {
            Button("重置", role: .destructive) { api.vmReset() }
            Button("取消", role: .cancel) {}
        }
        .confirmationDialog("确定重置设备标识？",
                            isPresented: $showIdentityConfirm, titleVisibility: .visible) {
            Button("重置", role: .destructive) { api.identityReset() }
            Button("取消", role: .cancel) {}
        } message: {
            Text("应用会将本机视作全新设备：可能需要重新登录账号，部分应用的设备绑定/风控状态会失效。")
        }
    }
}

// MARK: 应用库

struct AppLibraryPage: View {
    @ObservedObject var api: MWineAPI
    var install: () -> Void
    @Binding var page: Int
    @State private var search = ""
    @State private var uninstallingApp: ShellApp?

    let columns = [GridItem(.adaptive(minimum: 104), spacing: 16)]

    var filtered: [ShellApp] {
        guard !search.isEmpty else { return api.shells }
        return api.shells.filter { $0.name.localizedCaseInsensitiveContains(search) }
    }

    var body: some View {
        content
            .alert("卸载应用", isPresented: Binding(
                get: { uninstallingApp != nil },
                set: { if !$0 { uninstallingApp = nil } })) {
                Button("取消", role: .cancel) { uninstallingApp = nil }
                Button("卸载", role: .destructive) {
                    if let a = uninstallingApp { api.uninstall(app: a) }
                }
            } message: {
                Text("将删除桌面壳并卸载 \(uninstallingApp?.name ?? "")，其在虚拟机内的应用数据将一并丢失。")
            }
    }

    private var content: some View {
        Group {
            if api.shells.isEmpty {
                VStack(spacing: 12) {
                    Image(systemName: "square.grid.3x3").font(.system(size: 40)).foregroundStyle(.tertiary)
                    Text("应用库为空").font(.headline)
                    Text("拖放 APK 到本窗口，或点「安装 APK」").font(.subheadline).foregroundStyle(.secondary)
                    Button("安装 APK", action: install).buttonStyle(.borderedProminent)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                VStack(spacing: 0) {
                    HStack {
                        Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                        TextField("搜索应用", text: $search)
                            .textFieldStyle(.plain)
                        if !search.isEmpty {
                            Button { search = "" } label: { Image(systemName: "xmark.circle.fill") }
                                .buttonStyle(.borderless).foregroundStyle(.tertiary)
                        }
                    }
                    .padding(8).padding(.horizontal, 6)
                    .background(RoundedRectangle(cornerRadius: 8).fill(.quinary))
                    .padding(.horizontal, 14).padding(.top, 10)

                    ScrollView {
                        LazyVGrid(columns: columns, spacing: 18) {
                            ForEach(filtered) { app in
                                ShellIcon(app: app, installed: api.isInstalled(app))
                                    .onTapGesture { api.launchShell(app) }
                                    .contextMenu {
                                        Button("打开") { api.launchShell(app) }
                                        Divider()
                                        Button("卸载…", role: .destructive) {
                                            uninstallingApp = app
                                        }
                                    }
                            }
                        }
                        .padding()
                    }
                }
            }
        }
    }
}

struct ShellIcon: View {
    let app: ShellApp
    var installed: Bool = true
    @State private var hovered = false

    var body: some View {
        VStack(spacing: 5) {
            ZStack(alignment: .topTrailing) {
                Image(nsImage: app.icon)
                    .resizable()
                    .frame(width: 54, height: 54)
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                if !installed {
                    Text("未装")
                        .font(.system(size: 8, weight: .bold))
                        .padding(3)
                        .background(Capsule().fill(.red.opacity(0.85)))
                        .foregroundStyle(.white)
                        .offset(x: -4, y: 4)
                }
            }
            .scaleEffect(hovered ? 1.05 : 1)
            Text(app.name)
                .font(.system(size: 11))
                .lineLimit(1).frame(maxWidth: 96)
        }
        .opacity(installed ? 1 : 0.55)
        .onHover { hovered = $0 }
        .contentShape(Rectangle())
    }
}

// MARK: 按键映射

struct KeymapPage: View {
    @ObservedObject var api: MWineAPI
    @State private var pkg = ""
    @State private var bindings: [KeyBinding] = []
    @State private var screenshot: NSImage?
    @State private var imgSize: CGSize = CGSize(width: 1080, height: 2400)
    @State private var capturingKeyFor: UUID?
    @State private var monitorInstalled = false

    var body: some View {
        HStack(spacing: 0) {
            VStack(spacing: 8) {
                Picker("应用", selection: $pkg) {
                    if pkg.isEmpty { Text("选择应用…").tag("") }
                    ForEach(api.apps) { a in Text(a.name).tag(a.package) }
                }
                .frame(maxWidth: 300)

                if let img = screenshot {
                    GeometryReader { geo in
                        ZStack {
                            Image(nsImage: img)
                                .resizable().aspectRatio(contentMode: .fit)
                                .frame(width: geo.size.width, height: geo.size.height)
                            ForEach(bindings) { b in
                                BindingPin(b: b, capturing: capturingKeyFor == b.id)
                                    .position(pos(b, in: geo.size))
                            }
                        }
                        .background(Color.black.opacity(0.04))
                        .contentShape(Rectangle())
                        .gesture(
                            DragGesture(minimumDistance: 0).onEnded { v in
                                guard !pkg.isEmpty else { return }
                                let norm = toNorm(v.location, in: geo.size)
                                let nb = KeyBinding(key: "?", type: "tap",
                                                    x: norm.x, y: norm.y,
                                                    x2: nil, y2: nil, duration: 300, interval: 120)
                                bindings.append(nb)
                                capturingKeyFor = nb.id
                                installKeyMonitor()
                            }
                        )
                    }
                    Text("点击截图添加触点 → 点右侧按键框后按物理键绑定")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    VStack(spacing: 10) {
                        Image(systemName: "keyboard").font(.system(size: 36)).foregroundStyle(.tertiary)
                        Text(api.vmRunning ? "选择应用后截取屏幕开始编辑"
                                            : "启动 Android 后可编辑按键").font(.subheadline)
                            .foregroundStyle(.secondary)
                        Button("截取当前屏幕") { loadShot() }
                            .buttonStyle(.bordered)
                            .disabled(!api.vmRunning || pkg.isEmpty)
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .padding(14)
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            Divider()

            VStack(spacing: 0) {
                HStack {
                    Text("绑定（\(bindings.count)）").font(.headline)
                    Spacer()
                    if api.keywatcherRunning {
                        Button("停止映射") { api.stopKeywatcher() }
                    } else {
                        Button("启用映射") { api.startKeywatcher(pkg: pkg) }
                            .disabled(pkg.isEmpty || bindings.isEmpty)
                    }
                }
                .padding(10)

                List {
                    ForEach($bindings) { $b in
                        HStack(spacing: 8) {
                            KeyField(b: $b, capturing: capturingKeyFor == b.id) {
                                capturingKeyFor = b.id
                                installKeyMonitor()
                            }
                            Picker("", selection: $b.type) {
                                Text("点按").tag("tap")
                                Text("连点").tag("hold")
                                Text("滑动").tag("swipe")
                            }
                            .frame(width: 80).labelsHidden()
                            Text(String(format: "%.0f,%.0f", b.x, b.y))
                                .font(.system(size: 10, design: .monospaced))
                                .foregroundStyle(.secondary)
                            Spacer()
                            Button(role: .destructive) {
                                bindings.removeAll { $0.id == b.id }
                            } label: { Image(systemName: "trash") }
                                .buttonStyle(.borderless)
                        }
                        .padding(.vertical, 2)
                    }
                }
                .listStyle(.plain)

                HStack {
                    Button("保存") { save() }
                        .buttonStyle(.borderedProminent)
                        .disabled(pkg.isEmpty)
                    Button("重新截图") { loadShot() }
                        .disabled(!api.vmRunning || pkg.isEmpty)
                    Spacer()
                    Text("首次启用需授权「辅助功能」")
                        .font(.caption2).foregroundStyle(.tertiary)
                }
                .padding(10)
            }
            .frame(width: 330)
        }
        .onAppear { if pkg.isEmpty { pkg = api.apps.first?.package ?? "" } }
        .onChange(of: pkg) { _ in loadKeys() }
        .onChange(of: api.apps) { _ in if pkg.isEmpty { pkg = api.apps.first?.package ?? "" } }
    }

    func pos(_ b: KeyBinding, in container: CGSize) -> CGPoint {
        let scale = min(container.width / imgSize.width, container.height / imgSize.height)
        let w = imgSize.width * scale, h = imgSize.height * scale
        let ox = (container.width - w) / 2, oy = (container.height - h) / 2
        return CGPoint(x: ox + b.x * scale, y: oy + b.y * scale)
    }

    func toNorm(_ p: CGPoint, in container: CGSize) -> CGPoint {
        let scale = min(container.width / imgSize.width, container.height / imgSize.height)
        let w = imgSize.width * scale, h = imgSize.height * scale
        let ox = (container.width - w) / 2, oy = (container.height - h) / 2
        return CGPoint(x: (p.x - ox) / scale, y: (p.y - oy) / scale)
    }

    func loadShot() {
        guard !pkg.isEmpty else { return }
        let enc = pkg.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? pkg
        guard let u = URL(string: "http://127.0.0.1:7788/api/keys/\(enc)/shot") else { return }
        var req = URLRequest(url: u); req.httpMethod = "POST"
        URLSession.shared.dataTask(with: req) { data, _, _ in
            guard let d = data,
                  let v = try? JSONDecoder().decode([String: String].self, from: d),
                  let path = v["path"] else { return }
            DispatchQueue.main.async {
                if let img = NSImage(contentsOfFile: path) {
                    self.screenshot = img
                    self.imgSize = img.size
                }
            }
        }.resume()
    }

    func loadKeys() {
        guard !pkg.isEmpty else { return }
        let enc = pkg.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? pkg
        guard let u = URL(string: "http://127.0.0.1:7788/api/keys/\(enc)") else { return }
        URLSession.shared.dataTask(with: u) { data, _, _ in
            guard let d = data,
                  let r = try? JSONDecoder().decode(KeysResp.self, from: d) else { return }
            DispatchQueue.main.async {
                self.bindings = r.bindings
            }
        }.resume()
        loadShot()
    }

    func save() {
        let enc = pkg.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? pkg
        guard let u = URL(string: "http://127.0.0.1:7788/api/keys/\(enc)") else { return }
        var req = URLRequest(url: u)
        req.httpMethod = "PUT"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        let payload: [[String: Any?]] = bindings.map {
            ["key": $0.key, "type": $0.type, "x": $0.x, "y": $0.y,
             "x2": $0.x2, "y2": $0.y2, "duration": $0.duration, "interval": $0.interval]
        }
        req.httpBody = try? JSONSerialization.data(withJSONObject: payload)
        URLSession.shared.dataTask(with: req) { _, resp, _ in
            let ok = (resp as? HTTPURLResponse)?.statusCode == 200
            DispatchQueue.main.async {
                api.message = ok ? "✓ 键位已保存（点「启用映射」生效）" : "✗ 保存失败"
            }
        }.resume()
    }

    func installKeyMonitor() {
        guard !monitorInstalled else { return }
        monitorInstalled = true
        NSEvent.addLocalMonitorForEvents(matching: [.keyDown]) { event in
            if let target = self.capturingKeyFor,
               let idx = self.bindings.firstIndex(where: { $0.id == target }) {
                self.bindings[idx].key = Self.keyName(event)
                self.capturingKeyFor = nil
                return nil
            }
            return event
        }
    }

    static func keyName(_ e: NSEvent) -> String {
        let special: [UInt16: String] = [36: "return", 48: "tab", 49: "space", 51: "delete", 53: "escape",
                                         123: "left", 124: "right", 125: "down", 126: "up"]
        if let s = special[e.keyCode] { return s }
        return e.charactersIgnoringModifiers?.lowercased() ?? "k\(e.keyCode)"
    }
}

struct KeyField: View {
    @Binding var b: KeyBinding
    let capturing: Bool
    let onStart: () -> Void

    var body: some View {
        Group {
            if capturing {
                Text("按键…").font(.caption).foregroundStyle(.orange)
                    .frame(width: 56)
            } else {
                Text(b.key.isEmpty ? "未绑定" : b.key)
                    .font(.system(size: 12, weight: .semibold, design: .monospaced))
                    .frame(width: 56)
                    .padding(3)
                    .background(RoundedRectangle(cornerRadius: 5).fill(.quaternary))
            }
        }
        .onTapGesture { onStart() }
    }
}

struct BindingPin: View {
    let b: KeyBinding
    let capturing: Bool
    @State private var hovered = false

    var body: some View {
        ZStack {
            Circle()
                .fill(hovered || capturing ? Color.orange : Color.blue)
                .opacity(0.85)
                .frame(width: 22, height: 22)
            Text(b.key)
                .font(.system(size: 10, weight: .bold, design: .monospaced))
                .foregroundStyle(.white)
        }
        .onHover { hovered = $0 }
    }
}

// MARK: 镜像
/// UI 构建号（每次发版手改；镜像页底部可见）——远程排障先对版本，再谈现象
let uiBuildStamp = "20261007-1320"

struct ImagePage: View {
    @ObservedObject var api: MWineAPI
    @State private var officialFilter = "android-"
    @State private var showImportPicker = false
    @AppStorage("imageProxy") private var imageProxy = ""
    @State private var deletingImage: InstalledImage?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                if !api.imageBanner.isEmpty {
                    HStack(spacing: 8) {
                        Image(systemName: api.imageBannerOk ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                            .foregroundStyle(api.imageBannerOk ? .green : .orange)
                        Text(api.imageBanner).font(.callout)
                        Spacer()
                    }
                    .padding(10)
                    .background((api.imageBannerOk ? Color.green : Color.orange).opacity(0.1),
                                in: RoundedRectangle(cornerRadius: 8))
                }
                if api.imageSwitching {
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text("正在切换镜像（关停虚拟机 → 切换 → 重启）…").font(.callout).foregroundStyle(.secondary)
                    }
                    .padding(10)
                    .background(.quaternary.opacity(0.3), in: RoundedRectangle(cornerRadius: 8))
                }
                if let t = api.imageTask, t.running == true || (t.done != true && t.error?.isEmpty != true && t.lines?.isEmpty != true) {
                    imageTaskCard(t)
                }

                installedSection
                importSection
                officialSection
                Text("Houttuynia UI \(uiBuildStamp)")
                    .font(.caption2).foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .padding(.top, 4)
            }
            .padding(24)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear {
            api.loadInstalledImages()
            if api.officialImages.isEmpty { api.loadOfficialImages(filter: "android-", proxy: imageProxy) }
        }
        .sheet(isPresented: $showImportPicker) { ImportPickerSheet { url in
            api.importImage(path: url.path)
        } }
        .alert("切换镜像", isPresented: Binding(
            get: { api.pendingUse != nil },
            set: { if !$0 { api.pendingUse = nil } })) {
            Button("取消", role: .cancel) { api.pendingUse = nil }
            Button("清空并切换", role: .destructive) {
                if let img = api.pendingUse {
                    api.useImage(img, stopVm: api.vmRunning)
                }
            }
        } message: {
            Text("切换到 \(api.pendingUse?.display ?? "") \(api.pendingUse?.name ?? "") 将清空虚拟机内的全部应用与数据（不同镜像的系统数据不兼容，无法保留）。\(api.vmRunning ? "虚拟机将关停并以新镜像重启。" : "下次启动将以新镜像运行。")")
        }
        .alert("删除镜像", isPresented: Binding(
            get: { deletingImage != nil },
            set: { if !$0 { deletingImage = nil } })) {
            Button("取消", role: .cancel) {}
            Button("删除", role: .destructive) {
                if let img = deletingImage { api.deleteImage(img) }
            }
        } message: {
            Text("将删除 \(deletingImage?.name ?? "")（约 \(deletingImage?.sizeMb ?? 0) MB）。此操作不可恢复。")
        }
    }

    private func fmtSize(_ mb: UInt) -> String {
        mb >= 1024 ? String(format: "%.1f GB", Double(mb) / 1024) : "\(mb) MB"
    }

    private func taskTitle(_ t: ImageTask) -> String {
        if t.running == true { return "任务进行中" }
        if t.error == "已取消" { return "已取消" }
        if t.error?.isEmpty == false { return "任务失败" }
        return "任务完成"
    }

    private func imageTaskCard(_ t: ImageTask) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                if t.running == true && t.progressTotal == 0 { ProgressView().controlSize(.small) }
                Text(taskTitle(t))
                    .font(.headline)
                    .foregroundStyle(t.error == "已取消" ? AnyShapeStyle(.secondary) : AnyShapeStyle(.primary))
                Spacer()
                if t.running == true {
                    Button("取消") { api.cancelImageTask() }
                } else {
                    Button("关闭") { api.imageTask = nil }
                }
            }
            if t.running == true {
                progressSection(t)
            }
            if let err = t.error, !err.isEmpty {
                Text(err)
                    .foregroundStyle(err == "已取消" ? AnyShapeStyle(.secondary) : AnyShapeStyle(.red))
                    .font(.caption)
            }
            if let r = t.result, !r.isEmpty {
                Text("已安装：\(r)").font(.caption).foregroundStyle(.green)
            }
            if let lines = t.lines, lines.count > 1 {
                // 折叠日志：默认只看最近 3 行，可展开
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(showFullLog ? lines : lines.suffix(3), id: \.self) { l in
                        Text(l).font(.system(size: 10, design: .monospaced))
                            .foregroundStyle(.secondary).lineLimit(1)
                    }
                    if lines.count > 3 {
                        Button(showFullLog ? "收起日志" : "展开全部日志（\(lines.count) 行）") {
                            withAnimation { showFullLog.toggle() }
                        }
                        .font(.caption2).controlSize(.small)
                    }
                }
            }
        }
        .padding(12)
        .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 10))
    }

    @State private var showFullLog = false

    @State private var lastSample: (bytes: UInt64, at: Date)? = nil

    /// 结构化进度区：有总量→百分比条+速度；无总量→转圈+阶段名
    private func progressSection(_ t: ImageTask) -> some View {
        let phase = t.phase ?? ""
        let done = t.progress ?? 0
        let total = t.progressTotal ?? 0
        // 相邻轮询差值 → 速度（只在下载类阶段显示）
        var speed = ""
        if let s = lastSample, done > s.bytes {
            let dt = Date().timeIntervalSince(s.at)
            if dt > 0.3, done > s.bytes {
                let mbps = Double(done - s.bytes) / 1048576 / dt
                if mbps > 0.1 { speed = String(format: "  %.1f MiB/s", mbps) }
            }
        }
        lastSample = (done, Date())
        return VStack(alignment: .leading, spacing: 4) {
            if total > 0 {
                ProgressView(value: min(Double(done) / Double(total), 1))
                HStack {
                    Text(phase).font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Text("\(done / 1048576) / \(total / 1048576) MiB（\(Int(min(Double(done) / Double(total), 1) * 100))%）\(speed)")
                        .font(.caption).foregroundStyle(.secondary).monospacedDigit()
                }
            } else if !phase.isEmpty {
                HStack(spacing: 6) {
                    ProgressView().controlSize(.small)
                    Text(phase).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    private var installedSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("已安装镜像").font(.title3.bold())
            if api.installedImages.isEmpty {
                Text("暂无").foregroundStyle(.secondary)
            } else {
                ForEach(api.installedImages) { img in
                    HStack {
                        Image(systemName: "internaldrive")
                            .foregroundStyle(.blue)
                        VStack(alignment: .leading) {
                            HStack(spacing: 6) {
                                Text(img.display ?? img.api).font(.body.weight(.medium))
                                Text(img.name).font(.caption).foregroundStyle(.secondary)
                                if img.inUse == true {
                                    Text("使用中").font(.caption2).padding(.horizontal, 5).padding(.vertical, 1)
                                        .background(.green.opacity(0.2), in: Capsule())
                                        .foregroundStyle(.green)
                                }
                            }
                            Text(img.path).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                        }
                        Spacer()
                        Text(fmtSize(img.sizeMb ?? 0)).font(.caption).foregroundStyle(.secondary)
                        if img.inUse == true {
                            Label("使用中", systemImage: "checkmark.circle.fill")
                                .font(.caption).foregroundStyle(.green).labelStyle(.titleAndIcon)
                        } else {
                            Button("使用") { api.pendingUse = img }
                                .controlSize(.small)
                                .disabled(api.imageSwitching || api.imageTask?.running == true)
                            Button("删除", role: .destructive) { deletingImage = img }
                                .controlSize(.small)
                                .disabled(api.imageSwitching || api.imageTask?.running == true)
                        }
                    }
                    .padding(.vertical, 6).padding(.horizontal, 6)
                    .background(img.inUse == true ? Color.green.opacity(0.07) : .clear,
                                in: RoundedRectangle(cornerRadius: 6))
                    if img.inUse != true { Divider() }
                }
            }
        }
    }

    private var importSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("导入分发包").font(.title3.bold())
            Text("选择 houttuynia-image-*.tar.zst 分发包（打包者经任意渠道分享）。导入时自动校验、压缩落盘。")
                .font(.caption).foregroundStyle(.secondary)
            Button {
                showImportPicker = true
            } label: {
                Label("选择分发包导入…", systemImage: "square.and.arrow.down")
            }
            .controlSize(.large)
        }
    }

    private var officialSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Google 官方镜像").font(.title3.bold())
                Spacer()
                if api.officialLoading { ProgressView().controlSize(.small) }
            }
            HStack {
                TextField("过滤：16 或 android-36", text: $officialFilter)
                    .textFieldStyle(.roundedBorder).frame(width: 180)
                Button("刷新") { api.loadOfficialImages(filter: officialFilter, proxy: imageProxy) }
            }
            HStack {
                Image(systemName: "network")
                TextField("代理（可选，如 http://127.0.0.1:7890）", text: $imageProxy)
                    .textFieldStyle(.roundedBorder)
            }
            if api.officialImages.isEmpty {
                Text("无匹配（dl.google.com 不可达时需配代理）").foregroundStyle(.secondary).font(.caption)
            }
            ForEach(api.officialImages.prefix(30)) { img in
                HStack {
                    VStack(alignment: .leading) {
                        HStack(spacing: 6) {
                            Text(img.display ?? { let segs = img.path.components(separatedBy: ";"); return segs.count > 1 ? segs[1] : img.path }())
                                .font(.body.weight(.medium))
                            let segs = img.path.components(separatedBy: ";")
                            Text(segs.count > 2 ? segs[2] : "").font(.caption).foregroundStyle(.secondary)
                        }
                        Text("\((img.size ?? 0) / 1048576) MB · \(img.path)")
                            .font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    }
                    Spacer()
                    Button("拉取") { api.pullOfficial(img.path, proxy: imageProxy) }
                        .controlSize(.small)
                        .disabled(api.imageTask?.running == true)
                }
                .padding(.vertical, 4)
                Divider()
            }
        }
    }
}

struct ImportPickerSheet: View {
    var onPick: (URL) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var url: URL?

    var body: some View {
        VStack(spacing: 16) {
            Text("选择镜像分发包").font(.headline)
            Text(url?.lastPathComponent ?? "尚未选择").font(.caption).foregroundStyle(.secondary)
            HStack {
                Button("浏览…") {
                    let panel = NSOpenPanel()
                    panel.allowedContentTypes = [.init(filenameExtension: "zst") ?? .archive, .data]
                    panel.allowsMultipleSelection = false
                    panel.canChooseDirectories = false
                    if panel.runModal() == .OK, let u = panel.url { url = u }
                }
                Spacer()
                Button("取消") { dismiss() }
                Button("导入") {
                    if let u = url { onPick(u); dismiss() }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(url == nil)
            }
        }
        .padding(20)
        .frame(width: 420)
    }
}

// MARK: 性能

struct PerfPage: View {
    @ObservedObject var api: MWineAPI

    var body: some View {
        ScrollView {
            VStack(spacing: 14) {
                if api.vmRunning {
                    HStack(spacing: 12) {
                        GaugeCard(title: "CPU", value: Double(api.perf.cpuPct ?? 0), unit: "%",
                                  m: 100, color: .blue)
                        GaugeCard(title: "内存占用", value: usedRatio, unit: "%",
                                  m: 100, color: .green)
                        GaugeCard(title: "宿主 CPU", value: api.perf.hostQemuCpu ?? 0, unit: "%",
                                  m: 400, color: .orange)
                    }
                    HStack(spacing: 12) {
                        InfoRow(icon: "clock", label: "运行时长",
                                value: formatUptime(api.perf.uptimeS ?? 0))
                        InfoRow(icon: "memorychip", label: "可用内存",
                                value: "\(api.perf.memAvailMb ?? 0) / \(api.perf.memTotalMb ?? 0) MB")
                        InfoRow(icon: "cpu", label: "宿主内存",
                                value: "\(api.perf.hostQemuMemMb ?? 0) MB")
                    }
                    HistoryChart(title: "CPU（3 分钟）", data: api.cpuHistory, color: .blue)
                    HistoryChart(title: "内存占用（3 分钟）", data: api.memHistory, color: .green)
                    Text("每 2 秒自动刷新 · CPU 为差分采样（首刷 0 属正常）")
                        .font(.caption).foregroundStyle(.tertiary)
                } else {
                    VStack(spacing: 10) {
                        Image(systemName: "chart.bar").font(.system(size: 36)).foregroundStyle(.tertiary)
                        Text("Android 未运行").font(.subheadline).foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, minHeight: 200)
                }
            }
            .padding(20)
        }
    }

    var usedRatio: Double {
        guard let t = api.perf.memTotalMb, t > 0, let a = api.perf.memAvailMb else { return 0 }
        return Double(t - a) / Double(t) * 100
    }
}

struct GaugeCard: View {
    let title: String; let value: Double; let unit: String; let m: Double; let color: Color
    var body: some View {
        VStack(spacing: 6) {
            Gauge(value: Swift.min(Swift.max(value, 0), m), in: 0...m) {
                Text(title)
            } currentValueLabel: {
                Text(String(format: "%.0f\(unit)", value))
                    .font(.system(size: 13, weight: .semibold, design: .monospaced))
            }
            .gaugeStyle(.accessoryCircular)
            .tint(color)
            Text(title).font(.caption).foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(14)
        .background(RoundedRectangle(cornerRadius: 12).fill(.background).shadow(radius: 1))
    }
}

struct HistoryChart: View {
    let title: String
    let data: [Double]
    let color: Color

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title).font(.caption).foregroundStyle(.secondary)
                Spacer()
                Text(data.last.map { String(format: "%.0f%%", $0) } ?? "--")
                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
            }
            GeometryReader { geo in
                if data.count >= 2 {
                    let maxV = max(data.max() ?? 1, 10)
                    Path { p in
                        for (i, v) in data.enumerated() {
                            let x = geo.size.width * CGFloat(i) / CGFloat(data.count - 1)
                            let y = geo.size.height * (1 - CGFloat(min(v, maxV)) / CGFloat(maxV))
                            if i == 0 { p.move(to: CGPoint(x: x, y: y)) }
                            else { p.addLine(to: CGPoint(x: x, y: y)) }
                        }
                    }
                    .stroke(color, style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                } else {
                    Text("采样中…").font(.caption2).foregroundStyle(.tertiary)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            .frame(height: 56)
            .background(RoundedRectangle(cornerRadius: 6).fill(.quinary))
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 10).fill(.background).shadow(radius: 1))
    }
}

struct InfoRow: View {
    let icon: String; let label: String; let value: String
    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: icon).foregroundStyle(.tint)
            VStack(alignment: .leading, spacing: 1) {
                Text(label).font(.caption2).foregroundStyle(.secondary)
                Text(value).font(.system(size: 13, weight: .medium)).monospacedDigit()
            }
            Spacer()
        }
        .padding(10)
        .frame(maxWidth: .infinity)
        .background(RoundedRectangle(cornerRadius: 10).fill(.background).shadow(radius: 1))
    }
}

// MARK: - 入口

@main
struct MWineApp: App {
    var body: some Scene {
        WindowGroup { MainWindow() }
    }
}
