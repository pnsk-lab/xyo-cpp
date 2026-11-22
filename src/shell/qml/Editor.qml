import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Xyo.Editor 1.0

// xyo ビルトインエディタ（Qt Quick 版）。
// scratch-gui 準拠: 紫メニューバー / Code-Costumes-Sounds タブ /
// 左ツールボックス + フライアウト / ワークスペース / 右列（ステージ + スプライト）。
Item {
    id: root
    width: 1280
    height: 800
    focus: true

    // scratch-gui カラー
    readonly property color cMenu: "#855CD6"
    readonly property color cMenuDark: "#6B3EE8"
    readonly property color cBody: "#E9EEF2"
    readonly property color cPanel: "#FFFFFF"
    readonly property color cFlyout: "#F9F9F9"
    readonly property color cToolboxBg: "#E9EEF2"
    readonly property color cBorder: "#D5D9E2"
    readonly property color cFg: "#575E75"
    readonly property color cFgDim: "#8691A7"
    readonly property color cGreen: "#45993D"
    readonly property color cStop: "#EC5959"

    // ---- キー入力（ステージへ転送 + ショートカット）----
    Keys.onPressed: function(ev) {
        if (ev.modifiers & Qt.ControlModifier) {
            if (ev.key === Qt.Key_S) { editor.save(); ev.accepted = true; return; }
            if (ev.key === Qt.Key_Z) {
                if (ev.modifiers & Qt.ShiftModifier) editor.redo();
                else editor.undo();
                ev.accepted = true; return;
            }
            if (ev.key === Qt.Key_Y) { editor.redo(); ev.accepted = true; return; }
        }
        if (ev.key === Qt.Key_Delete || ev.key === Qt.Key_Backspace) {
            editor.deleteSelected(); ev.accepted = true; return;
        }
        editor.keyEvent(ev.key, true);
    }
    Keys.onReleased: function(ev) { editor.keyEvent(ev.key, false); }

    // ---- レイアウト ----
    // menuBar(44) + tabs(44) + 主行 + 右列。
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- メニューバー ----
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: cMenu
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 10
                // ロゴ
                Rectangle {
                    width: 26; height: 26; radius: 13
                    color: "#FFFFFF"; opacity: 0.9
                    Text { anchors.centerIn: parent; text: "x"; color: cMenu
                           font.pixelSize: 15; font.bold: true }
                }
                MenuButton {
                    text: "File"
                    menuItems: [
                        { label: "Save to your computer", act: "save" }
                    ]
                }
                MenuButton {
                    text: "Edit"
                    menuItems: [
                        { label: "Undo", act: "undo" },
                        { label: "Redo", act: "redo" }
                    ]
                }
                Item { Layout.fillWidth: true }
                // タイトル（点線で挟む）
                RowLayout {
                    spacing: 8
                    Repeater {
                        model: 3
                        Rectangle { width: 3; height: 3; radius: 1.5
                                    color: "#FFFFFF"; opacity: 0.7 }
                    }
                    Text {
                        text: editor.projectName
                        color: "white"; font.pixelSize: 13
                    }
                    Repeater {
                        model: 3
                        Rectangle { width: 3; height: 3; radius: 1.5
                                    color: "#FFFFFF"; opacity: 0.7 }
                    }
                }
                Item { Layout.fillWidth: true }
                // Save ボタン
                Rectangle {
                    width: 52; height: 28; radius: 14
                    color: saveHov.containsMouse ? cMenuDark : "#9A7BFF"
                    Text { anchors.centerIn: parent; text: "Save"
                           color: "white"; font.pixelSize: 12; font.bold: true }
                    MouseArea {
                        id: saveHov; anchors.fill: parent
                        hoverEnabled: true
                        onClicked: editor.save()
                    }
                }
            }
        }

        // ---- 本体 ----
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ---- 左: タブ + ツールボックス + フライアウト ----
            ColumnLayout {
                Layout.preferredWidth: 310
                Layout.maximumWidth: 310
                Layout.fillHeight: true
                spacing: 0

                // タブ（Code / Costumes / Sounds）
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    color: cPanel
                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: [
                                { name: "Code",     col: "#4C97FF" },
                                { name: "Costumes", col: "#9966FF" },
                                { name: "Sounds",   col: "#CF63CF" }
                            ]
                            delegate: Item {
                                width: parent.width / 3; height: 44
                                Rectangle {
                                    anchors.fill: parent
                                    color: editor.tab === index ? cPanel : "#D9E3F2"
                                    Rectangle {   // 上部カラー帯
                                        width: parent.width; height: 4
                                        color: modelData.col
                                        visible: editor.tab === index
                                    }
                                    Column {
                                        anchors.centerIn: parent
                                        spacing: 0
                                        Rectangle {
                                            width: 20; height: 20; radius: 10
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            color: modelData.col
                                            visible: editor.tab !== index
                                        }
                                        Text {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: modelData.name
                                            font.pixelSize: 11
                                            color: editor.tab === index ? cFg : cFgDim
                                        }
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: editor.tab = index
                                }
                            }
                        }
                    }
                }

                // Code タブ: ツールボックス + フライアウト
                RowLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0
                    visible: editor.tab === 0

                    // カテゴリツールボックス（60px）
                    Rectangle {
                        Layout.preferredWidth: 60
                        Layout.fillHeight: true
                        color: cToolboxBg
                        ListView {
                            anchors.fill: parent
                            model: editor.categoryNames.length
                            // Add Extension ボタン（ツールボックス最下部、
                            // editor.cpp::addExtRect_ 相当）
                            footer: Item {
                                width: 60; height: 52
                                Rectangle {
                                    width: 28; height: 28; radius: 14
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 4
                                    color: extMa.containsMouse
                                           ? "#6A4CA0" : "#855CD6"
                                    Text { anchors.centerIn: parent; text: "+"
                                           color: "white"; font.pixelSize: 18
                                           font.bold: true }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 34; width: 58
                                    horizontalAlignment: Text.AlignHCenter
                                    text: "Add Ext."
                                    font.pixelSize: 9; color: cFg
                                }
                                MouseArea {
                                    id: extMa; anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: extMenu.popup(
                                        parent.width, parent.height / 2)
                                }
                                Menu {
                                    id: extMenu
                                    MenuItem {
                                        text: "Pen"
                                        property bool en: {
                                            var n = editor.layoutRev
                                            return editor.enabledExtensions()
                                                   .indexOf("pen") >= 0
                                        }
                                        enabled: !en
                                        onTriggered: editor.addExtension("pen")
                                    }
                                    MenuItem {
                                        text: "Music"
                                        property bool en: {
                                            var n = editor.layoutRev
                                            return editor.enabledExtensions()
                                                   .indexOf("music") >= 0
                                        }
                                        enabled: !en
                                        onTriggered: editor.addExtension("music")
                                    }
                                }
                            }
                            delegate: Item {
                                width: 60; height: 46
                                Rectangle {
                                    anchors.fill: parent
                                    color: editor.categorySel === index
                                           ? "#FFFFFF" : "transparent"
                                }
                                Rectangle {   // カテゴリ色バブル
                                    width: 20; height: 20; radius: 10
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 5
                                    color: editor.categoryColors[index] || "#999"
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 27
                                    width: 58
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                    text: editor.categoryNames[index] || ""
                                    font.pixelSize: 9
                                    color: cFg
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: editor.categorySel = index
                                }
                            }
                        }
                        // 下枠線
                        Rectangle { anchors.bottom: parent.bottom
                                    width: parent.width; height: 1
                                    color: cBorder }
                    }

                    // フライアウト（実ブロック一覧）
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        BlockCanvas {
                            id: flyCanvas
                            anchors.fill: parent
                            ctrl: editor
                            flyout: true
                        }
                        // フライアウトのマウス処理。ドラッグ中は grab が続く
                        // ので、ws 上への移動・ドロップ・ws 外でのキャンセルも
                        // この MouseArea が全て追う。
                        MouseArea {
                            id: flyMa
                            anchors.fill: parent
                            hoverEnabled: true
                            onWheel: function(w) {
                                editor.flyWheel(w.angleDelta.y, flyCanvas.height)
                            }
                            onPressed: function(m) {
                                editor.flyMousePress(m.x, m.y, flyCanvas.width)
                            }
                            onPositionChanged: function(m) {
                                if (!pressed) return
                                var p = mapToItem(wsCanvas, m.x, m.y)
                                editor.wsDragMove(p.x, p.y)
                            }
                            onReleased: function(m) {
                                var p = mapToItem(wsCanvas, m.x, m.y)
                                if (p.x >= 0 && p.y >= 0 &&
                                    p.x < wsCanvas.width && p.y < wsCanvas.height)
                                    editor.wsMouseRelease(p.x, p.y)
                                else
                                    editor.flyMouseRelease()
                            }
                        }
                    }
                }

                // Costumes / Sounds タブ: 左にアセットセレクタ
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: editor.tab !== 0
                    color: cPanel
                    Column {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8
                        Text {
                            text: editor.tab === 1 ? "Costumes" : "Sounds"
                            color: cFgDim
                            font.pixelSize: 11; font.bold: true
                        }
                        // アセットタイル一覧
                        Flickable {
                            width: parent.width
                            height: parent.height - 54 - newBtn.height
                            contentHeight: assetCol.height
                            clip: true
                            Column {
                                id: assetCol
                                spacing: 8
                                Repeater {
                                    model: {
                                        var n = editor.layoutRev
                                        return editor.tab === 1
                                                ? editor.costumeItems()
                                                : editor.soundItems()
                                    }
                                    delegate: Rectangle {
                                        required property var modelData
                                        width: 200; height: 72
                                        radius: 6
                                        color: (editor.tab === 1
                                                ? modelData.current
                                                : editor.soundSel() === modelData.index)
                                               ? "#E4DEFF" : "white"
                                        border.color: (editor.tab === 1
                                                       ? modelData.current
                                                       : editor.soundSel() === modelData.index)
                                                      ? "#855CD6" : "#D9D9D9"
                                        border.width: (editor.tab === 1
                                                       ? modelData.current
                                                       : editor.soundSel() === modelData.index)
                                                      ? 2 : 1
                                        Image {
                                            visible: editor.tab === 1
                                            x: 8; y: 8; width: 56; height: 56
                                            source: "image://xyocostume/" +
                                                    editor.selTarget + "_" +
                                                    modelData.index + "/" +
                                                    editor.layoutRev
                                            fillMode: Image.PreserveAspectFit
                                        }
                                        Text {
                                            visible: editor.tab === 2
                                            x: 8; y: 24; text: "🔊"
                                            font.pixelSize: 24
                                        }
                                        Text {
                                            x: 72; y: 20
                                            width: 124
                                            text: modelData.name
                                            color: cFg
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            visible: editor.tab === 2
                                            x: 72; y: 38
                                            text: Number(modelData.secs).toFixed(2) + " s"
                                            color: cFgDim; font.pixelSize: 10
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: {
                                                if (editor.tab === 1)
                                                    editor.selectCostume(modelData.index)
                                                else
                                                    editor.selectSound(modelData.index)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        // + ボタン（新規アセット）
                        Rectangle {
                            id: newBtn
                            width: 44; height: 44; radius: 22
                            color: "#4C97FF"
                            anchors.horizontalCenter: parent.horizontalCenter
                            Text {
                                anchors.centerIn: parent
                                text: "+"; color: "white"
                                font.pixelSize: 24; font.bold: true
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: {
                                    if (editor.tab === 1) editor.addCostume()
                                    else editor.addSound()
                                }
                            }
                        }
                    }
                }
            }

            // ---- 中央: ワークスペース ----
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                BlockCanvas {
                    id: wsCanvas
                    anchors.fill: parent
                    ctrl: editor
                    flyout: false
                }
                MouseArea {
                    id: wsMa
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: function(m) {
                        if (m.button === Qt.RightButton) {
                            editor.openWsContextMenu(m.x, m.y)
                            ctxMenu.popup(m.x, m.y)
                            return
                        }
                        editor.wsMousePress(m.x, m.y)
                    }
                    onPositionChanged: function(m) {
                        if (pressed) editor.wsMouseMove(m.x, m.y)
                    }
                    onReleased: function(m) {
                        // ws 外（ツールボックス等）で解放 = 削除ドロップ
                        if (m.x < 0 || m.y < 0 ||
                            m.x >= wsCanvas.width || m.y >= wsCanvas.height)
                            editor.wsDropDelete()
                        else
                            editor.wsMouseRelease(m.x, m.y)
                    }
                    onWheel: function(w) {
                        editor.wsWheel(w.x, w.y, w.angleDelta.y)
                    }
                }
                // ズームボタン（右下）
                Column {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 12
                    spacing: 6
                    Repeater {
                        model: ["+", "−", "⊙"]
                        delegate: Rectangle {
                            width: 36; height: 36; radius: 18
                            color: zoomMa.containsMouse ? "#EDF1F5" : "#FFFFFF"
                            border.color: cBorder
                            Text {
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 18; color: cFg
                            }
                            MouseArea {
                                id: zoomMa; anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    if (index === 0) editor.zoomBy(1.25)
                                    else if (index === 1) editor.zoomBy(0.8)
                                    else editor.zoomReset()
                                }
                            }
                        }
                    }
                }
                // コンテキストメニュー（scratch-blocks showContextMenu_ 準拠:
                // C++ 側で項目を構築し ctxItems() で受け取る）
                Menu {
                    id: ctxMenu
                    Repeater {
                        model: {
                            var n = editor.layoutRev
                            return editor.ctxItems()
                        }
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.label
                            enabled: modelData.enabled
                            onTriggered: editor.runCtxItem(
                                index, wsCanvas.width, wsCanvas.height)
                        }
                    }
                }

                // ---- インラインフィールド編集 ----
                Rectangle {
                    id: fieldBox
                    visible: editor.fieldEditActive
                    x: editor.fieldEditRect.x
                    y: editor.fieldEditRect.y
                    width: Math.max(48, editor.fieldEditRect.width)
                    height: Math.max(26, editor.fieldEditRect.height)
                    color: "white"
                    border.color: "#4C97FF"
                    border.width: 2
                    radius: 4
                    z: 10
                    TextInput {
                        id: fieldInput
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        verticalAlignment: Text.AlignVCenter
                        font.pixelSize: Math.max(11, parent.height * 0.45)
                        color: cFg
                        selectByMouse: true
                        clip: true
                        text: editor.fieldEditText
                        onAccepted: editor.fieldEditCommit(text)
                        Keys.onEscapePressed: editor.fieldEditCancel()
                    }
                    onVisibleChanged: {
                        if (visible) {
                            fieldInput.text = editor.fieldEditText
                            fieldInput.forceActiveFocus()
                            fieldInput.selectAll()
                        }
                    }
                }

                // ---- ドロップダウンメニュー（blocklyDropDownDiv 準拠） ----
                Rectangle {
                    id: dropMenu
                    visible: editor.menuOpen
                    x: editor.menuPos.x
                    y: editor.menuPos.y
                    width: menuCol.implicitWidth + 16
                    height: menuCol.implicitHeight + 8
                    color: editor.menuBg
                    border.color: editor.menuBorder
                    border.width: 1
                    radius: 4
                    z: 10
                    Column {
                        id: menuCol
                        x: 8; y: 4
                        Repeater {
                            model: editor.menuItems
                            delegate: Rectangle {
                                width: Math.max(120, itemText.implicitWidth + 34)
                                height: 26
                                radius: 4
                                color: itemMa.containsMouse ? "#33000000"
                                        : "transparent"
                                Row {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    spacing: 6
                                    Text {
                                        text: index === editor.menuChecked ? "✓" : ""
                                        color: "white"
                                        font.pixelSize: 12; font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 12
                                    }
                                    Text {
                                        id: itemText
                                        text: modelData
                                        color: "white"
                                        font.pixelSize: 12; font.bold: true
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                                MouseArea {
                                    id: itemMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: editor.menuPick(index)
                                }
                            }
                        }
                    }
                }
                // メニュー/編集の外側クリックで閉じる
                MouseArea {
                    anchors.fill: parent
                    z: 9
                    visible: editor.menuOpen || editor.fieldEditActive
                    onPressed: function(m) {
                        if (editor.menuOpen) editor.menuDismiss()
                        if (editor.fieldEditActive)
                            editor.fieldEditCommit(fieldInput.text)
                        m.accepted = true
                    }
                }

                // ---- コメント編集（scratchComment 準拠の黄色吹き出し）----
                Rectangle {
                    id: cmtBox
                    property int rev: editor.layoutRev
                    visible: {
                        var n = rev
                        return editor.commentActive()
                    }
                    anchors.centerIn: parent
                    width: 300
                    height: cmtInput.implicitHeight + 60
                    color: "#FEF49C"
                    border.color: "#BCA903"
                    border.width: 1
                    radius: 4
                    z: 11
                    TextInput {
                        id: cmtInput
                        anchors.fill: parent
                        anchors.margins: 8
                        anchors.bottomMargin: 32
                        wrapMode: TextInput.Wrap
                        font.pixelSize: 12
                        color: "#000000"
                        clip: true
                        onAccepted: editor.commentCommit(text)
                        Keys.onEscapePressed: editor.commentCancel()
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 6
                        width: 44; height: 22; radius: 4
                        color: "#4C97FF"
                        Text { anchors.centerIn: parent; text: "OK"
                               color: "white"; font.pixelSize: 11 }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: editor.commentCommit(cmtInput.text)
                        }
                    }
                    onVisibleChanged: {
                        if (visible) {
                            cmtInput.text = editor.commentText()
                            cmtInput.forceActiveFocus()
                            cmtInput.selectAll()
                        }
                    }
                }

                // ---- Costumes / Sounds 詳細エリア ----
                Rectangle {
                    id: assetDetail
                    anchors.fill: parent
                    visible: editor.tab !== 0
                    color: cWsBg
                    z: 5
                    // プレビュー描画は C++ 側の image provider を利用。
                    // costume タブのみ画像、sound は再生ボタン。
                    // layoutRev を参照してモデル変更時に再評価させる
                    property int rev: editor.layoutRev
                    property var items: {
                        var n = rev
                        return editor.tab === 1 ? editor.costumeItems()
                                : editor.soundItems()
                    }
                    property int sel: editor.tab === 1 ? editor.costumeSel()
                                      : editor.soundSel()
                    property var cur: (sel >= 0 && sel < items.length)
                                      ? items[sel] : null
                    Column {
                        anchors.centerIn: parent
                        spacing: 14
                        // プレビュー（コスチューム画像 or 再生ボタン）
                        Rectangle {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 320; height: 240; radius: 8
                            color: "white"; border.color: cBorder
                            Image {
                                visible: editor.tab === 1
                                anchors.fill: parent
                                anchors.margins: 10
                                source: assetDetail.cur
                                        ? "image://xyocostume/" +
                                          editor.selTarget + "_" +
                                          assetDetail.cur.index + "/" +
                                          editor.layoutRev
                                        : ""
                                fillMode: Image.PreserveAspectFit
                            }
                            Rectangle {
                                visible: editor.tab === 2
                                anchors.centerIn: parent
                                width: 64; height: 64; radius: 32
                                color: playMa.containsMouse ? "#3D8534" : cGreen
                                Text {
                                    anchors.centerIn: parent
                                    text: "▶"; color: "white"
                                    font.pixelSize: 26
                                }
                                MouseArea {
                                    id: playMa; anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: editor.playSound(
                                        assetDetail.sel)
                                }
                            }
                        }
                        // 名前 + 操作ボタン
                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 8
                            TextField {
                                id: assetName
                                width: 220
                                text: assetDetail.cur ? assetDetail.cur.name : ""
                                font.pixelSize: 13
                                selectByMouse: true
                                onAccepted: {
                                    if (!assetDetail.cur) return
                                    if (editor.tab === 1)
                                        editor.renameCostume(assetDetail.sel, text)
                                    else
                                        editor.renameSound(assetDetail.sel, text)
                                }
                            }
                            Rectangle {
                                width: 84; height: 30; radius: 15
                                color: dupMa.containsMouse ? "#EDF1F5" : "white"
                                border.color: cBorder
                                Text { anchors.centerIn: parent
                                       text: "Duplicate"
                                       font.pixelSize: 11; color: cFg }
                                MouseArea {
                                    id: dupMa; anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        if (!assetDetail.cur) return
                                        if (editor.tab === 1)
                                            editor.duplicateCostume(assetDetail.sel)
                                        else
                                            editor.duplicateSound(assetDetail.sel)
                                    }
                                }
                            }
                            Rectangle {
                                width: 70; height: 30; radius: 15
                                color: delMa.containsMouse ? "#FFF0EB" : "white"
                                border.color: "#FF8C1A"
                                Text { anchors.centerIn: parent
                                       text: "Delete"
                                       font.pixelSize: 11; color: "#FF8C1A" }
                                MouseArea {
                                    id: delMa; anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        if (!assetDetail.cur) return
                                        if (editor.tab === 1)
                                            editor.deleteCostume(assetDetail.sel)
                                        else
                                            editor.deleteSound(assetDetail.sel)
                                    }
                                }
                            }
                        }
                    }
                    // キー入力を ws のキーハンドラから遮断
                    MouseArea { anchors.fill: parent }
                }
            }

            // ---- 右列: 緑旗/停止 + ステージ + スプライト ----
            ColumnLayout {
                Layout.preferredWidth: 500
                Layout.maximumWidth: 500
                Layout.fillHeight: true
                spacing: 0

                // ステージメニュー（旗・停止・拡大）
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    color: cPanel
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 8
                        // 緑旗
                        Rectangle {
                            width: 36; height: 36; radius: 18
                            color: flagMa.containsMouse ? "#3D8534" : cGreen
                            Text {
                                anchors.centerIn: parent
                                text: "▶"; color: "white"; font.pixelSize: 15
                            }
                            MouseArea {
                                id: flagMa; anchors.fill: parent
                                hoverEnabled: true
                                onClicked: editor.greenFlag()
                            }
                        }
                        // 停止
                        Rectangle {
                            width: 36; height: 36; radius: 18
                            color: stopMa.containsMouse ? "#D24C4C" : cStop
                            Rectangle {
                                anchors.centerIn: parent
                                width: 14; height: 14; radius: 2
                                color: "white"
                            }
                            MouseArea {
                                id: stopMa; anchors.fill: parent
                                hoverEnabled: true
                                onClicked: editor.stopAll()
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Rectangle { anchors.bottom: parent.bottom
                                width: parent.width; height: 1
                                color: cBorder }
                }

                // ステージ（4:3、radius 8 + 1px 境界）
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: width * 0.75
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 10
                        radius: 8
                        color: "white"
                        border.color: cBorder
                        clip: true
                        StageItem {
                            id: stageItem
                            anchors.fill: parent
                            ctrl: editor
                            onWidthChanged: editor.setStageDisplay(width, height)
                            onHeightChanged: editor.setStageDisplay(width, height)
                            Component.onCompleted: editor.setStageDisplay(width, height)
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: 10
                        hoverEnabled: true
                        onPressed: function(m) {
                            var p = mapToItem(stageItem, m.x, m.y)
                            editor.stageMouseDown(p.x, p.y)
                        }
                        onPositionChanged: function(m) {
                            var p = mapToItem(stageItem, m.x, m.y)
                            editor.stageMouseMove(p.x, p.y)
                        }
                        onReleased: function(m) {
                            var p = mapToItem(stageItem, m.x, m.y)
                            editor.stageMouseUp(p.x, p.y)
                        }
                    }
                }

                // スプライト情報パネル（sprite-info.jsx 準拠:
                // 名前/x/y + Show トグル + Size + Direction）
                Rectangle {
                    id: spInfoPanel
                    Layout.fillWidth: true
                    Layout.preferredHeight: 76
                    color: cPanel
                    Rectangle { anchors.top: parent.top
                                width: parent.width; height: 1
                                color: cBorder }
                    property bool isStage: editor.selTarget === 0
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.topMargin: 6
                        anchors.bottomMargin: 8
                        spacing: 4
                        RowLayout {
                            spacing: 8
                            Text {
                                text: "Sprite"
                                color: cFg; font.pixelSize: 11; font.bold: true
                            }
                            Rectangle {
                                Layout.preferredWidth: 128
                                Layout.preferredHeight: 28
                                radius: 14
                                color: "white"; border.color: cBorder
                                TextInput {
                                    id: spNameInput
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 10
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 12; color: cFg
                                    text: editor.spriteInfo.length > 0
                                          ? editor.spriteInfo[0] : ""
                                    enabled: !spInfoPanel.isStage
                                    onAccepted: editor.setSpriteName(text)
                                    onActiveFocusChanged:
                                        if (!activeFocus)
                                            editor.setSpriteName(text)
                                }
                            }
                            Item { Layout.fillWidth: true }
                            // x / y 入力
                            RowLayout {
                                spacing: 4
                                visible: editor.spriteInfo.length > 2
                                Text { text: "x"; color: cFg
                                       font.pixelSize: 11; font.bold: true }
                                Rectangle {
                                    Layout.preferredWidth: 52
                                    Layout.preferredHeight: 28; radius: 14
                                    color: "white"; border.color: cBorder
                                    TextInput {
                                        anchors.fill: parent
                                        horizontalAlignment: TextInput.AlignHCenter
                                        verticalAlignment: TextInput.AlignVCenter
                                        font.pixelSize: 11; color: cFg
                                        validator: DoubleValidator {}
                                        text: Math.round(
                                                  (editor.spriteInfo[1] || 0))
                                        onAccepted: editor.setSpriteX(
                                                        parseFloat(text))
                                        onActiveFocusChanged:
                                            if (!activeFocus)
                                                editor.setSpriteX(
                                                    parseFloat(text))
                                    }
                                }
                                Text { text: "y"; color: cFg
                                       font.pixelSize: 11; font.bold: true }
                                Rectangle {
                                    Layout.preferredWidth: 52
                                    Layout.preferredHeight: 28; radius: 14
                                    color: "white"; border.color: cBorder
                                    TextInput {
                                        anchors.fill: parent
                                        horizontalAlignment: TextInput.AlignHCenter
                                        verticalAlignment: TextInput.AlignVCenter
                                        font.pixelSize: 11; color: cFg
                                        validator: DoubleValidator {}
                                        text: Math.round(
                                                  (editor.spriteInfo[2] || 0))
                                        onAccepted: editor.setSpriteY(
                                                        parseFloat(text))
                                        onActiveFocusChanged:
                                            if (!activeFocus)
                                                editor.setSpriteY(
                                                    parseFloat(text))
                                    }
                                }
                            }
                        }
                        // 行2: Show トグル + Size + Direction（stage は非表示）
                        RowLayout {
                            spacing: 8
                            visible: editor.spriteInfo.length > 2
                            Text { text: "Show"; color: cFgDim
                                   font.pixelSize: 11 }
                            // 連結トグル（toggle-buttons.css 準拠）
                            Rectangle {
                                Layout.preferredWidth: 64
                                Layout.preferredHeight: 28
                                radius: 4; color: "white"
                                border.color: cBorder
                                Row {
                                    anchors.fill: parent
                                    Rectangle {   // Show（左）
                                        width: 32; height: 28; radius: 4
                                        color: editor.spriteInfo[5]
                                               ? "#CDDCF7" : "white"
                                        Text { anchors.centerIn: parent
                                               text: "👁"; font.pixelSize: 12 }
                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked:
                                                editor.setSpriteVisible(true)
                                        }
                                    }
                                    Rectangle { width: 1; height: 28
                                                color: cBorder }
                                    Rectangle {   // Hide（右）
                                        width: 31; height: 28; radius: 4
                                        color: !editor.spriteInfo[5]
                                               ? "#CDDCF7" : "white"
                                        Text { anchors.centerIn: parent
                                               text: "👁"
                                               color: "#999"
                                               font.pixelSize: 12 }
                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked:
                                                editor.setSpriteVisible(false)
                                        }
                                    }
                                }
                            }
                            Item { Layout.fillWidth: true }
                            Text { text: "Size"; color: cFgDim
                                   font.pixelSize: 11 }
                            Rectangle {
                                Layout.preferredWidth: 52
                                Layout.preferredHeight: 28; radius: 14
                                color: "white"; border.color: cBorder
                                TextInput {
                                    anchors.fill: parent
                                    horizontalAlignment: TextInput.AlignHCenter
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 11; color: cFg
                                    validator: DoubleValidator {}
                                    text: Math.round(editor.spriteInfo[3] || 0)
                                    onAccepted: editor.setSpriteSize(
                                                    parseFloat(text))
                                    onActiveFocusChanged:
                                        if (!activeFocus)
                                            editor.setSpriteSize(
                                                parseFloat(text))
                                }
                            }
                            Text { text: "Direction"; color: cFgDim
                                   font.pixelSize: 11 }
                            Rectangle {
                                Layout.preferredWidth: 52
                                Layout.preferredHeight: 28; radius: 14
                                color: "white"; border.color: cBorder
                                TextInput {
                                    anchors.fill: parent
                                    horizontalAlignment: TextInput.AlignHCenter
                                    verticalAlignment: TextInput.AlignVCenter
                                    font.pixelSize: 11; color: cFg
                                    validator: DoubleValidator {}
                                    text: Math.round(editor.spriteInfo[4] || 0)
                                    onAccepted: editor.setSpriteDirection(
                                                    parseFloat(text))
                                    onActiveFocusChanged:
                                        if (!activeFocus)
                                            editor.setSpriteDirection(
                                                parseFloat(text))
                                }
                            }
                        }
                    }
                }

                // スプライトセレクタ
                Rectangle {
                    id: sprPanel
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: cBody
                    Flow {
                        id: sprFlow
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8
                        Repeater {
                            model: editor.spriteNames.length
                            delegate: Rectangle {
                                id: tile
                                required property int index
                                width: 88; height: 100
                                radius: 6
                                // ドラッグ中は半透明（旧 editor の sdr_ ghost 準拠）
                                opacity: tileMa.dragging ? 0.4 : 1.0
                                color: editor.selTarget === index ? cPanel : "#F5F7FA"
                                border.color: editor.selTarget === index
                                              ? cMenu : cBorder
                                border.width: editor.selTarget === index ? 2 : 1
                                Rectangle {   // コスチュームサムネイル
                                    width: 48; height: 40; radius: 6
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 8
                                    color: index === 0 ? "#D9E3F2" : "#FFFFFF"
                                    border.color: cBorder
                                    clip: true
                                    Image {
                                        anchors.fill: parent
                                        anchors.margins: 3
                                        fillMode: Image.PreserveAspectFit
                                        source: "image://xyocostume/" + index
                                                + "/" + editor.layoutRev
                                        asynchronous: true
                                    }
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: 54
                                    width: parent.width - 8
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideRight
                                    text: editor.spriteNames[index] || ""
                                    font.pixelSize: 11; color: cFg
                                }
                                // 削除バッジ（ステージ以外・選択時）
                                Rectangle {
                                    visible: index !== 0 && editor.selTarget === index
                                    width: 18; height: 18; radius: 9
                                    x: parent.width - 14; y: -6
                                    color: cStop
                                    Text { anchors.centerIn: parent; text: "×"
                                           color: "white"; font.pixelSize: 12 }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: editor.deleteSprite(index)
                                    }
                                }
                                MouseArea {
                                    id: tileMa
                                    anchors.fill: parent
                                    z: -1
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    property bool dragging: false
                                    property real sx: 0
                                    property real sy: 0
                                    onPressed: function(m) {
                                        if (m.button === Qt.RightButton) {
                                            if (index !== 0) {
                                                sprCtxMenu.tileIndex = index
                                                sprCtxMenu.popup(
                                                    m.x, m.y)
                                            }
                                            return
                                        }
                                        sx = m.x; sy = m.y; dragging = false
                                        sprDragItem.tileIndex = index
                                    }
                                    onPositionChanged: function(m) {
                                        if (!pressed) return
                                        if (!dragging &&
                                            Math.abs(m.x - sx) +
                                            Math.abs(m.y - sy) < 6) return
                                        if (index === 0) return  // Stage は動かせない
                                        dragging = true
                                        var gp = tile.mapToItem(sprPanel,
                                                                m.x, m.y)
                                        sprDragItem.x = gp.x - 44
                                        sprDragItem.y = gp.y - 50
                                        sprDragItem.visible = true
                                        sprDragItem.ins =
                                            sprInsertIndex(gp.x, gp.y)
                                        sprInsLine.x =
                                            sprInsLineX(sprDragItem.ins) - 1
                                        sprInsLine.y =
                                            sprInsLineY(sprDragItem.ins)
                                        sprInsLine.visible = true
                                    }
                                    onReleased: function(m) {
                                        if (dragging) {
                                            editor.spriteReorder(
                                                index, sprDragItem.ins)
                                        }
                                        dragging = false
                                        sprDragItem.visible = false
                                        sprInsLine.visible = false
                                    }
                                    onClicked: function(m) {
                                        if (m.button === Qt.LeftButton)
                                            editor.selTarget = index
                                    }
                                    // 挿入位置計算（旧 spriteInsertIndex 準拠:
                                    // グリッド、行の先頭〜末尾を半分で区切る。
                                    // mx,my は sprPanel 座標（margin 10 込み））
                                    function sprInsertIndex(mx, my) {
                                        var n = editor.spriteNames.length - 1
                                        if (n <= 0) return 0
                                        var tw = 88, th = 100, gap = 8
                                        var cols = Math.max(1,
                                            Math.floor(
                                                (sprFlow.width - 20) / (tw + gap)))
                                        var relY = my - 10, relX = mx - 10
                                        var row = Math.max(0, Math.min(
                                            Math.floor(
                                                (relY + th / 2) / (th + gap)),
                                            Math.floor((n - 1) / cols) + 1))
                                        var first = row * cols
                                        var last = Math.min(n, first + cols)
                                        if (first >= n) return n
                                        var ins = first
                                        for (var i = first; i < last; ++i) {
                                            var cx = (i % cols) * (tw + gap) + tw / 2
                                            if (relX < cx) { ins = i; break }
                                            ins = i + 1
                                        }
                                        return Math.max(0, Math.min(ins, n))
                                    }
                                    function sprInsLineX(ins) {
                                        var tw = 88, gap = 8
                                        var cols = Math.max(1,
                                            Math.floor(
                                                (sprFlow.width - 20) / (tw + gap)))
                                        return 10 + (ins % cols) * (tw + gap) - gap / 2
                                    }
                                    function sprInsLineY(ins) {
                                        var th = 100, gap = 8
                                        var cols = Math.max(1,
                                            Math.floor(
                                                (sprFlow.width - 20) / (88 + gap)))
                                        return 10 + Math.floor(ins / cols) * (th + gap)
                                    }
                                }
                            }
                        }
                    }
                    // 挿入位置インジケータ（2px 紫線、旧 sdr_ 準拠）
                    Rectangle {
                        id: sprInsLine
                        visible: false
                        width: 2; height: 100
                        color: "#855CD6"
                        z: 5
                    }
                    // ドラッグゴースト
                    Rectangle {
                        id: sprDragItem
                        visible: false
                        property int tileIndex: -1
                        property int ins: 0
                        width: 88; height: 100; radius: 6
                        color: "white"; border.color: cMenu; border.width: 2
                        opacity: 0.75
                        z: 6
                        Image {
                            width: 48; height: 40
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 8
                            fillMode: Image.PreserveAspectFit
                            source: sprDragItem.tileIndex >= 0
                                    ? "image://xyocostume/" +
                                      sprDragItem.tileIndex + "/" +
                                      editor.layoutRev : ""
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 54
                            width: parent.width - 8
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: sprDragItem.tileIndex >= 0
                                  ? editor.spriteNames[sprDragItem.tileIndex] : ""
                            font.pixelSize: 11; color: cFg
                        }
                    }
                    // スプライト右クリックメニュー
                    // （context-menu.css 準拠: duplicate / export / delete）
                    Menu {
                        id: sprCtxMenu
                        property int tileIndex: -1
                        MenuItem {
                            text: "duplicate"
                            onTriggered: editor.duplicateSprite(sprCtxMenu.tileIndex)
                        }
                        MenuItem {
                            text: "export"
                            onTriggered: editor.exportSprite(sprCtxMenu.tileIndex)
                        }
                        MenuItem {
                            text: "delete"
                            onTriggered: editor.deleteSprite(sprCtxMenu.tileIndex)
                        }
                    }
                    // Add Sprite ボタン（ActionMenu 44px 青丸 +
                    // topMenu which 3 準拠の Choose/Paint/Surprise/Upload）
                    Rectangle {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 8
                        width: 44; height: 44; radius: 22
                        color: addSprMa.containsMouse ? "#3E7BD8" : "#4C97FF"
                        Text { anchors.centerIn: parent; text: "＋"
                               color: "white"; font.pixelSize: 22 }
                        MouseArea {
                            id: addSprMa; anchors.fill: parent
                            hoverEnabled: true
                            onClicked: addSprMenu.popup(
                                -addSprMenu.width + width, -addSprMenu.height)
                        }
                        Menu {
                            id: addSprMenu
                            MenuItem { text: "Choose a Sprite"
                                       onTriggered: editor.addSprite() }
                            MenuItem { text: "Paint"
                                       onTriggered: editor.addSprite() }
                            MenuItem { text: "Surprise"
                                       onTriggered: editor.addSprite() }
                            MenuItem { text: "Upload"
                                       onTriggered: editor.addSprite() }
                        }
                    }
                }
            }
        }

        // ---- バックパックバー ----
        Rectangle {
            id: bpBar
            Layout.fillWidth: true
            property int rev: editor.layoutRev
            property bool open: {
                var n = rev
                return editor.backpackOpen()
            }
            Layout.preferredHeight: open ? 130 : 30
            color: cPanel
            border.color: cBorder
            border.width: 1
            // ヘッダー（クリックで開閉）
            Rectangle {
                anchors.top: parent.top
                width: parent.width; height: 30
                color: "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "Backpack"
                    color: cFg; font.pixelSize: 13; font.bold: true
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: editor.toggleBackpack()
                }
            }
            // 項目一覧
            Flickable {
                visible: bpBar.open
                anchors.top: parent.top
                anchors.topMargin: 34
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 6
                contentWidth: bpRow.width
                clip: true
                Row {
                    id: bpRow
                    spacing: 8
                    Text {
                        property int rev: bpBar.rev
                        visible: { var n = rev; return editor.backpackCount() === 0 }
                        text: "Backpack is empty"
                        color: cFgDim; font.pixelSize: 13
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Repeater {
                        model: {
                            var n = bpBar.rev
                            return editor.backpackItems()
                        }
                        delegate: Rectangle {
                            required property var modelData
                            required property int index
                            width: 96; height: 84
                            radius: 4
                            color: bpItemMa.containsMouse ? "#EDF1F5" : "#F9F9F9"
                            border.color: cBorder
                            Text {
                                anchors.centerIn: parent
                                width: 88
                                text: modelData.label
                                color: cFg; font.pixelSize: 10
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                maximumLineCount: 3
                                wrapMode: Text.WordWrap
                            }
                            MouseArea {
                                id: bpItemMa
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: editor.backpackPasteAt(
                                    index, wsCanvas.width, wsCanvas.height)
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- メニューボタン ----
    component MenuButton: Item {
        property string text: ""
        property var menuItems: []
        width: mbLabel.implicitWidth + 24; height: 44
        Text {
            id: mbLabel
            anchors.centerIn: parent
            text: parent.text
            color: "white"; font.pixelSize: 13
        }
        Rectangle {
            anchors.fill: parent
            color: "#FFFFFF"; opacity: mbMa.containsMouse ? 0.15 : 0
        }
        MouseArea {
            id: mbMa; anchors.fill: parent
            hoverEnabled: true
            onClicked: mbMenu.open()
        }
        Menu {
            id: mbMenu
            y: 44
            Repeater {
                model: menuItems
                MenuItem {
                    text: modelData.label
                    onTriggered: {
                        if (modelData.act === "save") editor.save()
                        else if (modelData.act === "undo") editor.undo()
                        else if (modelData.act === "redo") editor.redo()
                    }
                }
            }
        }
    }

    // ---- Make a Block ダイアログ（custom-procedures 準拠）----
    Rectangle {
        id: makeBlockOverlay
        anchors.fill: parent
        property int rev: editor.layoutRev   // mbOpen/mbWarp 再評価フック
        visible: { var n = rev; return editor.mbOpen() }
        color: "#E6253E8C"                // $ui-modal-overlay 青90%
        z: 20
        MouseArea { anchors.fill: parent }   // モーダル: 裏を遮断
        Rectangle {
            width: 700; height: 340
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 50
            color: "transparent"
            border.color: "#59FFFFFF"
            border.width: 4; radius: 8
            Row {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 16
                // 左ピンク列: プレビューピル
                Rectangle {
                    width: 240; height: 300
                    color: "#ECA0D2"
                    radius: 4
                    Rectangle {
                        anchors.centerIn: parent
                        width: prevTxt.implicitWidth + 36; height: 44
                        radius: 22; color: "#FF6680"
                        Text {
                            id: prevTxt
                            anchors.centerIn: parent
                            text: {
                                var s = ""
                                var ps = editor.mbParts()
                                for (var i = 0; i < ps.length; i++) {
                                    if (ps[i].kind === 0)
                                        s += (s ? " " : "") + ps[i].text
                                    else
                                        s += (ps[i].kind === 2 ? " ◇" : " ○")
                                }
                                return s || "block name"
                            }
                            color: "white"; font.pixelSize: 14; font.bold: true
                        }
                    }
                }
                // 右列: パーツ編集 + ボタン
                Column {
                    width: 700 - 24 - 240 - 16
                    height: 300
                    spacing: 10
                    Text {
                        text: "Make a Block"
                        color: "white"; font.pixelSize: 16; font.bold: true
                    }
                    // parts 一覧
                    Flickable {
                        width: parent.width; height: 128
                        contentHeight: partCol.height
                        clip: true
                        Column {
                            id: partCol
                            width: parent.width - 8
                            spacing: 6
                            Repeater {
                                model: {
                                    var n = editor.layoutRev  // 再評価フック
                                    return editor.mbOpen() ? editor.mbParts() : []
                                }
                                delegate: Row {
                                    required property var modelData
                                    required property int index
                                    spacing: 6
                                    Rectangle {
                                        width: 250; height: 30; radius: 15
                                        color: "white"; border.color: cBorder
                                        TextInput {
                                            anchors.fill: parent
                                            anchors.leftMargin: 12
                                            anchors.rightMargin: 12
                                            verticalAlignment: Text.AlignVCenter
                                            font.pixelSize: 12; color: cFg
                                            selectByMouse: true; clip: true
                                            text: modelData.text
                                            onEditingFinished:
                                                editor.mbSetPartText(index, text)
                                        }
                                    }
                                    Text {
                                        text: modelData.kind === 0 ? "label"
                                              : modelData.kind === 1 ? "input"
                                              : "bool"
                                        color: "white"; font.pixelSize: 11
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Rectangle {
                                        width: 30; height: 30; radius: 15
                                        color: "transparent"
                                        Text { anchors.centerIn: parent
                                               text: "✕"; color: "white"
                                               font.pixelSize: 13 }
                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: editor.mbRemovePart(index)
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // Add input / label
                    Row {
                        spacing: 8
                        Repeater {
                            model: [
                                { t: "Add input string or number", k: 1, c: "#4C97FF" },
                                { t: "Add input boolean", k: 2, c: "#9966FF" },
                                { t: "Add label text", k: 0, c: "#CF63CF" }
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                width: addTxt.implicitWidth + 20; height: 34
                                radius: 17; color: modelData.c
                                Text {
                                    id: addTxt; anchors.centerIn: parent
                                    text: modelData.t
                                    color: "white"; font.pixelSize: 12
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: editor.mbAddPart(modelData.k)
                                }
                            }
                        }
                    }
                    // warp
                    Row {
                        spacing: 8
                        Rectangle {
                            width: 16; height: 16; radius: 2
                            property bool w: { var n = makeBlockOverlay.rev
                                               return editor.mbWarp() }
                            color: w ? "#4C97FF" : "white"
                            border.color: w ? "#4C97FF" : cBorder
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                anchors.centerIn: parent
                                text: parent.w ? "✓" : ""
                                color: "white"; font.pixelSize: 11
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: editor.mbSetWarp(!editor.mbWarp())
                            }
                        }
                        Text {
                            text: "Run without screen refresh"
                            color: "white"; font.pixelSize: 12
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                    // Cancel / OK
                    Row {
                        spacing: 10
                        anchors.right: parent.right
                        Rectangle {
                            width: 80; height: 36; radius: 4
                            color: "transparent"; border.color: "white"
                            Text { anchors.centerIn: parent; text: "Cancel"
                                   color: "white"; font.pixelSize: 13 }
                            MouseArea { anchors.fill: parent
                                        onClicked: editor.mbCancel() }
                        }
                        Rectangle {
                            width: 80; height: 36; radius: 4
                            color: "#FF8C1A"
                            Text { anchors.centerIn: parent; text: "OK"
                                   color: "white"; font.pixelSize: 13
                                   font.bold: true }
                            MouseArea { anchors.fill: parent
                                        onClicked: editor.mbOk() }
                        }
                    }
                }
            }
        }
    }

}
