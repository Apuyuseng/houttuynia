package com.mwine.bridge;

import android.app.WallpaperManager;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import java.io.FileOutputStream;

/**
 * 设备侧壁纸导出（当前系统壁纸的真实位图，无桌面图标干扰）。
 * 用法（shell uid 即可，读取壁纸无需权限）：
 *   CLASSPATH=$(pm path com.mwine.bridge | sed 's/package://') \
 *     app_process / com.mwine.bridge.WallpaperDump <输出.png> [最大边长]
 *
 * 与宿主侧「HOME 截屏」方案互补：本类拿到的是壁纸本体（静态/动态截图均可），
 * 截屏方案仅作本类不可用时的回退。
 */
public class WallpaperDump {

    public static void main(String[] args) {
        try {
            if (args.length < 1) {
                System.err.println("usage: WallpaperDump <out.png> [maxDim]");
                System.exit(2);
            }
            String out = args[0];
            int maxDim = args.length >= 2 ? Integer.parseInt(args[1]) : 1080;

            android.os.Looper.prepareMainLooper();
            Class<?> at = Class.forName("android.app.ActivityThread");
            Object thread = at.getDeclaredMethod("systemMain").invoke(null);
            Context sysContext = (Context) at.getMethod("getSystemContext").invoke(thread);
            // wallpaper 服务校验 opPackageName 与调用 uid 的归属：
            // systemContext 的属主包与 shell uid 不匹配会被拒（IllegalArgumentException）。
            // shell uid 自带包 com.android.shell —— 以它为 op 包即可通过校验。
            Context context = sysContext;
            try {
                context = sysContext.createPackageContext("com.android.shell", 0);
            } catch (Exception ignored) {
            }

            WallpaperManager wm = WallpaperManager.getInstance(context);
            Drawable d = wm.getDrawable();
            if (d == null)
                d = wm.getBuiltInDrawable();
            if (d == null) {
                System.err.println("no wallpaper drawable");
                System.exit(3);
            }

            int w = Math.max(1, d.getIntrinsicWidth());
            int h = Math.max(1, d.getIntrinsicHeight());
            final float scale = Math.min(1f, (float) maxDim / Math.max(w, h));
            Bitmap bmp = Bitmap.createBitmap(Math.max(1, Math.round(w * scale)),
                                             Math.max(1, Math.round(h * scale)),
                                             Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(bmp);
            canvas.scale((float) bmp.getWidth() / w, (float) bmp.getHeight() / h);
            d.setBounds(0, 0, w, h);
            d.draw(canvas);

            try (FileOutputStream fos = new FileOutputStream(out)) {
                bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
            }
            System.exit(0);
        } catch (Throwable t) {
            System.err.println("wallpaper dump failed: " + t);
            System.exit(4);
        }
    }
}
