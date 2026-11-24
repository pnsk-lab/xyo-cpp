#!/usr/bin/env bash
# bundle_macos_app.sh — xyo.app を自己完結バンドルにする（macOS リリース用）
#
# 使い方: tools/ci/bundle_macos_app.sh <ビルド済み xyo> <出力 .app> <qml dir> [version]
#
# 手順:
#   1) Contents/MacOS/xyo と最小 Info.plist を置く。
#   2) **macdeployqt より先に**、非 Qt の Homebrew dylib（SDL2, brotli, zstd 等）
#      をプリスティンな元ファイルから Frameworks へコピーし、@rpath へ書き換える。
#      署名済みファイルを後から書き換えると install_name_tool が
#      "link edit information does not fill the __LINKEDIT segment" で失敗する
#      ため、順序が重要（macdeployqt は自身が入れたものを ad-hoc 署名する）。
#   3) macdeployqt で Qt framework・QML モジュール・plugins・qt.conf・rpath を整える。
#      QML が無いと起動時に型が見つからず失敗するので、後段で必ず検証する。
#   4) 残った絶対パス依存を正規化（失敗時は元ファイルから入れ直して再試行）。
#   5) QML/QtQuick/{Layouts,Controls} と qt.conf の存在、依存の閉包を検証し、
#      最後に ad-hoc 署名する（公証はしない。Gatekeeper は quarantine 解除が必要）。
set -euo pipefail

BIN="$1"
APP="$2"
QMLDIR="$3"
VERSION="${4:-0.0.0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PLIST_IN="$ROOT/src/shell/Info.plist.in"

CONTENTS="$APP/Contents"
MACOS="$CONTENTS/MacOS"
FW="$CONTENTS/Frameworks"
RES="$CONTENTS/Resources"
rm -rf "$APP"
mkdir -p "$MACOS" "$FW" "$RES"
cp "$BIN" "$MACOS/xyo"
chmod u+w "$MACOS/xyo"
sed "s/@XYO_APP_VERSION@/$VERSION/g" "$PLIST_IN" > "$CONTENTS/Info.plist"

