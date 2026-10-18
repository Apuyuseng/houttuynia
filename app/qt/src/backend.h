// QML 后端：对 mwine-cored 守护进程 HTTP API 的异步封装 + CLI 进程集成。
// 设计约定：
//  - 所有属性只读 + NOTIFY，QML 声明式绑定；写操作走 Q_INVOKABLE
//  - 长任务（镜像/运行时/商店下载）统一轮询 /api/*/task，由 m_taskTimer 驱动
//  - 重轮询页（性能/流量）按页可见性开关（setPerfActive/setCaptureActive）
//  - 守护进程不可达时自动拉起（bundle 内置 mwine-cored 或开发目录）
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVariantList>

class QQmlEngine;
class QTranslator;

class Backend : public QObject {
    Q_OBJECT

    // ===== 连接 / 系统 =====
    Q_PROPERTY(bool daemonOnline READ daemonOnline NOTIFY statusChanged)
    Q_PROPERTY(bool darkMode READ darkMode NOTIFY darkModeChanged)
    Q_PROPERTY(QString language READ language NOTIFY languageChanged)
    Q_PROPERTY(bool vmRunning READ vmRunning NOTIFY statusChanged)
    Q_PROPERTY(QString vmSerial READ vmSerial NOTIFY statusChanged)
    Q_PROPERTY(QString vmError READ vmError NOTIFY statusChanged)
    Q_PROPERTY(bool vmBooting READ vmBooting NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged) // 安装/CLI 进行中

    // ===== 应用 =====
    Q_PROPERTY(QJsonArray apps READ apps NOTIFY appsChanged)
    Q_PROPERTY(QString appWallpaper READ appWallpaper NOTIFY appsChanged)
    Q_PROPERTY(bool vmDarkMode READ vmDarkMode NOTIFY appsChanged)
    Q_PROPERTY(int appCount READ appCount NOTIFY appsChanged)
    Q_PROPERTY(QString foregroundPkg READ foregroundPkg NOTIFY foregroundChanged)

    // ===== 镜像 / 运行时 =====
    Q_PROPERTY(QJsonArray images READ images NOTIFY imagesChanged)
    Q_PROPERTY(QJsonObject imageTask READ imageTask NOTIFY imageTaskChanged)
    Q_PROPERTY(QJsonArray officialImages READ officialImages NOTIFY officialImagesChanged)
    Q_PROPERTY(bool officialLoading READ officialLoading NOTIFY officialImagesChanged)
    Q_PROPERTY(QJsonObject runtimeTask READ runtimeTask NOTIFY runtimeTaskChanged)
    Q_PROPERTY(bool runtimePresent READ runtimePresent NOTIFY runtimeChanged)
    Q_PROPERTY(bool sdkAvailable READ sdkAvailable NOTIFY runtimeChanged)
    // 运行时状态是否已确认（首启引导只在确认缺失后弹，防启动早期误弹）
    Q_PROPERTY(bool runtimeKnown READ runtimeKnown NOTIFY runtimeChanged)

    // ===== 配置 / 磁盘 =====
    Q_PROPERTY(QJsonObject config READ config NOTIFY configChanged)
    Q_PROPERTY(int diskGB READ diskGB NOTIFY diskChanged)
    Q_INVOKABLE void saveConfig(const QJsonObject &cfg);

    // ===== 性能 =====
    Q_PROPERTY(QJsonObject perf READ perf NOTIFY perfChanged)
    Q_PROPERTY(QVariantList perfHistory READ perfHistory NOTIFY perfChanged)

    // ===== 终端 =====
    Q_PROPERTY(QString terminalOutput READ terminalOutput NOTIFY terminalOutputChanged)
    Q_PROPERTY(bool terminalConnected READ terminalConnected NOTIFY terminalChanged)

