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
            // wallpaper 服务校验 opPackageName 与调用 uid 的归属（A12 实测
            // "Given calling package android does not match caller's uid 2000"）。
            // shell uid 自带包 com.android.shell —— 归因成它即可通过校验。
            Context context = sysContext;
            try {
                context = sysContext.createPackageContext("com.android.shell", 0);
            } catch (Throwable t) {
                System.err.println("createPackageContext failed: " + t);
            }
            // systemContext/createPackageContext 的 opPackageName 都是 "android"
            //（A12 实测），binder 归因校验要 opPackageName 与 uid 的包一致——
            // shell uid 本尊包是 com.android.shell，反射改写 ContextImpl 字段
            try {
                java.lang.reflect.Field f = context.getClass().getDeclaredField("mOpPackageName");
                f.setAccessible(true);
                f.set(context, "com.android.shell");
                System.err.println("attribution set: " + context.getOpPackageName());
            } catch (Throwable t) {
                System.err.println("attribution set failed: " + t);
            }

            WallpaperManager wm = WallpaperManager.getInstance(context);
            Drawable d = null;
            // 策略 1：getDrawable（A11 TV 可调但回 1×1；A12 需归因正确）
            try {
                d = wm.getDrawable();
                System.err.println("getDrawable -> " + (d == null ? "null" : d.getIntrinsicWidth() + "x" + d.getIntrinsicHeight()));
            } catch (Throwable t) {
                System.err.println("getDrawable failed: " + t);
            }
            // 策略 2：getWallpaperFile（ParcelFileDescriptor 直读壁纸原图，
            // 走不同的 binder 路径，可能不受 getDrawable 的限制）
            if (d == null || d.getIntrinsicWidth() <= 1 || d.getIntrinsicHeight() <= 1) {
                try {
                    android.os.ParcelFileDescriptor pfd =
                            wm.getWallpaperFile(WallpaperManager.FLAG_SYSTEM);
                    System.err.println("getWallpaperFile -> " + (pfd == null ? "null" : pfd.getStatSize() + "B"));
                    if (pfd != null) {
                        android.graphics.BitmapFactory.Options o = new android.graphics.BitmapFactory.Options();
                        android.graphics.Bitmap raw = android.graphics.BitmapFactory.decodeFileDescriptor(pfd.getFileDescriptor(), null, o);
                        if (raw != null) {
                            System.err.println("raw wallpaper: " + raw.getWidth() + "x" + raw.getHeight());
                            d = new android.graphics.drawable.BitmapDrawable(
                                    android.content.res.Resources.getSystem(), raw);
                        }
                    }
                } catch (Throwable t) {
                    System.err.println("getWallpaperFile failed: " + t);
                }
            }
            // 策略 3：getBuiltInDrawable（出厂默认图）
            if (d == null || d.getIntrinsicWidth() <= 1 || d.getIntrinsicHeight() <= 1) {
                try {
                    d = wm.getBuiltInDrawable();
                    System.err.println("getBuiltInDrawable -> " + (d == null ? "null" : d.getIntrinsicWidth() + "x" + d.getIntrinsicHeight()));
                } catch (Throwable t) {
                    System.err.println("getBuiltInDrawable failed: " + t);
                }
            }
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
