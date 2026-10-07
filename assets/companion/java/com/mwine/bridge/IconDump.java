package com.mwine.bridge;

import android.content.Context;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.AdaptiveIconDrawable;
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

            Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bmp);
            if (d instanceof AdaptiveIconDrawable) {
                // launcher 语义：整只 adaptive 画进 108 视口再取中心 72/108。
                // 曾按"背景铺满+前景 inset"手工拆层——微信/迅雷等前景占满安全区的
                // 图标被裁掉主体，仅简单居中类（极空间等）侥幸正确。
                AdaptiveIconDrawable aid = (AdaptiveIconDrawable) d;
                aid.setBounds(0, 0, size, size);
                aid.draw(canvas);
                Bitmap cropped = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
                Canvas c2 = new Canvas(cropped);
                int inset = size * 18 / 108;
                c2.drawBitmap(bmp, null,
                        new android.graphics.RectF(-inset, -inset, size + inset, size + inset), null);
                bmp = cropped;
            } else {
                d.setBounds(0, 0, size, size);
                d.draw(canvas);
            }

            try (FileOutputStream fos = new FileOutputStream(out)) {
                bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
            }
            System.out.println("ok " + out);
        } catch (Exception e) {
            Throwable c = e.getCause();
            System.err.println("icon dump failed: " + e + (c != null ? " cause: " + c : ""));
            System.exit(1);
        }
    }
}
