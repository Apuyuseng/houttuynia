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
            }
        } catch (Exception e) {
            android.util.Log.w("mwine", "bridge server died: " + e);
            synchronized (LOCK) {
                server = null;
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