    // ===== 在线商店 =====
    Q_PROPERTY(QJsonArray storeSources READ storeSources NOTIFY storeChanged)
    Q_PROPERTY(QJsonArray storeResults READ storeResults NOTIFY storeChanged)
    Q_PROPERTY(bool storeSearching READ storeSearching NOTIFY storeChanged)
    Q_PROPERTY(QJsonArray storeRecommended READ storeRecommended NOTIFY storeChanged)
    Q_PROPERTY(bool storeRecommendLoading READ storeRecommendLoading NOTIFY storeChanged)
    Q_PROPERTY(QString storeRecommendError READ storeRecommendError NOTIFY storeChanged)
    Q_PROPERTY(QJsonObject storeApp READ storeApp NOTIFY storeChanged)
    Q_PROPERTY(QJsonArray storeVersions READ storeVersions NOTIFY storeChanged)
    Q_PROPERTY(bool storeVersionsLoading READ storeVersionsLoading NOTIFY storeChanged)
    Q_PROPERTY(double storeInstalledCode READ storeInstalledCode NOTIFY storeChanged)
    Q_PROPERTY(QJsonObject storeTask READ storeTask NOTIFY storeTaskChanged)
    Q_PROPERTY(QJsonObject installTask READ installTask NOTIFY installTaskChanged)
    Q_PROPERTY(bool appsError READ appsError NOTIFY appsChanged)
    Q_PROPERTY(bool keepApk READ keepApk WRITE setKeepApk NOTIFY storeChanged)

    // ===== 按键映射 =====
    Q_PROPERTY(QJsonArray keyProfiles READ keyProfiles NOTIFY keysChanged)
    Q_PROPERTY(QJsonArray keyTemplates READ keyTemplates NOTIFY keysChanged)
    Q_PROPERTY(bool keywatcherRunning READ keywatcherRunning NOTIFY keysChanged)
    Q_PROPERTY(QString keywatcherPkg READ keywatcherPkg NOTIFY keysChanged)
    Q_PROPERTY(QString keyShotPath READ keyShotPath NOTIFY keysChanged)

    // ===== 快照 =====
    Q_PROPERTY(QJsonArray snapshots READ snapshots NOTIFY snapshotsChanged)

    // ===== 真机连接 =====
    Q_PROPERTY(QJsonArray deviceList READ deviceList NOTIFY devicesChanged)
    Q_PROPERTY(QJsonArray deviceDiscovered READ deviceDiscovered NOTIFY devicesChanged)
    Q_PROPERTY(bool devicePairing READ devicePairing NOTIFY devicesChanged)
    // 出码配对会话（Android Studio 同款：Mac 出二维码、手机扫码连过来）
    Q_PROPERTY(QJsonObject deviceHostPair READ deviceHostPair NOTIFY deviceHostPairChanged)

    // ===== 流量分析 =====
    Q_PROPERTY(QJsonObject captureStatus READ captureStatus NOTIFY captureChanged)
    Q_PROPERTY(QJsonArray captureFlows READ captureFlows NOTIFY captureChanged)
    Q_PROPERTY(int captureTotal READ captureTotal NOTIFY captureChanged)
    Q_PROPERTY(QJsonObject captureDetail READ captureDetail NOTIFY captureChanged)

public:
    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    // 退出主程序收尾：自拉 daemon 时存档关停 VM（阻塞至完成/30s 超时）；
    // aboutToQuit 里调用（main.cpp）
    void shutdownForQuit();

