#!/bin/bash
# C++ 工作区构建（严格模式：任何失败都响亮报错，绝不静默吞掉）
# 用法：scripts/build-cpp.sh [target...]（默认全量 + 测试）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/cpp/build"
QT_PREFIX="${QT_PREFIX:-/opt/homebrew}"

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  cmake -B "$BUILD" -S "$ROOT/cpp" -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
        -DCMAKE_BUILD_TYPE=Release
fi

# shellcheck disable=SC2086
if [ $# -gt 0 ]; then
  cmake --build "$BUILD" --target "$@" -j
else
  cmake --build "$BUILD" -j
  ctest --test-dir "$BUILD" --output-on-failure
fi

# 新鲜度断言：产物必须晚于全部源码（防止「构建在错误目录跑了个寂寞」）
BIN="$BUILD/daemon/mwine-cored"
if [ -f "$BIN" ]; then
  STALE=$(find "$ROOT/cpp/common" "$ROOT/cpp/modules" "$ROOT/cpp/daemon" \
            -newer "$BIN" \( -name '*.cpp' -o -name '*.hpp' \) 2>/dev/null | head -3)
  if [ -n "$STALE" ]; then
    echo "✗ 二进制比源码旧（疑似构建未生效）， newer-than-binary 源文件：" >&2
    echo "$STALE" >&2
    exit 1
  fi
fi
echo "✓ 构建完成（产物已校验新鲜）: $BIN"
