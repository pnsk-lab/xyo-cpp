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

                // Costumes / Sounds タブ（v1: プレースホルダ）
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: editor.tab !== 0
                    color: cPanel
                    Text {
                        anchors.centerIn: parent
                        text: editor.tab === 1 ? "Costumes" : "Sounds"
                        color: cFgDim; font.pixelSize: 13
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
                // コンテキストメニュー
                Menu {
                    id: ctxMenu
                    MenuItem { text: "Delete block"; onTriggered: editor.deleteSelected() }
                    MenuItem { text: "Duplicate"; onTriggered: editor.duplicateSelected() }
                    MenuSeparator {}
                    MenuItem { text: "Clean up Blocks"; onTriggered: editor.cleanupBlocks() }
                    MenuItem { text: "Undo"; enabled: editor.canUndo; onTriggered: editor.undo() }
                    MenuItem { text: "Redo"; enabled: editor.canRedo; onTriggered: editor.redo() }
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
                                color: itemMa.containsMouse ? "rgba(0,0,0,0.2)"
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

                // スプライト情報パネル
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    color: cPanel
                    Rectangle { anchors.top: parent.top
                                width: parent.width; height: 1
                                color: cBorder }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 8
                        Text {
                            text: editor.spriteInfo.length > 0
                                  ? editor.spriteInfo[0] : ""
                            color: cFg; font.pixelSize: 13; font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            visible: editor.spriteInfo.length > 2
                            text: "x: " + Math.round(editor.spriteInfo[1] || 0)
                                  + "  y: " + Math.round(editor.spriteInfo[2] || 0)
                            color: cFgDim; font.pixelSize: 11
                        }
                    }
                }

                // スプライトセレクタ
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: cBody
                    Flow {
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8
                        Repeater {
                            model: editor.spriteNames.length
                            delegate: Rectangle {
                                width: 88; height: 100
                                radius: 6
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
                                    anchors.fill: parent
                                    z: -1
                                    onClicked: editor.selTarget = index
                                }
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

}
