#!/bin/bash
# 刷新 ~/Applications/Mwine 下所有应用壳的内嵌 mwine 二进制
# ⚠️ 只刷带 app.conf 的壳；绝不动 MWine.app（主程序）与 Mwine Installer.app
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NEW="$ROOT/target/release/mwine"
[ -f "$NEW" ] || { echo "先 cargo build --release"; exit 1; }
for app in ~/Applications/Houttuynia/*.app; do
  name=$(basename "$app")
  [ "$name" = "MWine.app" ] && continue
  [ "$name" = "Mwine Installer.app" ] && continue
  conf="$app/Contents/Resources/app.conf"
  [ -f "$conf" ] || continue   # 无 conf 的不是应用壳
  exe=$(basename "$(ls "$app/Contents/MacOS/" | head -1)")
  cp "$NEW" "$app/Contents/MacOS/$exe"
  chmod 755 "$app/Contents/MacOS/$exe"
  codesign -f -s - "$app/Contents/MacOS/$exe" >/dev/null 2>&1 || true
  echo "✓ $name"
done