    // ===== 属性 =====
    bool daemonOnline() const { return m_daemonOnline; }
    bool darkMode() const { return m_darkMode; }
    QString language() const { return m_language; }
    bool vmRunning() const { return m_vmRunning; }
    QString vmSerial() const { return m_vmSerial; }
    QString vmError() const { return m_vmError; }
    bool vmBooting() const { return m_vmBooting; }
    bool busy() const { return m_busyCount > 0; }
    QJsonArray apps() const { return m_apps; }
    QString appWallpaper() const { return m_appWallpaper; }
    bool vmDarkMode() const { return m_vmDark; }
    int appCount() const { return static_cast<int>(m_apps.size()); }
    QString foregroundPkg() const { return m_foregroundPkg; }
    QJsonArray images() const { return m_images; }
    QJsonObject imageTask() const { return m_imageTask; }
    QJsonArray officialImages() const { return m_officialImages; }
    bool officialLoading() const { return m_officialLoading; }
    QJsonObject runtimeTask() const { return m_runtimeTask; }
    bool runtimePresent() const { return m_runtimePresent; }
    bool sdkAvailable() const { return m_sdkAvailable; }
    bool runtimeKnown() const { return m_runtimeKnown; }
    QJsonObject config() const { return m_config; }
    int diskGB() const { return m_diskGB; }
    QJsonObject perf() const { return m_perf; }
    QVariantList perfHistory() const { return m_perfHistory; }
    QString terminalOutput() const { return m_terminalOutput; }
    bool terminalConnected() const { return m_terminalConnected; }
    QJsonArray storeSources() const { return m_storeSources; }
    QJsonArray storeResults() const { return m_storeResults; }
    bool storeSearching() const { return m_storeSearching; }
    QJsonArray storeRecommended() const { return m_storeRecommended; }
    bool storeRecommendLoading() const { return m_storeRecommendLoading; }
    QString storeRecommendError() const { return m_storeRecommendError; }
    QJsonObject storeApp() const { return m_storeApp; }
    QJsonArray storeVersions() const { return m_storeVersions; }
    bool storeVersionsLoading() const { return m_storeVersionsLoading; }
    double storeInstalledCode() const { return m_storeInstalledCode; }
    QJsonObject storeTask() const { return m_storeTask; }
    QJsonObject installTask() const { return m_installTask; }
    bool appsError() const { return m_appsError; }
    bool keepApk() const { return m_keepApk; }
    void setKeepApk(bool v);
    QJsonArray deviceList() const { return m_deviceList; }
    QJsonArray deviceDiscovered() const { return m_deviceDiscovered; }
    bool devicePairing() const { return m_devicePairing; }
    QJsonObject deviceHostPair() const { return m_deviceHostPair; }
    QJsonArray keyProfiles() const { return m_keyProfiles; }
    QJsonArray keyTemplates() const { return m_keyTemplates; }
    bool keywatcherRunning() const { return m_keywatcher != nullptr && m_keywatcherRunning; }
    QString keywatcherPkg() const { return m_keywatcherPkg; }
    QString keyShotPath() const { return m_keyShotPath; }
    QJsonArray snapshots() const { return m_snapshots; }
    QJsonObject captureStatus() const { return m_captureStatus; }
    QJsonArray captureFlows() const { return m_captureFlows; }
    int captureTotal() const { return m_captureTotal; }
    QJsonObject captureDetail() const { return m_captureDetail; }

    // ===== VM 控制 =====
    Q_INVOKABLE void startVm();
    Q_INVOKABLE void showVmWindow();      // 显示 emulator 主窗口（unhide+置前）
    Q_INVOKABLE void stopVm();
    Q_INVOKABLE void screenshot();          // 截屏存桌面
    Q_INVOKABLE void identityReset();       // 设备身份重置
    Q_INVOKABLE void vmReset();             // 清空虚拟机（危险）
    Q_INVOKABLE void resizeDisk(int gb);    // 磁盘扩容（409 时提示，二次调用带 stop_vm）

    // ===== 应用 =====
    Q_INVOKABLE void launchApp(const QString &pkg);
    Q_INVOKABLE void setAppMode(const QString &pkg, const QString &mode);
    Q_INVOKABLE void uninstallApp(const QString &pkg);
    Q_INVOKABLE void installApk(const QString &path, bool deleteOnSuccess = false);
    Q_INVOKABLE void installApkTo(const QString &path, const QString &serial); // 指定真机
    Q_INVOKABLE void retryInstall();    // 重试上次失败的安装（包还在原位置时）
    Q_INVOKABLE void refreshForeground();

    // ===== 在线商店 =====
    Q_INVOKABLE void storeSearch(const QString &q, const QString &source = {},
                                 const QString &mirror = {});
    Q_INVOKABLE void storeRecommend(const QString &source = QStringLiteral("fdroid"),
                                    const QString &mirror = {});
    Q_INVOKABLE void storeLoadVersions(const QJsonObject &app);
    Q_INVOKABLE void storeDownload(const QJsonObject &app, const QJsonObject &ver);
    Q_INVOKABLE void storeCancel();

    // 独立窗口几何记忆（key 区分不同窗口；QSettings 持久化）
    Q_INVOKABLE QVariantList winGeometry(const QString &key) const;
    Q_INVOKABLE void saveWinGeometry(const QString &key, int x, int y, int w, int h);

