#!/bin/bash
# Qt 前端构建（macOS 开发机；其他平台见 app/qt/README.md）
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/app/qt"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qtbase)}"
cmake -B build -DCMAKE_PREFIX_PATH="$QT_PREFIX" -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build -j
echo "✓ $(du -h build/HouttuyniaApp | cut -f1)  $ROOT/app/qt/build/HouttuyniaApp"
