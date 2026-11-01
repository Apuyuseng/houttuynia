package com.mwine.bridge;

import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.drawable.Drawable;
import java.io.FileOutputStream;

/**
 * 设备侧图标渲染器（宿主侧 aapt2 无法处理混淆资源名 / 矢量 / 纯 adaptive icon）。
 * 用法（shell uid 即可，无需任何权限）：
 *   CLASSPATH=$(pm path com.mwine.bridge | sed 's/package://') \
 *     app_process / com.mwine.bridge.IconDump <包名> <输出.png> [尺寸]
 *
 * Context 获取走 scrcpy 同款反射（ActivityThread.systemMain -> getSystemContext）。
 */
public class IconDump {

    public static void main(String[] args) {
        try {
            if (args.length < 2) {
                System.err.println("usage: IconDump <package> <out.png> [size]");
                System.exit(2);
            }
            String pkg = args[0];
            String out = args[1];
            int size = args.length >= 3 ? Integer.parseInt(args[2]) : 512;

            android.os.Looper.prepareMainLooper();
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getDeclaredMethod("systemMain").invoke(null);
            Context context = (Context) at.getMethod("getSystemContext").invoke(thread);
            PackageManager pm = context.getPackageManager();
            Drawable d = pm.getApplicationIcon(pkg);

            // 渲染统一走 IconRender（adaptive 108 视口取 72/108 + legacy 图形垫底板）
            byte[] png = IconRender.renderPng(d, size);
            try (FileOutputStream fos = new FileOutputStream(out)) {
                fos.write(png);
            }
            System.out.println("ok " + out);
        } catch (Exception e) {
            Throwable c = e.getCause();
            System.err.println("icon dump failed: " + e + (c != null ? " cause: " + c : ""));
            System.exit(1);
        }
    }
}