    // ===== 镜像 =====
    Q_INVOKABLE void importImage(const QString &path);
    // path 为镜像绝对路径（image/list 返回的 path 字段；daemon 校验 system-images 前缀）
    Q_INVOKABLE void useImage(const QString &path);
    Q_INVOKABLE void deleteImage(const QString &path);
    Q_INVOKABLE void imageCancelTask();
    Q_INVOKABLE void ackImageTask();       // 镜像任务错误"已读"（确认后关闭提示）
    Q_INVOKABLE void officialImagesLoad(const QString &filter = {});
    Q_INVOKABLE void officialPull(const QString &target);

    // ===== 运行时 =====
    Q_INVOKABLE void importRuntime(const QString &path);
    Q_INVOKABLE void runtimeDownload();   // 在线下载分发包（断点续传 → 自动安装）
    Q_INVOKABLE void runtimeCancel();     // 取消下载（已下载部分保留供续传）

    // ===== 快照 =====
    Q_INVOKABLE void snapshotSave(const QString &name);
    Q_INVOKABLE void snapshotLoad(const QString &name);
    Q_INVOKABLE void snapshotDelete(const QString &name);

    // ===== 终端 =====
    Q_INVOKABLE void openTerminal();
    Q_INVOKABLE void closeTerminal();
    Q_INVOKABLE void terminalWrite(const QString &data);
    Q_INVOKABLE void terminalInterrupt();  // Ctrl-C
    Q_INVOKABLE void terminalClear();      // 清空本地显示缓冲

    // ===== 按键映射 =====
    Q_INVOKABLE void keysApplyTemplate(const QString &pkg, const QString &templateId);
    Q_INVOKABLE void keysDelete(const QString &pkg);
    Q_INVOKABLE void keysShot(const QString &pkg);
    Q_INVOKABLE void keywatchStart(const QString &pkg);
    Q_INVOKABLE void keywatchStop();

    // ===== 流量分析 =====
    Q_INVOKABLE void captureStart();
    Q_INVOKABLE void captureStop();
    Q_INVOKABLE void captureQueryFlows(const QString &q = {}, const QString &host = {},
                                       const QString &package = {});
    Q_INVOKABLE void captureLoadDetail(qint64 id);
    Q_INVOKABLE void captureClear();

    // ===== 页面级轮询开关 =====
    Q_INVOKABLE void setPerfActive(bool active);
    Q_INVOKABLE void setCaptureActive(bool active);

    // ===== 真机连接 =====
    Q_INVOKABLE void deviceRefresh();                                   // 立即刷新列表+发现
    Q_INVOKABLE void devicePairQr(const QString &qr);                   // 扫码内容直配（粘贴/脚本）
    Q_INVOKABLE void devicePairCode(const QString &code, const QString &addr = {},
                                    const QString &service = {});       // 6 位配对码
    Q_INVOKABLE void deviceConnect(const QString &addr);                // 手动 ip:端口
    Q_INVOKABLE void deviceDisconnect(const QString &serial);
    Q_INVOKABLE void deviceForget(const QString &serial);
    Q_INVOKABLE void deviceTcpip(const QString &serial);                // USB 一键转无线
    Q_INVOKABLE void deviceMirror(const QString &serial);               // 投屏窗口
    Q_INVOKABLE void deviceHostPairBegin();                             // 出码配对（显示二维码）
    Q_INVOKABLE void deviceHostPairCancel();                            // 取消等待扫码
    Q_INVOKABLE void setDevicesActive(bool active);                     // 页可见性轮询

    // ===== 杂项 =====
    Q_INVOKABLE void revealPath(const QString &path); // 系统文件管理器中定位
    Q_INVOKABLE QString cliPath();                    // 诊断用

