package com.mwine.bridge;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.drawable.AdaptiveIconDrawable;
import android.graphics.drawable.Drawable;
import java.io.ByteArrayOutputStream;

/** 图标渲染公用：与 IconDump 同语义（adaptive 整只画 108 视口取中心 72/108）。 */
public final class IconRender {
    private IconRender() {}

    public static byte[] renderPng(Drawable d, int size) {
        Bitmap bmp = renderDrawable(d, size);
        // legacy「纯图形」图标（透明底小 glyph，如系统应用）：铺不满格子，
        // 垫一层不透明底板（浅色图形→白底；白色图形→深底防消失），统一全出血观感
        if (opaqueCoverage(bmp) < 0.62f) {
            bmp = padGlyphOnTile(bmp, size);
        }
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        bmp.compress(Bitmap.CompressFormat.PNG, 100, bos);
        bmp.recycle();
        return bos.toByteArray();
    }

    private static Bitmap renderDrawable(Drawable d, int size) {
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
            bmp.recycle();
            bmp = cropped;
        } else {
            d.setBounds(0, 0, size, size);
            d.draw(canvas);
        }
        return bmp;
    }

    /** 不透明像素占比（alpha 阈值 0x30 过滤边缘半透明） */
    private static float opaqueCoverage(Bitmap bmp) {
        int[] px = new int[bmp.getWidth() * bmp.getHeight()];
        bmp.getPixels(px, 0, bmp.getWidth(), 0, 0, bmp.getWidth(), bmp.getHeight());
        int opaque = 0;
        for (int c : px) {
            if (((c >>> 24) & 0xff) > 0x30) opaque++;
        }
        return opaque / (float) px.length;
    }

    /** 亮度低位百分位（仅计不透明像素，0=黑 1=白）。
     *  判「白色图形」不能看平均值——白底彩标（迅雷/咪咕等）均值被白色拉高
     *  会误配深底；纯白单色图形所有像素都亮，低位百分位仍高 */
    private static float glyphLuminance(Bitmap bmp) {
        int[] px = new int[bmp.getWidth() * bmp.getHeight()];
        bmp.getPixels(px, 0, bmp.getWidth(), 0, 0, bmp.getWidth(), bmp.getHeight());
        float[] lums = new float[px.length];
        int n = 0;
        for (int c : px) {
            int a = (c >>> 24) & 0xff;
            if (a > 0x30) {
                float r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
                lums[n++] = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255f;
            }
        }
        if (n == 0) return 1f;
        java.util.Arrays.sort(lums, 0, n);
        return lums[(int) (n * 0.08f)];
    }

    /** glyph 缩放至 62% 居中，垫满格不透明底板（CSS 侧再做圆角裁切） */
    private static Bitmap padGlyphOnTile(Bitmap glyph, int size) {
        float lum = glyphLuminance(glyph);
        int tile = lum > 0.72f ? 0xFF3C4043 : 0xFFF6F7F8; // 纯白图形配深底，其余配浅底
        Bitmap out = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(out);
        canvas.drawColor(tile);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        int inner = size * 62 / 100;
        int off = (size - inner) / 2;
        canvas.drawBitmap(glyph, null, new android.graphics.RectF(off, off, off + inner, off + inner), p);
        glyph.recycle();
        return out;
    }
}
