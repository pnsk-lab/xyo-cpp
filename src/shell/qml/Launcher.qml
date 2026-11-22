import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    width: 1120
    height: 760

    // 落ち着いたデスクトップ・ツール調のカラーパレット（xyo / Scratch 系の紫をアクセントに）
    QtObject {
        id: pal
        readonly property color windowBg:     "#F3F4F6"
        readonly property color surface:      "#FFFFFF"
        readonly property color surfaceAlt:   "#F8F9FB"
        readonly property color sidebarBg:    "#ECEEF2"
        readonly property color sidebarHover: "#E1E4EB"
        readonly property color sidebarSel:   "#E3DCF8"
        readonly property color border:       "#D5D9E2"
        readonly property color borderLight:  "#E6E9EF"
        readonly property color textPrimary:  "#1C2029"
        readonly property color textSecond:   "#5B6374"
        readonly property color textMuted:    "#8B93A4"
        readonly property color accent:       "#7C4DFF"
        readonly property color accentHover:  "#6B3EE8"
        readonly property color accentSoft:   "#EFEAFF"
        readonly property color accentBorder: "#C5B3F9"
        readonly property color rowSelBg:     "#E9E3FC"
        readonly property color rowHoverBg:   "#F2F4F8"
        readonly property color folderMain:   "#E8B857"
        readonly property color folderTab:    "#D19E3B"
        readonly property color ok:           "#2E9E5B"
        readonly property color danger:       "#D64545"
    }

    readonly property bool listMode: launcher.listView
    readonly property int gridCols: Math.max(1, Math.floor((contentPane.width - 28 + 12) / (176 + 12)))

    // 軽量ベクターアイコン（Canvas で 1.5px ストロークの線画アイコンを描画）
    component LineIcon: Canvas {
        property string kind: ""
        property color strokeColor: pal.textSecond
        property real strokeWidth: 1.5
        width: 16; height: 16
        onKindChanged: requestPaint()
        onStrokeColorChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = strokeColor
            ctx.fillStyle = strokeColor
            ctx.lineWidth = strokeWidth
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            var w = width, h = height
            ctx.beginPath()
            if (kind === "back") {
                ctx.moveTo(w * 0.62, h * 0.22); ctx.lineTo(w * 0.32, h * 0.50); ctx.lineTo(w * 0.62, h * 0.78)
            } else if (kind === "forward") {
                ctx.moveTo(w * 0.38, h * 0.22); ctx.lineTo(w * 0.68, h * 0.50); ctx.lineTo(w * 0.38, h * 0.78)
            } else if (kind === "up") {
                ctx.moveTo(w * 0.50, h * 0.78); ctx.lineTo(w * 0.50, h * 0.24)
                ctx.moveTo(w * 0.24, h * 0.46); ctx.lineTo(w * 0.50, h * 0.22); ctx.lineTo(w * 0.76, h * 0.46)
            } else if (kind === "reload") {
                ctx.arc(w * 0.5, h * 0.5, w * 0.31, -0.4, Math.PI * 1.45)
                ctx.moveTo(w * 0.74, h * 0.22); ctx.lineTo(w * 0.80, h * 0.40); ctx.lineTo(w * 0.62, h * 0.40)
            } else if (kind === "search") {
                ctx.arc(w * 0.44, h * 0.44, w * 0.26, 0, Math.PI * 2)
                ctx.moveTo(w * 0.64, h * 0.64); ctx.lineTo(w * 0.84, h * 0.84)
            } else if (kind === "list") {
                ctx.moveTo(w * 0.20, h * 0.30); ctx.lineTo(w * 0.82, h * 0.30)
                ctx.moveTo(w * 0.20, h * 0.50); ctx.lineTo(w * 0.82, h * 0.50)
                ctx.moveTo(w * 0.20, h * 0.70); ctx.lineTo(w * 0.82, h * 0.70)
            } else if (kind === "grid") {
                ctx.rect(w * 0.18, h * 0.18, w * 0.25, h * 0.25)
                ctx.rect(w * 0.57, h * 0.18, w * 0.25, h * 0.25)
                ctx.rect(w * 0.18, h * 0.57, w * 0.25, h * 0.25)
                ctx.rect(w * 0.57, h * 0.57, w * 0.25, h * 0.25)
            } else if (kind === "settings") {
                ctx.arc(w * 0.5, h * 0.5, w * 0.17, 0, Math.PI * 2)
                for (var i = 0; i < 6; ++i) {
                    var a = i * Math.PI / 3
                    ctx.moveTo(w * 0.5 + Math.cos(a) * w * 0.25, h * 0.5 + Math.sin(a) * h * 0.25)
                    ctx.lineTo(w * 0.5 + Math.cos(a) * w * 0.38, h * 0.5 + Math.sin(a) * h * 0.38)
                }
            } else if (kind === "home") {
                ctx.moveTo(w * 0.18, h * 0.48); ctx.lineTo(w * 0.50, h * 0.20); ctx.lineTo(w * 0.82, h * 0.48)
                ctx.moveTo(w * 0.28, h * 0.42); ctx.lineTo(w * 0.28, h * 0.80); ctx.lineTo(w * 0.72, h * 0.80); ctx.lineTo(w * 0.72, h * 0.42)
            } else if (kind === "folder") {
                ctx.moveTo(w * 0.16, h * 0.26); ctx.lineTo(w * 0.42, h * 0.26); ctx.lineTo(w * 0.50, h * 0.34)
                ctx.lineTo(w * 0.84, h * 0.34); ctx.lineTo(w * 0.84, h * 0.76); ctx.lineTo(w * 0.16, h * 0.76); ctx.closePath()
            } else if (kind === "download") {
                ctx.moveTo(w * 0.50, h * 0.20); ctx.lineTo(w * 0.50, h * 0.64)
                ctx.moveTo(w * 0.30, h * 0.46); ctx.lineTo(w * 0.50, h * 0.66); ctx.lineTo(w * 0.70, h * 0.46)
                ctx.moveTo(w * 0.22, h * 0.80); ctx.lineTo(w * 0.78, h * 0.80)
            } else if (kind === "star") {
                ctx.moveTo(w * 0.50, h * 0.16); ctx.lineTo(w * 0.60, h * 0.38); ctx.lineTo(w * 0.84, h * 0.41)
                ctx.lineTo(w * 0.66, h * 0.58); ctx.lineTo(w * 0.71, h * 0.82); ctx.lineTo(w * 0.50, h * 0.70)
                ctx.lineTo(w * 0.29, h * 0.82); ctx.lineTo(w * 0.34, h * 0.58); ctx.lineTo(w * 0.16, h * 0.41)
                ctx.lineTo(w * 0.40, h * 0.38); ctx.closePath()
            } else if (kind === "play") {
                ctx.moveTo(w * 0.30, h * 0.22); ctx.lineTo(w * 0.78, h * 0.50); ctx.lineTo(w * 0.30, h * 0.78); ctx.closePath()
                ctx.stroke(); ctx.fill(); return
            } else if (kind === "edit") {
                ctx.moveTo(w * 0.22, h * 0.78); ctx.lineTo(w * 0.28, h * 0.58); ctx.lineTo(w * 0.68, h * 0.18)
                ctx.lineTo(w * 0.82, h * 0.32); ctx.lineTo(w * 0.42, h * 0.72); ctx.closePath()
            } else if (kind === "bolt") {
                ctx.moveTo(w * 0.56, h * 0.16); ctx.lineTo(w * 0.26, h * 0.54); ctx.lineTo(w * 0.50, h * 0.54)
                ctx.lineTo(w * 0.44, h * 0.84); ctx.lineTo(w * 0.74, h * 0.46); ctx.lineTo(w * 0.50, h * 0.46); ctx.closePath()
            } else if (kind === "export") {
                ctx.moveTo(w * 0.22, h * 0.62); ctx.lineTo(w * 0.22, h * 0.80); ctx.lineTo(w * 0.78, h * 0.80); ctx.lineTo(w * 0.78, h * 0.62)
                ctx.moveTo(w * 0.50, h * 0.62); ctx.lineTo(w * 0.50, h * 0.18)
                ctx.moveTo(w * 0.32, h * 0.36); ctx.lineTo(w * 0.50, h * 0.18); ctx.lineTo(w * 0.68, h * 0.36)
            } else if (kind === "close") {
                ctx.moveTo(w * 0.26, h * 0.26); ctx.lineTo(w * 0.74, h * 0.74)
                ctx.moveTo(w * 0.74, h * 0.26); ctx.lineTo(w * 0.26, h * 0.74)
            }
            ctx.stroke()
        }
    }

    // フォルダアイコン（シンプルで視認性の高い2トーン）
    component FolderIcon: Item {
        width: 24; height: 20
        Rectangle {
            x: 1; y: 1; width: parent.width * 0.44; height: parent.height * 0.35
            radius: 2; color: pal.folderTab
        }
        Rectangle {
            x: 1; y: parent.height * 0.22; width: parent.width - 2; height: parent.height * 0.72
            radius: 3; color: pal.folderMain
            Rectangle {
                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                height: 2; color: "#F4CE7C"
            }
        }
    }

    // ツールボタン（フラットで押しやすいデスクトップ風）
    component ToolBtn: Rectangle {
        property string iconKind: ""
        property string textLabel: ""
        property bool active: false
        property bool enabled: true
        property color fgColor: active ? pal.accent : (enabled ? pal.textPrimary : pal.textMuted)
        implicitWidth: textLabel !== "" ? Math.max(30, lbl.implicitWidth + (iconKind !== "" ? 32 : 18)) : 30
        implicitHeight: 28
        radius: 5
        color: !enabled ? "transparent"
             : active ? pal.accentSoft
             : (tma.containsMouse ? "#E5E8EE" : "transparent")
        border.width: active ? 1 : 0
        border.color: pal.accentBorder

        Row {
            anchors.centerIn: parent
            spacing: 5
            LineIcon {
                visible: iconKind !== ""
                kind: parent.parent.iconKind
                strokeColor: parent.parent.fgColor
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: lbl
                visible: textLabel !== ""
                text: textLabel
                color: parent.parent.fgColor
                font.pixelSize: 12
                font.weight: active ? Font.DemiBold : Font.Normal
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        MouseArea {
            id: tma; anchors.fill: parent; hoverEnabled: true; enabled: parent.enabled
            onClicked: parent.clicked()
        }
        signal clicked()
    }

    // アクションボタン（右インスペクタの実行/編集/AOT等）
    component ActionBtn: Rectangle {
        property string iconKind: ""
        property string label: ""
        property bool primary: false
        property bool enabled: true
        implicitHeight: primary ? 38 : 32
        radius: 6
        color: !enabled ? "#ECEEF2"
             : primary ? (ama.containsMouse ? pal.accentHover : pal.accent)
             : (ama.containsMouse ? "#F3F4F8" : pal.surface)
        border.width: primary && enabled ? 0 : 1
        border.color: !enabled ? pal.borderLight : (ama.containsMouse ? "#BFC5D2" : pal.border)

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12; anchors.rightMargin: 12
            spacing: 8
            LineIcon {
                visible: iconKind !== ""
                kind: parent.parent.iconKind
                strokeColor: !parent.parent.enabled ? pal.textMuted
                           : (parent.parent.primary ? "#FFFFFF" : pal.textSecond)
            }
            Text {
                Layout.fillWidth: true
                text: parent.parent.label
                color: !parent.parent.enabled ? pal.textMuted
                     : (parent.parent.primary ? "#FFFFFF" : pal.textPrimary)
                font.pixelSize: parent.parent.primary ? 13 : 12
                font.weight: parent.parent.primary ? Font.DemiBold : Font.Medium
                elide: Text.ElideRight
            }
        }
        MouseArea {
            id: ama; anchors.fill: parent; hoverEnabled: true; enabled: parent.enabled
            onClicked: parent.clicked()
        }
        signal clicked()
    }

    Rectangle {
        anchors.fill: parent
        color: pal.windowBg
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // =====================================================
        // トップツールバー（ナビゲーション + パス + 検索 + 表示切替）
        // =====================================================
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: pal.surface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8

                // アプリ識別（控えめで機能的なブランド表記）
                RowLayout {
                    spacing: 7
                    Rectangle {
                        width: 24; height: 24; radius: 6; color: pal.accent
                        Text {
                            anchors.centerIn: parent
                            text: "x"; color: "#FFFFFF"
                            font.pixelSize: 15; font.bold: true
                        }
                    }
                    Text {
                        text: "xyo"
                        color: pal.textPrimary
                        font.pixelSize: 15; font.bold: true
                    }
                }

                Rectangle { width: 1; height: 20; color: pal.borderLight }

                // 履歴・親フォルダ・再読込
                RowLayout {
                    spacing: 2
                    ToolBtn { iconKind: "back"; enabled: launcher.canGoBack; onClicked: launcher.goBack() }
                    ToolBtn { iconKind: "forward"; enabled: launcher.canGoForward; onClicked: launcher.goForward() }
                    ToolBtn { iconKind: "up"; enabled: launcher.canGoUp; onClicked: launcher.goUp() }
                    ToolBtn { iconKind: "reload"; onClicked: launcher.reload() }
                }

                // パスバー（通常はクリックできるパンくず、クリックまたは Ctrl+L で直接入力）
                Rectangle {
                    id: addressBar
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    radius: 5
                    color: pathEdit.visible ? pal.surface : pal.surfaceAlt
                    border.width: 1
                    border.color: pathEdit.visible ? pal.accent
                                : (addrMa.containsMouse ? "#BFC5D2" : pal.border)

                    Row {
                        visible: !pathEdit.visible
                        anchors.left: parent.left; anchors.leftMargin: 8
                        anchors.right: parent.right; anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        clip: true
                        spacing: 2
                        Repeater {
                            model: launcher.crumbs
                            delegate: Row {
                                spacing: 2
                                height: 24
                                Text {
                                    visible: index > 0
                                    text: "/"
                                    color: pal.textMuted
                                    font.pixelSize: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Rectangle {
                                    height: 22
                                    width: crumbTxt.implicitWidth + 10
                                    radius: 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: cma.containsMouse ? "#E5E8EE" : "transparent"
                                    Text {
                                        id: crumbTxt
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: index === launcher.crumbs.length - 1 ? pal.textPrimary : pal.textSecond
                                        font.pixelSize: 12
                                        font.weight: index === launcher.crumbs.length - 1 ? Font.DemiBold : Font.Normal
                                    }
                                    MouseArea {
                                        id: cma; anchors.fill: parent; hoverEnabled: true
                                        onClicked: launcher.openDir(modelData.path)
                                    }
                                }
                            }
                        }
                    }

                    TextInput {
                        id: pathEdit
                        visible: false
                        anchors.fill: parent
                        anchors.leftMargin: 10; anchors.rightMargin: 10
                        verticalAlignment: TextInput.AlignVCenter
                        font.pixelSize: 12
                        color: pal.textPrimary
                        selectByMouse: true
                        clip: true
                        function open() {
                            text = launcher.dirPath
                            visible = true
                            forceActiveFocus()
                            selectAll()
                        }
                        function close() { visible = false; focus = false }
                        Keys.onReturnPressed: { launcher.openDir(text); close() }
                        Keys.onEnterPressed: { launcher.openDir(text); close() }
                        Keys.onEscapePressed: close()
                        onActiveFocusChanged: if (!activeFocus && visible) close()
                    }

                    MouseArea {
                        id: addrMa
                        anchors.fill: parent
                        hoverEnabled: true
                        propagateComposedEvents: true
                        z: -1
                        onClicked: pathEdit.open()
                    }
                }

                // 検索フィールド
                Rectangle {
                    Layout.preferredWidth: Math.min(220, Math.max(140, root.width * 0.2))
                    Layout.preferredHeight: 30
                    radius: 5
                    color: searchInput.activeFocus ? pal.surface : pal.surfaceAlt
                    border.width: 1
                    border.color: searchInput.activeFocus ? pal.accent : pal.border

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8; anchors.rightMargin: 6
                        spacing: 6
                        LineIcon { kind: "search"; width: 14; height: 14; strokeColor: pal.textMuted }
                        TextInput {
                            id: searchInput
                            Layout.fillWidth: true
                            font.pixelSize: 12
                            color: pal.textPrimary
                            selectByMouse: true
                            clip: true
                            onTextChanged: launcher.filter = text
                            Keys.onEscapePressed: { text = ""; focus = false }
                            Keys.onReturnPressed: { focus = false; launcher.activate() }
                            Keys.onEnterPressed: { focus = false; launcher.activate() }
                            Keys.onDownPressed: launcher.moveSelectionRows(1, root.listMode ? 1 : root.gridCols)
                            Keys.onUpPressed: launcher.moveSelectionRows(-1, root.listMode ? 1 : root.gridCols)
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !parent.text && !parent.activeFocus
                                text: "検索 (/)"
                                color: pal.textMuted
                                font.pixelSize: 12
                            }
                        }
                        ToolBtn {
                            visible: searchInput.text.length > 0
                            iconKind: "close"
                            implicitWidth: 20; implicitHeight: 20
                            onClicked: searchInput.text = ""
                        }
                    }
                    Connections {
                        target: launcher
                        function onFilterChanged() {
                            if (searchInput.text !== launcher.filter)
                                searchInput.text = launcher.filter
                        }
                    }
                }

                // リスト / アイコン切替セグメント
                Rectangle {
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: 62
                    radius: 5
                    color: pal.surfaceAlt
                    border.width: 1; border.color: pal.border
                    RowLayout {
                        anchors.fill: parent; anchors.margins: 2; spacing: 2
                        Rectangle {
                            Layout.fillWidth: true; Layout.fillHeight: true; radius: 3
                            color: root.listMode ? pal.surface : "transparent"
                            border.width: root.listMode ? 1 : 0; border.color: pal.borderLight
                            LineIcon {
                                anchors.centerIn: parent; width: 14; height: 14; kind: "list"
                                strokeColor: root.listMode ? pal.accent : pal.textSecond
                            }
                            MouseArea { anchors.fill: parent; onClicked: launcher.listView = true }
                        }
                        Rectangle {
                            Layout.fillWidth: true; Layout.fillHeight: true; radius: 3
                            color: !root.listMode ? pal.surface : "transparent"
                            border.width: !root.listMode ? 1 : 0; border.color: pal.borderLight
                            LineIcon {
                                anchors.centerIn: parent; width: 14; height: 14; kind: "grid"
                                strokeColor: !root.listMode ? pal.accent : pal.textSecond
                            }
                            MouseArea { anchors.fill: parent; onClicked: launcher.listView = false }
                        }
                    }
                }

                ToolBtn {
                    textLabel: launcher.sortRecent ? "更新日順" : "名前順"
                    onClicked: launcher.sortRecent = !launcher.sortRecent
                }
                ToolBtn {
                    iconKind: "settings"
                    textLabel: "設定"
                    active: settingsDialog.visible
                    onClicked: settingsDialog.visible = !settingsDialog.visible
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom; width: parent.width; height: 1
                color: pal.border
            }
        }

        // =====================================================
        // メイン 3 ペイン（左: 場所と履歴 | 中央: 一覧 | 右: プレビューと操作）
        // =====================================================
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // -------------------------------------------------
            // 左サイドバー
            // -------------------------------------------------
            Rectangle {
                Layout.preferredWidth: root.width < 880 ? 176 : 204
                Layout.fillHeight: true
                color: pal.sidebarBg

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 4

                    Text {
                        text: "場所"
                        color: pal.textMuted
                        font.pixelSize: 11; font.weight: Font.DemiBold
                        Layout.leftMargin: 6; Layout.topMargin: 4; Layout.bottomMargin: 2
                    }

                    component SidebarItem: Rectangle {
                        property string iconKind: "folder"
                        property string label: ""
                        property string target: ""
                        property bool current: target !== "" && launcher.dirPath === target
                        Layout.fillWidth: true
                        Layout.preferredHeight: 30
                        visible: target !== ""
                        radius: 5
                        color: current ? pal.sidebarSel : (sma.containsMouse ? pal.sidebarHover : "transparent")

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8; anchors.rightMargin: 8
                            spacing: 8
                            LineIcon {
                                kind: parent.parent.iconKind
                                width: 15; height: 15
                                strokeColor: parent.parent.current ? pal.accent : pal.textSecond
                            }
                            Text {
                                Layout.fillWidth: true
                                text: parent.parent.label
                                color: parent.parent.current ? pal.textPrimary : pal.textSecond
                                font.pixelSize: 12
                                font.weight: parent.parent.current ? Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                            }
                        }
                        MouseArea {
                            id: sma; anchors.fill: parent; hoverEnabled: true
                            onClicked: launcher.openDir(parent.target)
                        }
                    }

                    SidebarItem { iconKind: "home";     label: "ホーム";       target: launcher.homeDir }
                    SidebarItem { iconKind: "folder";   label: "デスクトップ"; target: launcher.desktopDir }
                    SidebarItem { iconKind: "folder";   label: "書類";         target: launcher.documentsDir }
                    SidebarItem { iconKind: "download"; label: "ダウンロード"; target: launcher.downloadsDir }
                    SidebarItem { iconKind: "star";     label: "サンプル";     target: launcher.examplesDir }

                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 1
                        Layout.topMargin: 8; Layout.bottomMargin: 6
                        color: pal.border
                    }

                    Text {
                        text: "最近開いた項目"
                        color: pal.textMuted
                        font.pixelSize: 11; font.weight: Font.DemiBold
                        Layout.leftMargin: 6; Layout.bottomMargin: 2
                    }

                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 2
                        model: launcher.recentList
                        boundsBehavior: Flickable.StopAtBounds
                        delegate: Rectangle {
                            width: ListView.view.width
                            height: 40
                            radius: 5
                            color: rma.containsMouse ? pal.sidebarHover : "transparent"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8; anchors.rightMargin: 8
                                anchors.topMargin: 5; anchors.bottomMargin: 5
                                spacing: 1
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.stem
                                    color: pal.textPrimary
                                    font.pixelSize: 12; font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.parent
                                    color: pal.textMuted
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }
                            MouseArea {
                                id: rma; anchors.fill: parent; hoverEnabled: true
                                onClicked: launcher.openRecent(modelData.path)
                                onDoubleClicked: {
                                    launcher.openRecent(modelData.path)
                                    launcher.activate()
                                }
                            }
                        }
                        Text {
                            anchors.top: parent.top; anchors.left: parent.left; anchors.leftMargin: 8
                            visible: launcher.recentList.length === 0
                            text: "履歴はありません"
                            color: pal.textMuted
                            font.pixelSize: 11
                        }
                    }
                }

                Rectangle {
                    anchors.right: parent.right; width: 1; height: parent.height
                    color: pal.border
                }
            }

            // -------------------------------------------------
            // 中央: プロジェクト一覧（リスト / グリッド）
            // -------------------------------------------------
            Rectangle {
                id: contentPane
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: pal.surface

                // リスト表示
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    visible: root.listMode

                    // カラムヘッダ
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 28
                        color: pal.surfaceAlt
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 14; anchors.rightMargin: 16
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                text: "名前 (" + entryModel.rowCount() + ")"
                                color: pal.textSecond; font.pixelSize: 11; font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.preferredWidth: 88
                                visible: contentPane.width >= 420
                                text: "更新日"
                                color: pal.textSecond; font.pixelSize: 11; font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.preferredWidth: 68
                                visible: contentPane.width >= 420
                                horizontalAlignment: Text.AlignRight
                                text: "サイズ"
                                color: pal.textSecond; font.pixelSize: 11; font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.preferredWidth: 54
                                visible: contentPane.width >= 500
                                horizontalAlignment: Text.AlignRight
                                text: "種類"
                                color: pal.textSecond; font.pixelSize: 11; font.weight: Font.DemiBold
                            }
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom; width: parent.width; height: 1
                            color: pal.border
                        }
                    }

                    ListView {
                        id: fileList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: entryModel
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: Rectangle {
                            width: fileList.width
                            height: 36
                            readonly property bool sel: model.path === launcher.selectedPath
                            color: sel ? pal.rowSelBg
                                 : (lma.containsMouse ? pal.rowHoverBg : (index % 2 === 1 ? "#FAFBFD" : pal.surface))

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12; anchors.rightMargin: 16
                                spacing: 10

                                // アイコン or ミニサムネイル
                                Item {
                                    Layout.preferredWidth: 26
                                    Layout.preferredHeight: 20
                                    FolderIcon {
                                        anchors.centerIn: parent
                                        visible: model.isDir
                                        width: 20; height: 16
                                    }
                                    Rectangle {
                                        anchors.fill: parent; radius: 3
                                        visible: !model.isDir
                                        color: "#EDEEF3"
                                        border.width: 1; border.color: pal.borderLight
                                        clip: true
                                        Image {
                                            id: listThumbImg
                                            anchors.fill: parent
                                            visible: model.thumbUrl !== ""
                                            source: visible ? model.thumbUrl : ""
                                            fillMode: Image.PreserveAspectCrop
                                            asynchronous: true
                                        }
                                        Text {
                                            anchors.centerIn: parent
                                            visible: model.thumbUrl === "" || listThumbImg.status !== Image.Ready
                                            text: model.ext.replace(".", "")
                                            color: pal.textMuted; font.pixelSize: 8; font.bold: true
                                        }
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: model.displayName
                                    color: pal.textPrimary
                                    font.pixelSize: 13
                                    font.weight: sel ? Font.DemiBold : Font.Normal
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 88
                                    visible: contentPane.width >= 420
                                    text: model.dateText
                                    color: pal.textSecond; font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.preferredWidth: 68
                                    visible: contentPane.width >= 420
                                    horizontalAlignment: Text.AlignRight
                                    text: model.sizeText
                                    color: pal.textSecond; font.pixelSize: 11
                                }
                                Text {
                                    Layout.preferredWidth: 54
                                    visible: contentPane.width >= 500
                                    horizontalAlignment: Text.AlignRight
                                    text: model.isDir ? "フォルダ" : model.ext.toUpperCase().replace(".", "")
                                    color: pal.textMuted; font.pixelSize: 11
                                }
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom; width: parent.width; height: 1
                                color: pal.borderLight
                            }
                            MouseArea {
                                id: lma; anchors.fill: parent; hoverEnabled: true
                                onClicked: launcher.selectAt(index)
                                onDoubleClicked: launcher.activateAt(index)
                            }
                        }
                    }
                }

                // グリッド表示
                GridView {
                    id: fileGrid
                    visible: !root.listMode
                    anchors.fill: parent
                    anchors.margins: 14
                    clip: true
                    model: entryModel
                    cellWidth: Math.floor((width) / root.gridCols)
                    cellHeight: Math.round((cellWidth - 12) * 0.72) + 52
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Item {
                        width: fileGrid.cellWidth
                        height: fileGrid.cellHeight
                        readonly property bool sel: model.path === launcher.selectedPath

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 6
                            radius: 7
                            color: sel ? pal.accentSoft : (gma.containsMouse ? pal.surfaceAlt : pal.surface)
                            border.width: sel ? 2 : 1
                            border.color: sel ? pal.accent : (gma.containsMouse ? "#BFC5D2" : pal.borderLight)

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 7
                                spacing: 6

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    radius: 4
                                    color: model.isDir ? "transparent" : "#EEF0F5"
                                    clip: true

                                    FolderIcon {
                                        anchors.centerIn: parent
                                        visible: model.isDir
                                        width: 58; height: 46
                                    }
                                    Image {
                                        id: gridThumbImg
                                        anchors.fill: parent
                                        visible: !model.isDir && model.thumbUrl !== ""
                                        source: visible ? model.thumbUrl : ""
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        visible: !model.isDir && (model.thumbUrl === "" || gridThumbImg.status !== Image.Ready)
                                        text: model.ext.toUpperCase().replace(".", "")
                                        color: pal.textMuted; font.pixelSize: 13; font.bold: true
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text {
                                        Layout.fillWidth: true
                                        text: model.displayName
                                        color: pal.textPrimary
                                        font.pixelSize: 12; font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: model.isDir ? "フォルダ" : (model.sizeText + (model.dateText !== "—" ? " · " + model.dateText : ""))
                                        color: pal.textMuted
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            MouseArea {
                                id: gma; anchors.fill: parent; hoverEnabled: true
                                onClicked: launcher.selectAt(index)
                                onDoubleClicked: launcher.activateAt(index)
                            }
                        }
                    }
                }

                // 空状態表示
                Column {
                    anchors.centerIn: parent
                    spacing: 6
                    visible: entryModel.rowCount() === 0
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: launcher.filter === "" ? "プロジェクトが見つかりません" : "検索条件に一致する項目がありません"
                        color: pal.textSecond; font.pixelSize: 14; font.weight: Font.Medium
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: launcher.filter === ""
                        text: ".sb3 / .sb2 ファイルを置くか、ウィンドウにドロップしてください"
                        color: pal.textMuted; font.pixelSize: 12
                    }
                }
            }

            // -------------------------------------------------
            // 右インスペクタ（プレビュー + 実行/編集/AOT 操作）
            // -------------------------------------------------
            Rectangle {
                Layout.preferredWidth: 268
                Layout.fillHeight: true
                color: pal.surfaceAlt

                Rectangle {
                    anchors.left: parent.left; width: 1; height: parent.height
                    color: pal.border
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 12

                    // プレビュー枠（4:3）
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.round(width * 0.75)
                        radius: 6
                        color: pal.surface
                        border.width: 1; border.color: pal.border
                        clip: true

                        Text {
                            anchors.centerIn: parent
                            visible: !launcher.hasSelection
                            text: "未選択"
                            color: pal.textMuted; font.pixelSize: 12
                        }
                        FolderIcon {
                            anchors.centerIn: parent
                            visible: launcher.hasSelection && launcher.selectedIsDir
                            width: 64; height: 50
                        }
                        Image {
                            id: pvImg
                            anchors.fill: parent
                            visible: launcher.hasSelection && !launcher.selectedIsDir && launcher.selectedThumb !== ""
                            source: visible ? launcher.selectedThumb : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                        }
                        Text {
                            anchors.centerIn: parent
                            visible: launcher.hasSelection && !launcher.selectedIsDir && pvImg.status !== Image.Ready
                            text: "読込中..."
                            color: pal.textMuted; font.pixelSize: 11
                        }
                    }

                    // 選択アイテム情報
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Text {
                            Layout.fillWidth: true
                            text: launcher.hasSelection ? launcher.selectedName : "ファイルを選択"
                            color: pal.textPrimary
                            font.pixelSize: 15; font.bold: true
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: launcher.hasSelection
                            text: launcher.selectedKind
                            color: pal.accent; font.pixelSize: 11; font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: launcher.hasSelection
                            text: launcher.selectedIsDir ? "開いてフォルダ内を表示" : launcher.selectedMeta
                            color: pal.textSecond; font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: pal.borderLight }

                    // 操作ボタン群
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        ActionBtn {
                            Layout.fillWidth: true
                            primary: true
                            iconKind: launcher.selectedIsDir ? "folder" : "play"
                            label: launcher.selectedIsDir ? "フォルダを開く" : "実行する"
                            enabled: launcher.hasSelection && (launcher.selectedIsDir || launcher.playAvailable)
                            onClicked: launcher.activate()
                        }
                        ActionBtn {
                            Layout.fillWidth: true
                            iconKind: "edit"
                            label: "エディタで編集 (E)"
                            enabled: launcher.projectSelected
                            onClicked: launcher.openEditor()
                        }
                        ActionBtn {
                            Layout.fillWidth: true
                            iconKind: "bolt"
                            label: "AOT コンパイル実行"
                            enabled: launcher.projectSelected && launcher.jitAvailable
                            onClicked: launcher.runAotProject()
                        }
                        ActionBtn {
                            Layout.fillWidth: true
                            iconKind: "export"
                            label: "単体実行ファイルを書き出し (A)"
                            enabled: launcher.projectSelected && launcher.jitAvailable
                            onClicked: launcher.exportAot()
                        }
                    }

                    Item { Layout.fillHeight: true }

                    // 現在の実行設定サマリ
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: optCol.implicitHeight + 16
                        radius: 6
                        color: pal.surface
                        border.width: 1; border.color: pal.borderLight

                        ColumnLayout {
                            id: optCol
                            anchors.fill: parent; anchors.margins: 8
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                Text {
                                    Layout.fillWidth: true
                                    text: "実行プロファイル"
                                    color: pal.textMuted; font.pixelSize: 11; font.weight: Font.DemiBold
                                }
                                Text {
                                    text: "変更..."
                                    color: pal.accent; font.pixelSize: 11; font.weight: Font.DemiBold
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: settingsDialog.visible = true
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: launcher.optScale + "× · "
                                    + (launcher.optFps === -1 ? "FPS自動" : launcher.optFps + "fps") + " · "
                                    + (launcher.jitAvailable ? (launcher.optJit === 0 ? "HotSpot" : launcher.optJit === 1 ? "Eager" : "JIT OFF") : "Interp")
                                    + (launcher.optTurbo ? " · Turbo" : "")
                                    + (launcher.optDanger ? " · 危険" : "")
                                color: launcher.optDanger ? pal.danger : pal.textSecond
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }

        // =====================================================
        // 最下部ステータスバー
        // =====================================================
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: pal.surface

            Rectangle {
                anchors.top: parent.top; width: parent.width; height: 1
                color: pal.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12; anchors.rightMargin: 12
                spacing: 10

                Rectangle {
                    width: 7; height: 7; radius: 4
                    visible: launcher.statusText !== ""
                    color: launcher.statusIsError ? pal.danger : pal.ok
                }
                Text {
                    Layout.fillWidth: true
                    text: launcher.statusText !== ""
                          ? launcher.statusText
                          : (launcher.dirPath + "  —  " + entryModel.rowCount() + " 項目")
                    color: launcher.statusIsError ? pal.danger : pal.textSecond
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }

                // AOT 進捗バー
                Rectangle {
                    visible: launcher.aotProgress >= 0
                    Layout.preferredWidth: 160
                    Layout.preferredHeight: 6
                    radius: 3
                    color: pal.sidebarBg
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, launcher.aotProgress))
                        height: parent.height; radius: 3; color: pal.accent
                    }
                }

                Text {
                    text: "終了 (Esc)"
                    color: qma.containsMouse ? pal.textPrimary : pal.textMuted
                    font.pixelSize: 11
                    MouseArea {
                        id: qma; anchors.fill: parent; hoverEnabled: true
                        onClicked: launcher.quitApp()
                    }
                }
            }
        }
    }

    // =========================================================
    // 実行設定モーダル（コンパクトで明快なフォーム）
    // =========================================================
    Item {
        id: settingsDialog
        visible: false
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent; color: "#8010141C"
            MouseArea { anchors.fill: parent; onClicked: settingsDialog.visible = false }
        }

        Rectangle {
            width: Math.min(540, parent.width - 40)
            height: settingsCol.implicitHeight + 36
            anchors.centerIn: parent
            radius: 8
            color: pal.surface
            border.width: 1; border.color: pal.border
            MouseArea { anchors.fill: parent } // クリック透過防止

            ColumnLayout {
                id: settingsCol
                anchors.fill: parent; anchors.margins: 18
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: "実行設定"
                        color: pal.textPrimary; font.pixelSize: 16; font.bold: true
                    }
                    ToolBtn {
                        iconKind: "close"
                        onClicked: settingsDialog.visible = false
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: pal.borderLight }

                component SegRow: RowLayout {
                    property string title: ""
                    property var items: []
                    property int current: 0
                    property bool enabled: true
                    signal chosen(int idx)
                    Layout.fillWidth: true
                    spacing: 12

                    Text {
                        Layout.preferredWidth: 130
                        text: parent.title
                        color: pal.textSecond; font.pixelSize: 12; font.weight: Font.Medium
                    }
                    Rectangle {
                        Layout.fillWidth: true; Layout.preferredHeight: 30
                        radius: 5; color: pal.surfaceAlt
                        border.width: 1; border.color: pal.border
                        RowLayout {
                            anchors.fill: parent; anchors.margins: 2; spacing: 2
                            Repeater {
                                model: items.length
                                delegate: Rectangle {
                                    required property int index
                                    Layout.fillWidth: true; Layout.fillHeight: true; radius: 3
                                    readonly property bool sel: index === current
                                    color: sel ? (enabled ? pal.accent : pal.textMuted) : "transparent"
                                    Text {
                                        anchors.centerIn: parent
                                        text: items[index]
                                        color: sel ? "#FFFFFF" : (enabled ? pal.textPrimary : pal.textMuted)
                                        font.pixelSize: 12; font.weight: sel ? Font.DemiBold : Font.Normal
                                    }
                                    MouseArea {
                                        anchors.fill: parent; enabled: parent.parent.parent.parent.enabled
                                        onClicked: chosen(index)
                                    }
                                }
                            }
                        }
                    }
                }

                SegRow {
                    title: "画面倍率"; items: ["1×", "2×", "3×", "4×"]
                    current: launcher.optScale - 1
                    onChosen: function(i) { launcher.optScale = i + 1 }
                }
                SegRow {
                    title: "フレームレート"; items: ["自動", "30", "60", "120"]
                    current: launcher.optFps === -1 ? 0 : launcher.optFps === 30 ? 1 : launcher.optFps === 60 ? 2 : 3
                    onChosen: function(i) { launcher.optFps = [-1, 30, 60, 120][i] }
                }
                SegRow {
                    title: "JIT 実行方式"; items: ["HotSpot", "Eager", "OFF"]
                    current: launcher.jitAvailable ? launcher.optJit : 2
                    enabled: launcher.jitAvailable
                    onChosen: function(i) { launcher.optJit = i }
                }
                SegRow {
                    title: "遅延 JIT"; items: ["自動", "ON", "OFF"]
                    current: launcher.jitAvailable ? launcher.optLazy : 2
                    enabled: launcher.jitAvailable && launcher.optJit !== 2
                    onChosen: function(i) { launcher.optLazy = i }
                }
                SegRow {
                    title: "GPU 描画 (実験的)"; items: ["通常", "Ray", "Path"]
                    current: launcher.optGpu + 1
                    onChosen: function(i) { launcher.optGpu = i - 1 }
                }

                RowLayout {
                    Layout.fillWidth: true; spacing: 10; Layout.topMargin: 4
                    ActionBtn {
                        Layout.fillWidth: true
                        label: launcher.optTurbo ? "✓ Turbo モード強制 ON" : "Turbo モード: 自動"
                        primary: launcher.optTurbo
                        onClicked: launcher.optTurbo = !launcher.optTurbo
                    }
                    ActionBtn {
                        Layout.fillWidth: true
                        label: launcher.optDanger ? "✓ 危険モード (時間制限なし)" : "危険モード: 無効"
                        primary: launcher.optDanger
                        onClicked: launcher.optDanger = !launcher.optDanger
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: launcher.optDanger
                          ? "警告: スクリプトの実行時間制限が解除されます（起動ごとに無効へ戻ります）。"
                          : "設定は自動的に保存されます（危険モードのみ安全のため毎回無効に戻ります）。"
                    color: launcher.optDanger ? pal.danger : pal.textMuted
                    font.pixelSize: 11; wrapMode: Text.Wrap
                }
            }
        }
    }

    Connections {
        target: launcher
        function onToggleSettingsRequested() {
            settingsDialog.visible = !settingsDialog.visible
        }
    }

    // ビジーオーバーレイ
    Rectangle {
        anchors.fill: parent
        visible: launcher.busy
        color: "#4010141C"
        MouseArea { anchors.fill: parent; enabled: parent.visible }
    }

    // D&D
    DropArea {
        anchors.fill: parent
        onDropped: function(drop) {
            if (drop.hasUrls && drop.urls.length > 0) {
                const p = decodeURIComponent(String(drop.urls[0]).replace(/^file:\/\//, ""))
                launcher.openDir(p)
            }
        }
    }

    // キーボードショートカット
    focus: true
    Keys.onPressed: function(ev) {
        if (settingsDialog.visible) {
            if (ev.key === Qt.Key_Escape || ev.key === Qt.Key_Return || ev.key === Qt.Key_Enter) {
                settingsDialog.visible = false
                ev.accepted = true
            }
            return
        }
        if (searchInput.activeFocus || pathEdit.visible) return
        const ctrl = ev.modifiers & (Qt.ControlModifier | Qt.MetaModifier)
        const alt = ev.modifiers & Qt.AltModifier
        if ((ctrl && ev.key === Qt.Key_Comma) || ev.key === Qt.Key_F4) {
            settingsDialog.visible = !settingsDialog.visible
        } else if (ctrl && (ev.key === Qt.Key_L || ev.key === Qt.Key_O)) {
            pathEdit.open()
        } else if (ctrl && ev.key === Qt.Key_F) {
            searchInput.forceActiveFocus()
        } else if (ctrl && ev.key === Qt.Key_1) {
            launcher.listView = false
        } else if (ctrl && ev.key === Qt.Key_2) {
            launcher.listView = true
        } else if (ctrl && ev.key === Qt.Key_Up) {
            launcher.goUp()
        } else if (ctrl && ev.key === Qt.Key_Down) {
            launcher.activate()
        } else if (alt && ev.key === Qt.Key_Left) {
            launcher.goBack()
        } else if (alt && ev.key === Qt.Key_Right) {
            launcher.goForward()
        } else if (ev.key === Qt.Key_Escape) {
            launcher.quitApp()
        } else if (ev.key === Qt.Key_F5) {
            launcher.reload()
        } else if (ev.key === Qt.Key_Up) {
            launcher.moveSelectionRows(-1, root.listMode ? 1 : root.gridCols)
        } else if (ev.key === Qt.Key_Down) {
            launcher.moveSelectionRows(1, root.listMode ? 1 : root.gridCols)
        } else if (ev.key === Qt.Key_Left) {
            if (root.listMode) launcher.goUp()
            else launcher.moveSelection(-1)
        } else if (ev.key === Qt.Key_Right) {
            if (root.listMode && launcher.selectedIsDir) launcher.activate()
            else if (!root.listMode) launcher.moveSelection(1)
        } else if (ev.key === Qt.Key_PageUp) {
            launcher.moveSelectionRows(-10, 1)
        } else if (ev.key === Qt.Key_PageDown) {
            launcher.moveSelectionRows(10, 1)
        } else if (ev.key === Qt.Key_Home) {
            launcher.selectHome()
        } else if (ev.key === Qt.Key_End) {
            launcher.selectEnd()
        } else if (ev.key === Qt.Key_Backspace) {
            launcher.goUp()
        } else if (ev.key === Qt.Key_Return || ev.key === Qt.Key_Enter) {
            launcher.activate()
        } else if (ev.text === "e") {
            launcher.openEditor()
        } else if (ev.text === "a") {
            launcher.exportAot()
        } else if (ev.text === "/") {
            searchInput.forceActiveFocus()
        } else {
            ev.accepted = false
            return
        }
        ev.accepted = true
    }

    Connections {
        target: launcher
        function onRevealSelection() {
            const row = launcher.proxyRowOf(launcher.selectedPath)
            if (row < 0) return
            if (root.listMode)
                fileList.positionViewAtIndex(row, ListView.EnsureVisible)
            else
                fileGrid.positionViewAtIndex(row, GridView.EnsureVisible)
        }
        function onScrollBy(delta) {
            if (root.listMode)
                fileList.contentY = Math.max(0, fileList.contentY + delta)
            else
                fileGrid.contentY = Math.max(0, fileGrid.contentY + delta)
        }
    }
}
