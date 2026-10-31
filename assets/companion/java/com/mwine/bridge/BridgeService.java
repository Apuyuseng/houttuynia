package com.mwine.bridge;

import android.app.Notification;
import android.service.notification.NotificationListenerService;
import android.service.notification.StatusBarNotification;
import android.os.Bundle;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONObject;

/**
 * 通知桥：监听设备全部通知，单行 JSON 推送到宿主。
 * 架构（forward 模式，与 scrcpy 同向，不依赖 adb reverse）：
 *   进程内单例 TCP server（127.0.0.1:27185）；
 *   宿主 `adb forward tcp:27186 tcp:27185` 后连接 27186 持长连接读取。
 * 每个接入的客户端先收一行 {"event":"hello"}，之后收
 * {"pkg","key","title","text","postTime"} 通知行；断开的客户端自动剔除。
 *
 * server/线程池均为进程级静态：宿主会在重装后 disallow->allow 重绑服务，
 * 服务实例会销毁重建，但进程通常被缓存复用——server 不能随实例关闭，
 * 否则新实例 EADDRINUSE、旧 accept 线程写入被拒。进程退出即整体回收。
 */
public class BridgeService extends NotificationListenerService {

    static final int PORT = 27185;

    private static final Object LOCK = new Object();
    private static ServerSocket server; // 进程内单例；null = 未启动/已死
    private static final List<Socket> clients = new ArrayList<>();
    private static final ExecutorService io = Executors.newSingleThreadExecutor();

    @Override
    public void onCreate() {
        super.onCreate();
        instance = this;
        synchronized (LOCK) {
            if (server == null) {
                try {
                    server = new ServerSocket(PORT, 4, InetAddress.getByName("127.0.0.1"));
                } catch (Exception e) {
                    android.util.Log.w("mwine", "bridge server bind failed: " + e);
                    return;
                }
                Thread t = new Thread(BridgeService::serve, "mwine-bridge-server");
                t.start();
            }
        }
    }

    private static void serve() {
        // 自愈循环：accept 异常（如 backlog 占满后文件描述符问题）不永久杀死监听，
        // 关闭旧 ServerSocket 重建再收（荣耀 MagicOS 长时间运行实测会偶发 accept 失败）
        while (true) {
            try {
                android.util.Log.i("mwine", "bridge server listening on 127.0.0.1:" + PORT);
                while (true) {
                    final Socket s = server.accept();
                    android.util.Log.i("mwine", "host connected: " + s);
                    io.execute(() -> {
                        sendToOne(s, eventJson("hello"));
                        synchronized (clients) {
                            clients.add(s);
                        }
                    });
                    // 请求通道（与通知推送共用同一条连接）：
                    //   {"req":"apps"} → {"resp":"apps","apps":[{pkg,label,icon}…]}
                    //   icon 为 base64 PNG（系统 API 渲染，密度/本地化与桌面一致）
                    Thread rt = new Thread(() -> serveRequests(s), "mwine-bridge-req");
                    rt.start();
                }
            } catch (Exception e) {
                android.util.Log.w("mwine", "bridge server died, rebuilding: " + e);
                synchronized (LOCK) {
                    try { if (server != null) server.close(); } catch (Exception ignored) {}
                    server = null;
                    try {
                        server = new ServerSocket(PORT, 8, InetAddress.getByName("127.0.0.1"));
                        android.util.Log.i("mwine", "bridge server rebuilt");
                    } catch (Exception e2) {
                        android.util.Log.w("mwine", "rebuild failed: " + e2);
                    }
                }
                try { Thread.sleep(2000); } catch (InterruptedException ignored) {}
                if (server == null) return; // 重建也失败：等下次 onCreate/重绑
            }
        }
    }

    @Override
    public void onNotificationPosted(StatusBarNotification sbn) {
        // 自己的测试通知也转发（便于 E2E 验证）
        Notification n = sbn.getNotification();
        if (n == null) {
            return;
        }
        Bundle extras = n.extras;
        CharSequence titleCs = extras.getCharSequence(Notification.EXTRA_TITLE);
        CharSequence textCs = extras.getCharSequence(Notification.EXTRA_TEXT);
        String title = titleCs == null ? "" : titleCs.toString();
        String text = textCs == null ? "" : textCs.toString();
        if (title.isEmpty() && text.isEmpty()) {
            return;
        }
        try {
            JSONObject obj = new JSONObject();
            obj.put("pkg", sbn.getPackageName());
            obj.put("key", sbn.getKey());
            obj.put("title", title);
            obj.put("text", text);
            obj.put("postTime", sbn.getPostTime());
            io.execute(() -> broadcast(obj.toString()));
        } catch (Exception e) {
            android.util.Log.w("mwine", "json failed: " + e);
        }
    }