    // ===== 国际化 =====
    // 绑定引擎（语言切换后触发 retranslate，QML 绑定即时重算）
    void attachEngine(QQmlEngine* engine) { m_engine = engine; }
    // "zh"（默认，无翻译器，源文即中文）| "en" | 未来语言
    Q_INVOKABLE void setLanguage(const QString& code);
    Q_INVOKABLE QStringList availableLanguages() const; // ["zh", "en", ...随 .qm 增]

signals:
    void statusChanged();
    void darkModeChanged();
    void languageChanged();
    void busyChanged();
    void appsChanged();
    void foregroundChanged();
    void imagesChanged();
    void imageTaskChanged();
    /// 镜像切换浮层（现代化提示）：真正执行切换（stop_vm）时开始/结束
    void imageSwitchStarted();
    void imageSwitchCancelled();
    void imageSwitchFinished(bool ok, const QString &error);
    void officialImagesChanged();
    void runtimeTaskChanged();
    void runtimeChanged();
    void configChanged();
    void diskChanged();
    void perfChanged();
    void terminalOutputChanged();
    void terminalChanged();
    void storeChanged();
    void storeTaskChanged();
    void installTaskChanged();
    void keysChanged();
    void snapshotsChanged();
    void captureChanged();
    void devicesChanged();
    void deviceHostPairChanged();
    void toast(const QString &message, const QString &type);
    /// 需要用户确认的危险操作（vmReset 等）；QML 弹确认框后调 confirm()
    void confirmationRequested(const QString &action, const QString &message);
    void confirmed(const QString &action);

private slots:
    void pollStatus();
    void pollTasks();
    void pollInstallTask();
    void pollPerf();
    void pollCapture();
    void pollTerminal();

private:
    // HTTP 基础
    void fetch(const QString &path, std::function<void(QJsonObject, int)> cb);
    void fetch(const QString &path, std::function<void(QJsonObject, int)> cb, int timeoutMs);
    void send(const QString &method, const QString &path, const QByteArray &body,
              std::function<void(QJsonObject, int)> cb);
    void send(const QString &method, const QString &path, const QByteArray &body,
              std::function<void(QJsonObject, int)> cb, int timeoutMs);
    void post(const QString &path, const QJsonObject &body,
              std::function<void(QJsonObject, int)> cb);
    void postEmpty(const QString &path, std::function<void(QJsonObject, int)> cb);
    void del(const QString &path, std::function<void(QJsonObject, int)> cb);

    // 刷新子集
    void refreshImages();
    void refreshKeys();
    void refreshSnapshots();
    void refreshDisk();
    void refreshApps();
    void refreshStoreSources();
    void devicePairFinish(const QJsonObject &obj, int code); // 配对结果统一反馈
    void refreshDiscovered(); // mDNS 发现按需拉取（子进程开销，不进周期轮询）
    void refreshHostPair();   // 出码配对会话状态轮询（随 deviceRefresh）

    // 镜像切换：stopVm=true 用于确认框放行后的重发（VM 运行中先关停再切）
    void postImageUse(const QString &path, bool stopVm);
    QString m_pendingUseImage;

    // CLI / 进程
    QString findProgram(const QString &name) const;
    void runCli(const QStringList &args, std::function<void(bool ok, const QString &tail)> cb);
    void maybeSpawnDaemon();
    void beginBusy();
    void endBusy();
    /// 确认流：dangerous action 先发 confirmationRequested，QML 调 confirmAction 后执行
    Q_INVOKABLE void confirmAction(const QString &action);

    // 网络与定时
    QNetworkAccessManager m_nam;
    QTimer m_slowTimer;   // 2s：状态/运行时
    QTimer m_lazyTimer;   // 6s：镜像/键位/快照/磁盘
    QTimer m_taskTimer;   // 1s：三类任务进度
    QTimer m_perfTimer;   // 2s：性能（页可见时）
    QTimer m_captureTimer; // 2s：流量（页可见时）
    QTimer m_deviceTimer; // 3s：真机列表/发现（页可见时）
    QTimer m_installTimer; // 1s：APK 安装任务进度
    QTimer m_termTimer;   // 400ms：终端读
    int m_tick = 0;
    bool m_anyTaskSeen = false;  // pollTasks 异步回调累计（悬垂规避）
    int m_taskIdleTicks = 0;