is_system_or_rpath() {
  case "$1" in
    /usr/lib/*|/System/*|@rpath/*|@executable_path/*|@loader_path/*) return 0 ;;
    *) return 1 ;;
  esac
}
is_qt_framework() {
  case "$1" in
    *Qt*.framework/*|*/qt/lib/*|*/qt6/lib/*) return 0 ;;
    *) return 1 ;;
  esac
}

# --- 2) Qt 以外の依存を先に集める（元ファイルは未署名なので書き換え可能） -----
bundle_deps() {
pass=0
while [ $pass -lt 20 ]; do
  pass=$((pass + 1))
  changed=0
  targets=("$MACOS/xyo")
  while IFS= read -r -d '' f; do targets+=("$f"); done < \
    <(find "$FW" \( -type f -o -type l \) -print0 2>/dev/null || true)
  for t in "${targets[@]}"; do
    chmod u+w "$t" 2>/dev/null || true
    while IFS= read -r dep; do
      is_system_or_rpath "$dep" && continue
      is_qt_framework "$dep" && continue
      base="$(basename "$dep")"
      if [ ! -f "$FW/$base" ]; then
        [ -f "$dep" ] || { echo "依存ライブラリが見つかりません: $dep" >&2; exit 1; }
        echo "bundle: $base"
        cp -L "$dep" "$FW/$base"
        chmod u+w "$FW/$base"
        install_name_tool -id "@rpath/$base" "$FW/$base"
        install_name_tool -add_rpath "@loader_path" "$FW/$base" 2>/dev/null || true
      fi
      if install_name_tool -change "$dep" "@rpath/$base" "$t"; then
        changed=1
      else
        echo "[bundle] install_name_tool 失敗: $t <- $dep" >&2
      fi
    done < <(otool -L "$t" 2>/dev/null | awk 'NR>1 {print $1}')
  done
  [ $changed -eq 0 ] && break
done
}

bundle_deps

# sdl2-compat は実行時に SDL3 を dlopen する（@loader_path / @executable_path
# の "libSDL3.dylib"）。同梱 SDL2 が sdl2-compat なら SDL3 も入れないと
# 起動直後に "Failed loading SDL3 library" で落ちる。
SDL2_DYLIB="$(ls "$FW"/libSDL2*.dylib 2>/dev/null | head -1 || true)"
if [ -n "$SDL2_DYLIB" ] && strings -a "$SDL2_DYLIB" | grep -q "libSDL3\.dylib"; then
  SDL3_SRC=""
  if command -v brew >/dev/null 2>&1; then
    p="$(brew --prefix sdl3 2>/dev/null || true)"
    [ -n "$p" ] && SDL3_SRC="$(ls "$p"/lib/libSDL3*.dylib 2>/dev/null | head -1 || true)"
  fi
  if [ -z "$SDL3_SRC" ]; then
    echo "sdl2-compat ですが SDL3 が見つかりません（brew install sdl3）" >&2
    exit 1
  fi
  echo "bundle: libSDL3.dylib (sdl2-compat の実行時依存)"
  cp -L "$SDL3_SRC" "$FW/libSDL3.dylib"
  chmod u+w "$FW/libSDL3.dylib"
  install_name_tool -id "@rpath/libSDL3.dylib" "$FW/libSDL3.dylib"
  install_name_tool -add_rpath "@loader_path" "$FW/libSDL3.dylib" 2>/dev/null || true
  # sdl2-compat は @executable_path/libSDL3.dylib も試すので実行ファイル隣にも置く。
  ln -sfn ../Frameworks/libSDL3.dylib "$MACOS/libSDL3.dylib"
  # SDL3 自身の依存も取り込む。
  bundle_deps
fi

# --- 3) macdeployqt -------------------------------------------------------
DEPLOYQT="$(command -v macdeployqt || true)"
if [ -z "$DEPLOYQT" ] && command -v brew >/dev/null 2>&1; then
  p="$(brew --prefix qt 2>/dev/null || true)"
  [ -n "$p" ] && [ -x "$p/bin/macdeployqt" ] && DEPLOYQT="$p/bin/macdeployqt"
fi
if [ -z "$DEPLOYQT" ]; then
  echo "macdeployqt が見つかりません（brew install qt）" >&2
  exit 1
fi
"$DEPLOYQT" "$APP" -qmldir="$QMLDIR" -verbose=1 || \
  echo "macdeployqt が警告付きで終了しました（検証は続行）" >&2

# macdeployqt は Qt のバージョンにより qt.conf を生成しないことがある。
# バンドル内の探索パスを固定するため、明示的に書く（Prefix は Contents、
# QML は macdeployqt が置く Resources/qml）。
cat > "$RES/qt.conf" <<'CONF'
[Paths]
Prefix = ..
PlugIns = PlugIns
Imports = Resources/qml
Qml2Imports = Resources/qml
CONF

# Qt が読むのは実行ファイルと同じディレクトリの qt.conf。
# Prefix を書かない場合、相対パスは実行ファイルのディレクトリ基準になる。
cat > "$MACOS/qt.conf" <<'CONF'
[Paths]
PlugIns = ../PlugIns
Imports = ../Resources/qml
Qml2Imports = ../Resources/qml
CONF

# qt.conf が効かない経路（バンドル外から素のバイナリとして起動、
# 一部 Qt バージョン等）でも cocoa プラグインと QML を見つけられるよう、
# 実行ファイル隣から実体へ相対シンボリックリンクを張る。
ln -sfn ../PlugIns/platforms "$MACOS/platforms"
ln -sfn ../Resources/qml "$MACOS/qml"

# --- 4) 残った絶対パス依存の正規化（macdeployqt が後から足した分） -----------
for pass in 1 2 3; do
  remain=0
  targets=("$MACOS/xyo")
  while IFS= read -r -d '' f; do targets+=("$f"); done < \
    <(find "$FW" \( -type f -o -type l \) -print0 2>/dev/null || true)
  for t in "${targets[@]}"; do
    chmod u+w "$t" 2>/dev/null || true
    while IFS= read -r dep; do
      is_system_or_rpath "$dep" && continue
      is_qt_framework "$dep" && continue
      remain=1
      base="$(basename "$dep")"
      if [ ! -f "$FW/$base" ] && [ -f "$dep" ]; then
        echo "bundle(late): $base"
        cp -L "$dep" "$FW/$base"
        chmod u+w "$FW/$base"
        install_name_tool -id "@rpath/$base" "$FW/$base"
      fi
      codesign --remove-signature "$t" 2>/dev/null || true
      install_name_tool -change "$dep" "@rpath/$base" "$t" || {
        # 署名剥がしで壊れた場合は元ファイルから入れ直して再試行する。
        if [ -f "$dep" ]; then
          codesign --remove-signature "$t" 2>/dev/null || true
          cp -f "$dep" "$t"
          chmod u+w "$t"
          install_name_tool -id "@rpath/$(basename "$t")" "$t" 2>/dev/null || true
          install_name_tool -change "$dep" "@rpath/$base" "$t"
        else
          echo "書き換えに失敗（元ファイルも無し）: $t <- $dep" >&2
        fi
      }
    done < <(otool -L "$t" 2>/dev/null | awk 'NR>1 {print $1}')
  done
  [ $remain -eq 0 ] && break
done

# --- 5) 検証 --------------------------------------------------------------
bad=0
for t in "$MACOS/xyo" $(find "$FW" \( -type f -o -type l \) 2>/dev/null); do
  while IFS= read -r dep; do
    if ! is_system_or_rpath "$dep" && ! is_qt_framework "$dep"; then
      echo "未同梱の依存: $t -> $dep" >&2
      echo "--- otool -L $t ---" >&2
      otool -L "$t" >&2 || true
      bad=1
    fi
  done < <(otool -L "$t" 2>/dev/null | awk 'NR>1 {print $1}')
done
[ $bad -eq 0 ] || exit 1

test -d "$RES/qml/QtQuick/Layouts" \
  || { echo "qml/QtQuick/Layouts が同梱されていません" >&2; exit 1; }
test -d "$RES/qml/QtQuick/Controls" \
  || { echo "qml/QtQuick/Controls が同梱されていません" >&2; exit 1; }
test -f "$RES/qt.conf" \
  || { echo "qt.conf(Resources) が生成されていません" >&2; exit 1; }
test -f "$MACOS/qt.conf" \
  || { echo "qt.conf(MacOS) が生成されていません" >&2; exit 1; }
# Qt が実際に探す場所から cocoa プラグインと QML が見えること。
test -f "$MACOS/platforms/libqcocoa.dylib" \
  || { echo "platforms/libqcocoa.dylib が見つかりません（qt.conf/リンク不備）" >&2; exit 1; }
test -f "$MACOS/qml/QtQuick/Controls/qmldir" \
  || { echo "qml/QtQuick/Controls が見つかりません（qt.conf/リンク不備）" >&2; exit 1; }
# sdl2-compat なら SDL3 が実行ファイル隣と Frameworks の両方から解決できること。
SDL2_DYLIB="$(ls "$FW"/libSDL2*.dylib 2>/dev/null | head -1 || true)"
if [ -n "$SDL2_DYLIB" ] && strings -a "$SDL2_DYLIB" | grep -q "libSDL3\.dylib"; then
  test -f "$FW/libSDL3.dylib" \
    || { echo "sdl2-compat 用の SDL3 が同梱されていません" >&2; exit 1; }
  test -f "$MACOS/libSDL3.dylib" \
    || { echo "SDL3 が実行ファイル隣にありません（@executable_path で解決不可）" >&2; exit 1; }
fi

# 署名は install_name_tool で剥がれている。公証はしないので ad-hoc 署名で整える
# （Gatekeeper 通過には従来どおり quarantine 解除が必要）。
codesign --force --deep --sign - "$APP" 2>/dev/null || true

echo "bundled: $APP"
du -sh "$APP" || true
