package com.mwine.bridge;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import java.io.ByteArrayOutputStream;

/** 图标渲染公用：与 IconDump 同语义（adaptive 整只画 108 视口取中心 72/108）。 */
public final class IconRender {
    private IconRender() {}

    public static byte[] renderPng(Drawable d, int size) {
        Bitmap bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bmp);
        if (d instanceof AdaptiveIconDrawable) {
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
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        bmp.compress(Bitmap.CompressFormat.PNG, 100, bos);
        bmp.recycle();
        return bos.toByteArray();
    }
}