    // 连接 / VM
    bool m_daemonOnline = false;
    int m_offlineTicks = 0;   // 连续 /api/status 不可达拍数（≥3 才判离线并重启守护）
    bool m_darkMode = false;
    QString m_language = QStringLiteral("zh");
    QQmlEngine* m_engine = nullptr;
    QTranslator* m_translator = nullptr;
    bool m_vmRunning = false;
    QString m_vmSerial;
    QString m_vmError;
    bool m_vmBooting = false;
    int m_bootTicks = 0;      // 启动乐观态超时（2s/tick）
    QDateTime m_lastSpawnAttempt;
    QProcess *m_daemon = nullptr;

    // 应用
    QJsonArray m_apps;
    QString m_appWallpaper;
    bool m_vmDark = false;
    QString m_foregroundPkg;
    int m_busyCount = 0;

    // 镜像 / 运行时
    QJsonArray m_images;
    QJsonObject m_imageTask;
    QJsonArray m_officialImages;
    bool m_officialLoading = false;
    QJsonObject m_runtimeTask;
    bool m_runtimePresent = false;
    bool m_sdkAvailable = false;
    bool m_runtimeKnown = false;

    // 配置
    QJsonObject m_config;
    int m_diskGB = 0;

    // 性能
    QJsonObject m_perf;
    QVariantList m_perfHistory;
    bool m_perfActive = false;

    // 终端
    QString m_terminalOutput; // 渲染后（ANSI 剥离/覆盖语义处理）
    QString m_terminalRaw;    // 原始流（渲染输入）
    bool m_terminalConnected = false;
    qint64 m_termSessionId = -1;
    quint64 m_termCursor = 0;
    bool m_termReadInFlight = false;
    quint64 m_termSessionReadReqCursor = 0;
    bool m_termOpening = false;      // open 互斥（多触发源竞态：状态轮询/页切换/信号）
    int m_termInitEchoLeft = 0;      // 会话初始化 stty 回显抑制（一次性）
    // 增量渲染：已定稿文本（到最后一个 \n）与其对应的 raw 偏移。
    // 每轮只重渲最后一段（其后无 \n 的不稳定尾段），避免整缓冲重渲拖慢回显。
    QString m_termStable;
    qsizetype m_termStableBytes = 0;
    QString renderSegment(const QString& raw, qsizetype from, qsizetype to, bool* cleared);
    QString renderTerminalStream(const QString& raw);
    void suppressInitEcho();

    // 商店
    QJsonArray m_storeSources;
    QJsonArray m_storeResults;
    bool m_storeSearching = false;
    QJsonArray m_storeRecommended;
    bool m_storeRecommendLoading = false;
    QString m_storeRecommendError;
    QJsonObject m_storeApp;
    QJsonArray m_storeVersions;
    bool m_storeVersionsLoading = false;
    double m_storeInstalledCode = 0;
    QJsonObject m_storeTask;
    QJsonObject m_installTask;
    bool m_appsError = false;
    QString m_installPath;
    bool m_installDelete = false;
    bool m_installActive = false;
    bool m_storeInstalled = true; // 本次下载是否已触发安装
    bool m_keepApk = false;

    // 按键映射
    QJsonArray m_keyProfiles;
    QJsonArray m_keyTemplates;
    QProcess *m_keywatcher = nullptr;
    bool m_keywatcherRunning = false;
    QString m_keywatcherPkg;
    QString m_keyShotPath;

    // 快照
    QJsonArray m_snapshots;

    // 流量
    QJsonObject m_captureStatus;
    QJsonArray m_captureFlows;
    int m_captureTotal = 0;
    QJsonObject m_captureDetail;

    // 真机连接
    QJsonArray m_deviceList;
    QJsonArray m_deviceDiscovered;
    bool m_devicePairing = false;
    QJsonObject m_deviceHostPair; // 出码配对会话（state/qr/rows/message…）
    bool m_devicesActive = false;
    bool m_installTargetReal = false; // 本轮安装任务目标是真机（完成不生成桌面壳）
    bool m_captureActive = false;
    bool m_captureFlowsQueried = false; // 用户查询过才自动刷新
    QString m_lastCaptureQ, m_lastCaptureHost, m_lastCapturePkg;

    // 安装队列（VM 未运行时先开机再装）
    struct PendingInstall {
        QString path;
        bool deleteOnSuccess = false;
        int waitedTicks = 0;
    };
    QList<PendingInstall> m_pendingInstalls;
};
