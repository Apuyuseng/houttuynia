package com.mwine.bridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * E2E 测试接收器：`adb shell am broadcast -a com.mwine.bridge.TEST [--es text "..."]`
 * 发出一条真实通知，供 BridgeService 捕获并回传宿主。
 */
public class TestReceiver extends BroadcastReceiver {

    private static final String CHANNEL = "mwine_test";

    @Override
    public void onReceive(Context context, Intent intent) {
        NotificationManager nm =
                (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (nm == null) {
            return;
        }
        NotificationChannel ch = new NotificationChannel(
                CHANNEL, "mwine 测试", NotificationManager.IMPORTANCE_DEFAULT);
        nm.createNotificationChannel(ch);

        String text = intent.getStringExtra("text");
        if (text == null) {
            text = "来自 mwine 通知桥的测试消息";
        }
        Notification n = new Notification.Builder(context, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_notify_chat)
                .setContentTitle("mwine bridge")
                .setContentText(text)
                .setAutoCancel(true)
                .build();
        nm.notify((int) System.currentTimeMillis(), n);
    }
}