    /** 单客户端请求循环：一行一个请求，读到断开为止。 */
    private static void serveRequests(Socket s) {
        try {
            java.io.BufferedReader in = new java.io.BufferedReader(
                    new java.io.InputStreamReader(s.getInputStream(), "UTF-8"));
            while (true) {
                String line = in.readLine();
                if (line == null) break;
                String resp = handleRequest(line.trim());
                if (resp != null) {
                    synchronized (clients) {
                        // 直接写（可能数 MB，不能经 io 单线程排队阻塞通知）
                        try {
                            java.io.OutputStream out = s.getOutputStream();
                            out.write((resp + "\n").getBytes("UTF-8"));
                            out.flush();
                        } catch (Exception e) {
                            closeQuietly(s);
                            synchronized (clients) { clients.remove(s); }
                            return;
                        }
                    }
                }
            }
        } catch (Exception ignored) {
        }
    }

    private static String handleRequest(String line) {
        try {
            JSONObject req = new JSONObject(line);
            String r = req.optString("req", "");
            if ("apps".equals(r)) {
                return appsJson();
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    /** 全量可启动应用：系统 PackageManager 取 label + 图标（144px PNG base64）。 */
    private static String appsJson() {
        try {
            android.content.pm.PackageManager pm = getCtx().getPackageManager();
            android.content.Intent it = new android.content.Intent(
                    android.content.Intent.ACTION_MAIN);
            it.addCategory(android.content.Intent.CATEGORY_LAUNCHER);
            java.util.List<android.content.pm.ResolveInfo> ris =
                    pm.queryIntentActivities(it, 0);
            org.json.JSONArray arr = new org.json.JSONArray();
            java.util.HashSet<String> seen = new java.util.HashSet<>();
            for (android.content.pm.ResolveInfo ri : ris) {
                String pkg = ri.activityInfo == null ? null : ri.activityInfo.packageName;
                if (pkg == null || !seen.add(pkg)) continue;
                try {
                    CharSequence labelCs = ri.loadLabel(pm);
                    String label = labelCs == null ? pkg : labelCs.toString();
                    android.graphics.drawable.Drawable icon = ri.loadIcon(pm);
                    String b64 = "";
                    if (icon != null) {
                        b64 = android.util.Base64.encodeToString(
                                IconRender.renderPng(icon, 144), android.util.Base64.NO_WRAP);
                    }
                    JSONObject o = new JSONObject();
                    o.put("pkg", pkg);
                    o.put("label", label);
                    o.put("icon", b64);
                    arr.put(o);
                } catch (Exception ignored) {
                }
            }
            JSONObject resp = new JSONObject();
            resp.put("resp", "apps");
            resp.put("apps", arr);
            return resp.toString();
        } catch (Exception e) {
            android.util.Log.w("mwine", "appsJson failed: " + e);
            return "{\"resp\":\"apps\",\"apps\":[]}";
        }
    }

    private static android.content.Context getCtx() {
        return ctx != null ? ctx : (ctx = instance);
    }
    private static volatile android.content.Context ctx;
    private static volatile android.content.Context instance;

    private static String eventJson(String event) {
        try {
            JSONObject obj = new JSONObject();
            obj.put("event", event);
            return obj.toString();
        } catch (Exception e) {
            return "{}";
        }
    }

    /** 单线程 io executor 里执行：写一个客户端，失败则剔除。 */
    private static void sendToOne(Socket s, String line) {
        try {
            java.io.OutputStream out = s.getOutputStream();
            out.write((line + "\n").getBytes("UTF-8"));
            out.flush();
        } catch (Exception e) {
            android.util.Log.w("mwine", "write failed, dropping client: " + e);
            closeQuietly(s);
        }
    }

    private static void broadcast(String line) {
        synchronized (clients) {
            Iterator<Socket> it = clients.iterator();
            while (it.hasNext()) {
                Socket s = it.next();
                try {
                    java.io.OutputStream out = s.getOutputStream();
                    out.write((line + "\n").getBytes("UTF-8"));
                    out.flush();
                } catch (Exception e) {
                    it.remove();
                    closeQuietly(s);
                }
            }
        }
    }

    private static void closeQuietly(Socket s) {
        try {
            s.close();
        } catch (Exception ignored) {
        }
    }
}
