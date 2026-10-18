#!/usr/bin/env bash
# 构建 mwine 通知桥 companion APK（无需 Android Studio / gradle）
# 依赖：JDK 8+（javac）、Android SDK build-tools 34、platforms/android-33
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/assets/companion"
OUT="$ROOT/assets/companion/dist"
SDK="${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}"
BT="$SDK/build-tools/34.0.0"
AJ="$SDK/platforms/android-33/android.jar"
JAVA_HOME_BIN="$(/usr/libexec/java_home -v 17+ 2>/dev/null || /usr/libexec/java_home)"
JAVAC="$JAVA_HOME_BIN/bin/javac"
KEYTOOL="$JAVA_HOME_BIN/bin/keytool"
# d8/aapt2 走 PATH 上的 java：固定为新版 JDK，避免落到 Java 8
export JAVA_HOME="$JAVA_HOME_BIN"
export PATH="$JAVA_HOME_BIN/bin:$PATH"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"

echo "[1/5] aapt2 link（无资源，仅 manifest）"
"$BT/aapt2" link \
    -I "$AJ" \
    --manifest "$SRC/AndroidManifest.xml" \
    --min-sdk-version 29 \
    --target-sdk-version 33 \
    --version-code 1 \
    --version-name 0.1 \
    -o "$OUT/mwine-bridge-unsigned.apk"

echo "[2/5] javac"
"$JAVAC" -source 8 -target 8 -Xlint:-options \
    -cp "$AJ" \
    -d "$OUT/classes" \
    "$SRC"/java/com/mwine/bridge/*.java

echo "[3/5] d8 dex"
"$BT/d8" --release --lib "$AJ" --min-api 29 \
    --output "$OUT/dex" \
    "$OUT"/classes/com/mwine/bridge/*.class

echo "[4/5] 打包 dex 进 apk"
(cd "$OUT/dex" && zip -q -j "$OUT/mwine-bridge-unsigned.apk" classes.dex)
"$BT/zipalign" -f 4 "$OUT/mwine-bridge-unsigned.apk" "$OUT/mwine-bridge-aligned.apk"

echo "[5/5] 签名"
if [ ! -f "$SRC/debug.keystore" ]; then
    "$KEYTOOL" -genkeypair -keystore "$SRC/debug.keystore" \
        -alias mwine -keyalg RSA -keysize 2048 -validity 10000 \
        -storepass mwine2026 -keypass mwine2026 \
        -dname "CN=mwine,O=mwine,C=CN" >/dev/null 2>&1
fi
"$BT/apksigner" sign \
    --ks "$SRC/debug.keystore" --ks-pass pass:mwine2026 \
    --out "$OUT/mwine-bridge.apk" \
    "$OUT/mwine-bridge-aligned.apk"

ls -la "$OUT/mwine-bridge.apk"
echo "完成：$OUT/mwine-bridge.apk"
