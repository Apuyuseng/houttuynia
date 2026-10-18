# Houttuynia Logo

基于鱼腥草（Houttuynia cordata）真实叶片形态设计的品牌标识。

## 设计要素

对照实物照片提取的四个辨识特征：

| 特征 | 处理方式 |
|------|----------|
| 卵状心形叶身，叶基深凹的 V 形缺口 | 主轮廓单路径勾画，缺口窄而深 |
| 掌状叶脉（中脉 + 两对侧脉） | 5 条奶白色弧线自凹口顶端放射 |
| 叶尖微微偏斜、左右略不对称 | 叶尖偏向右上，右半叶稍宽 |
| 叶基红晕 + 红紫色叶柄 | 凹口向下渐浓的红色晕染；叶柄绿→紫红渐变 |

## 文件

| 文件 | 用途 |
|------|------|
| `logo.svg` | 主标识（透明底，512 viewBox） |
| `logo-wordmark.svg` | 横版字标：叶标 + Houttuynia / 鱼腥草 · ANDROID ON MACOS |
| `logo-appicon.svg` | macOS 图标风格的 squircle 版本 |
| `png/logo-1024.png` | 主标识位图导出 |
| `png/logo-wordmark-1600.png` | 字标位图导出（README 头图） |
| `png/logo-appicon-1024.png` / `-128.png` | 图标位图（大图 / 小尺寸可读性检查） |

## 配色

- 叶身渐变：`#A9D468 → #7CB84E → #4E9A3F`
- 叶脉：`#F4F7E3`
- 叶基红晕：`#C25B4E`（0 → 50% 透明度渐入）
- 叶柄：`#6FAF4C → #9C5A50 → #AD4A5E`
- 字标深绿：`#2E4A26` / 副标灰绿：`#5F7D54`
- 图标底（squircle）：`#F2F8EC → #E3EFDA`

## 重新导出

```sh
rsvg-convert -w 1024 -h 1024 logo.svg -o png/logo-1024.png
rsvg-convert -w 1600 logo-wordmark.svg -o png/logo-wordmark-1600.png
rsvg-convert -w 1024 -h 1024 logo-appicon.svg -o png/logo-appicon-1024.png
```

替换正式 App 图标时，用 `logo-appicon.svg` 各尺寸导出 PNG 后经 `iconutil -c icns` 生成 `assets/icons/AppIcon.icns`。
