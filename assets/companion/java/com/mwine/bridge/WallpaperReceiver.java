package com.mwine.bridge;

import android.app.WallpaperManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import java.io.File;
import java.io.FileOutputStream;

/**
 * 壁纸导出接收器：在真应用进程里读 WallpaperManager（包名/uid 归属天然匹配，
 * app_process 方式会被 wallpaper 服务的 attestation 校验拒绝）。
 *
 *   adb shell am broadcast -a com.mwine.bridge.EXPORT_WALLPAPER \
 *        --es out /sdcard/.mwine_wp_real.png [--ei maxDim 1080]
 *
 * 结果文件存在即成功（PNG）；接收器跑完就退，无常驻开销。
 */
public class WallpaperReceiver extends BroadcastReceiver {

    @Override
    public void onReceive(Context context, Intent intent) {
        // Scoped Storage：直写 /sdcard 需要 WRITE_EXTERNAL_STORAGE（targetSdk 33 已无）
        // —— 落到应用外部私有目录（免权限，宿主经 adb 可读）。
        // 入参 out 仅作文件名；实际路径 = getExternalFilesDir/<out>
        String name = intent.getStringExtra("out");
        if (name == null || name.isEmpty()) {
            name = "wallpaper.png";
        }
        name = new File(name).getName();
        // 落点优先 /sdcard 根（宿主 adb shell 可读——A11+ scoped storage 挡的
        // 正是应用外部私有目录）。MANAGE_EXTERNAL_STORAGE 由宿主 appops 授予；
        // 未授时直写会失败 → 兜底应用外部私有目录（老安卓 adb 可读）。
        File dir = new File(android.os.Environment.getExternalStorageDirectory(), "mwine");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            dir = context.getExternalFilesDir(null);
            android.util.Log.i("mwine-bridge", "sdcard 不可写，回落应用目录");
        }
        android.util.Log.i("mwine-bridge", "WallpaperReceiver run, out=" + name
                + " dir=" + dir);
        if (dir == null) {
            return;
        }
        final String out = new File(dir, name).getAbsolutePath();
        int maxDim = intent.getIntExtra("maxDim", 1080);
        try {
            WallpaperManager wm = WallpaperManager.getInstance(context);
            Drawable d = wm.getDrawable();
            if (d == null) {
                d = wm.getBuiltInDrawable();
            }
            if (d == null) {
                return;
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

            File f = new File(out);
            try (FileOutputStream fos = new FileOutputStream(f)) {
                bmp.compress(Bitmap.CompressFormat.PNG, 100, fos);
            }
        } catch (Throwable t) {
            try (FileOutputStream e = new FileOutputStream(out + ".err")) {
                e.write(String.valueOf(t).getBytes());
            } catch (Throwable ignored2) {
            }
        }
    }
}
