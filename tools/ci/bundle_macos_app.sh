#!/bin/bash
# xyo 実行ファイル + Qt framework + その他の Homebrew dylib を
# 自己完結した xyo.app に同梱する。
#
#   bundle_macos_app.sh <xyo-binary> <out.app> <qml-src-dir> <version>
#
# 仕組み:
#  1) xyo.app/Contents/{MacOS,Frameworks,Resources} を作り、バイナリと
#     最小 Info.plist を配置する。
#  2) macdeployqt -qmldir=<qml-src-dir> が Qt6*.framework・Qt プラグイン・
#     QML モジュール（QtQuick.Controls/Layouts 等）をコピーし、実行
#     ファイル側の参照を @rpath へ書き換え、Resources/qt.conf も生成する。
#     Qt は LGPL-3.0 のため framework（動的リンク）のまま同梱する
#     （= ユーザーが差し替え可能 = LGPL 順守）。静的リンクにしない。
#  3) 残りの非 Qt 依存（SDL2 等の Homebrew dylib）は otool -L で拾い、
#     Frameworks へコピーして @rpath 参照に書き換える。Qt framework は
#     macdeployqt に任せるため除外する。
#  4) 全依存が @rpath / システムの範囲内に収まったことを検証し、
#     qml/QtQuick/{Layouts,Controls} の存在を確認してから ad-hoc 署名する。
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
sed "s/@XYO_APP_VERSION@/$VERSION/g" "$PLIST_IN" > "$CONTENTS/Info.plist"

# macdeployqt を先に実行し、Qt framework・QML・qt.conf・rpath を整える。
# brew の qt は keg-only で PATH に載らないことがあるため、
# $(brew --prefix qt)/bin も探す。
DEPLOYQT="$(command -v macdeployqt || true)"
if [ -z "$DEPLOYQT" ] && command -v brew >/dev/null 2>&1; then
  p="$(brew --prefix qt 2>/dev/null || true)"
  [ -n "$p" ] && [ -x "$p/bin/macdeployqt" ] && DEPLOYQT="$p/bin/macdeployqt"
fi
if [ -z "$DEPLOYQT" ]; then
  echo "macdeployqt が見つかりません（brew install qt）" >&2
  exit 1
fi
"$DEPLOYQT" "$APP" -qmldir="$QMLDIR" -verbose=1

# macdeployqt は Qt のバージョンによって qt.conf を生成しないことがある。
# バンドル内の探索パスを固定するため、明示的に書く（Prefix は Contents、
# QML は macdeployqt が置く Resources/qml）。
cat > "$RES/qt.conf" <<'CONF'
[Paths]
Prefix = ..
PlugIns = PlugIns
Imports = Resources/qml
Qml2Imports = Resources/qml
CONF

# 実行ファイルが参照する非 Qt・非システムの dylib を Frameworks へ
# 再帰的にコピーし、@rpath 参照へ書き換える。brew の絶対パス
# （/opt/homebrew, /usr/local）に留まる依存が無くなるまで繰り返す。
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

pass=0
while [ $pass -lt 20 ]; do
  pass=$((pass + 1))
  changed=0
  # 対象: 実行ファイル + Frameworks 内の（Qt 以外の）dylib。
  targets=("$MACOS/xyo")
  while IFS= read -r -d '' f; do targets+=("$f"); done < \
    <(find "$FW" -type f -print0 2>/dev/null || true)
  for t in "${targets[@]}"; do
    # macdeployqt が ad-hoc 署名した Mach-O は install_name_tool での
    # load command 書き換えを受け付けない（code signature invalid）。
    # 署名を剥がしてから書き換え、最後にアプリ全体を ad-hoc 署名し直す。
    codesign --remove-signature "$t" 2>/dev/null || true
    chmod u+w "$t" 2>/dev/null || true
    while IFS= read -r dep; do
      is_system_or_rpath "$dep" && continue
      is_qt_framework "$dep" && continue
      base="$(basename "$dep")"
      if [ ! -f "$FW/$base" ]; then
        [ -f "$dep" ] || { echo "依存ライブラリが見つかりません: $dep" >&2; exit 1; }
        echo "bundle: $base"
        cp -L "$dep" "$FW/$base"
        codesign --remove-signature "$FW/$base" 2>/dev/null || true
        chmod u+w "$FW/$base"
        install_name_tool -id "@rpath/$base" "$FW/$base"
        # 同梱 dylib 自身が別の同梱 dylib を @rpath で解決できるよう
        # @loader_path（Frameworks 内 = 自分と同じ階層）を rpath に足す。
        install_name_tool -add_rpath "@loader_path" "$FW/$base" 2>/dev/null \
          || true
      fi
      if install_name_tool -change "$dep" "@rpath/$base" "$t"; then
        changed=1
      else
        echo "[bundle] install_name_tool 失敗: $t <- $dep" >&2
      fi
    done < <(otool -L "$t" | awk 'NR>1 {print $1}')
  done
  [ $changed -eq 0 ] && break
done

# 検証: brew 等の絶対パス依存が残っていないこと。
bad=0
for t in "$MACOS/xyo" $(find "$FW" -type f 2>/dev/null); do
  while IFS= read -r dep; do
    if ! is_system_or_rpath "$dep" && ! is_qt_framework "$dep"; then
      echo "未同梱の依存: $t -> $dep" >&2
      bad=1
    fi
  done < <(otool -L "$t" | awk 'NR>1 {print $1}')
done
[ $bad -eq 0 ] || exit 1

# QML モジュールの同梱を検証（Windows 側の欠落と同じ失敗を防ぐ）。
test -d "$RES/qml/QtQuick/Layouts" \
  || { echo "qml/QtQuick/Layouts が同梱されていません" >&2; exit 1; }
test -d "$RES/qml/QtQuick/Controls" \
  || { echo "qml/QtQuick/Controls が同梱されていません" >&2; exit 1; }
test -f "$RES/qt.conf" \
  || { echo "qt.conf が生成されていません" >&2; exit 1; }

# インストール名を書き換えたため署名は剥がれている。公証はしないので
# ad-hoc 署名で整える（Gatekeeper 通過には従来どおり quarantine 解除が必要）。
codesign --force --deep --sign - "$APP" 2>/dev/null || true

echo "bundled: $APP"
du -sh "$APP" || true
