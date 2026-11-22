// xyo-cpp: Qt Quick ランチャー（`xyo` / `xyo gui`）
// C++ 側は src/shell/gui_qt.cpp の LauncherController（context 名 `launcher`）
// と `entryModel`（フィルタ+ソート済みプロキシ）を使う。
import QtQuick

Item {
    id: root
    width: 1120
    height: 760

    // ---- palette（旧 SDL ランチャーと同じ色）
    QtObject {
        id: theme
        readonly property color bg: "#F4F6FB"
        readonly property color panel: "#FFFFFF"
        readonly property color fg: "#575E75"
        readonly property color strong: "#2B3247"
        readonly property color dim: "#8691A7"
        readonly property color muted: "#A8B1C2"
        readonly property color border: "#1A000000"
        readonly property color field: "#F1F4F9"
        readonly property color accent: "#3A78D8"
        readonly property color accentHot: "#2964BF"
        readonly property color folder: "#F2C46D"
        readonly property color folderDark: "#E0AE4F"
        readonly property color folderLight: "#F8D996"
        readonly property color green: "#3FB26B"
        readonly property color warn: "#E05B52"
        readonly property color sidebar: "#EDF0F6"
        readonly property color sidebarHover: "#DBE2F3"
        readonly property color sidebarText: "#2E3850"
        readonly property color sidebarMuted: "#7A859D"
        readonly property color selectBg: "#DAE6FF"
        readonly property color rowHover: "#F1F4FC"
        readonly property color hairline: "#E3E7F0"
    }

    readonly property int sideW: width < 880 ? 186 : 218
    readonly property int footerH: 96
    readonly property int gridX: sideW + 24
    // preview pane (list mode only)
    readonly property int previewW: listMode && (width - gridX) >= 720
        ? Math.max(230, Math.min(290, Math.floor((width - gridX) / 3))) : 0
    readonly property bool listMode: launcher.listView
    // grid mode metrics
    readonly property int gap: 16
    readonly property int gridAvail: Math.max(180, width - gridX - 24)
    readonly property int gridCols: Math.max(1, Math.min(5,
        Math.floor((gridAvail + gap) / (192 + gap))))
    readonly property int cardW: Math.floor(
        (gridAvail - (gridCols - 1) * gap) / gridCols)
    readonly property int cardH: Math.round(cardW * 0.75) - 12 + 54
    readonly property string fontUi: ""

    // =========================================================
    // navigation rail
    // =========================================================
    Rectangle {
        id: rail
        x: 0; y: 0; width: root.sideW; height: root.height
        color: theme.sidebar

        Rectangle {
            x: 20; y: 19; width: 40; height: 40; radius: 12
            color: theme.accent
            Text {
                anchors.centerIn: parent
                text: "X"; color: "white"
                font.pixelSize: 20; font.bold: true
            }
        }
        Text {
            x: 72; y: 18; text: "xyo"; color: theme.sidebarText
            font.pixelSize: 23; font.bold: true
        }
        Text {
            x: 73; y: 47; text: "PROJECT STUDIO"; color: theme.sidebarMuted
            font.pixelSize: 10
        }
        Rectangle {
            x: 20; y: 78; width: root.sideW - 40; height: 1
            color: "#D5DBE7"
        }
        Text {
            x: 22; y: 95; text: "よく使う項目"; color: theme.sidebarMuted
            font.pixelSize: 12; font.bold: true
        }

        component NavItem: Item {
            property string glyph: ""
            property string label: ""
            property string target: ""
            property bool selected: launcher.dirPath === target
            width: parent.width - 24; height: 38
            visible: target !== ""
            Rectangle {
                anchors.fill: parent; radius: 10; visible: parent.selected || ma.containsMouse
                color: parent.selected ? "#D3E3FA" : theme.sidebarHover
            }
            Rectangle {
                x: 9; y: 8; width: 22; height: 22; radius: 6
                color: parent.selected ? "#BBD5F6" : "#DEE4F1"
                Text {
                    anchors.centerIn: parent
                    text: parent.parent.glyph; color: theme.accent
                    font.pixelSize: 14; font.bold: true
                }
            }
            Text {
                x: 41; y: 10; width: parent.width - 51
                text: parent.label; color: parent.selected ? theme.strong : theme.sidebarText
                font.pixelSize: 13; font.bold: parent.selected
                elide: Text.ElideRight
            }
            MouseArea {
                id: ma; anchors.fill: parent; hoverEnabled: true
                onClicked: launcher.openDir(parent.target)
            }
        }
        Column {
            x: 12; y: 120; spacing: 5
            NavItem { glyph: "H"; label: "ホーム"; target: launcher.homeDir }
            NavItem { glyph: "D"; label: "デスクトップ"; target: launcher.desktopDir }
            NavItem { glyph: "F"; label: "書類"; target: launcher.documentsDir }
            NavItem { glyph: "↓"; label: "ダウンロード"; target: launcher.downloadsDir }
            NavItem { glyph: "S"; label: "サンプル"; target: launcher.examplesDir }
        }

        Rectangle {
            x: 20; y: 345; width: root.sideW - 40; height: 1
            color: "#D5DBE7"
        }
        Text {
            x: 22; y: 357; text: "最近開いたプロジェクト"; color: theme.sidebarMuted
            font.pixelSize: 12; font.bold: true
        }
        Repeater {
            model: launcher.recentList
            delegate: Item {
                x: 12; y: 384 + index * 44
                width: root.sideW - 24; height: 40
                visible: y + height < root.height - root.footerH - 60
                Rectangle {
                    anchors.fill: parent; radius: 10
                    color: rma.containsMouse ? theme.sidebarHover : "transparent"
                }
                Rectangle {
                    x: 9; y: 8; width: 24; height: 24; radius: 7
                    color: "#D9D5F6"
                    Text {
                        anchors.centerIn: parent; text: "P"
                        color: theme.accent; font.pixelSize: 12; font.bold: true
                    }
                }
                Text {
                    x: 42; y: 3; width: parent.width - 50
                    text: modelData.stem; color: theme.sidebarText
                    font.pixelSize: 12; font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    x: 42; y: 22; width: parent.width - 50
                    text: modelData.parent; color: theme.sidebarMuted
                    font.pixelSize: 10; elide: Text.ElideRight
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
        }
        Text {
            x: 22; y: 392; visible: launcher.recentList.length === 0
            text: "実行するとここに表示"; color: theme.sidebarMuted
            font.pixelSize: 12; width: root.sideW - 34; elide: Text.ElideRight
        }
        Text {
            x: 22; y: root.height - root.footerH - 49
            text: "ファイルをドロップして開く"; color: theme.sidebarMuted
            font.pixelSize: 11
        }
        Text {
            x: 22; y: root.height - root.footerH - 29
            text: "Ctrl / Cmd + L でパス入力"; color: theme.sidebarMuted
            font.pixelSize: 11
        }
    }

    // =========================================================
    // workspace header
    // =========================================================
    Rectangle {
        x: root.sideW; y: 0; width: root.width - root.sideW; height: 75
        color: theme.panel
        Rectangle {
            anchors.bottom: parent.bottom; width: parent.width; height: 1
            color: "#E1E7F1"
        }
        Text {
            x: 24; y: 17; width: Math.max(80, parent.width - 24 - 375)
            text: launcher.dirName; color: theme.strong
            font.pixelSize: 20; font.bold: true; elide: Text.ElideRight
        }
        Text {
            x: 24; y: 49
            text: "xyo  /  Scratch プロジェクト"; color: theme.dim
            font.pixelSize: 12
        }
    }

    // search field
    Rectangle {
        id: searchBox
        width: Math.min(326, Math.floor((root.width - root.sideW) / 2))
        height: 40
        x: root.width - 24 - width; y: 18
        radius: 12
        color: searchInput.activeFocus ? "#F1EFFF" : theme.field
        border.width: 1
        border.color: searchInput.activeFocus ? theme.accent
            : (searchMa.containsMouse ? "#C9B8F0" : "#DEE4EE")

        Canvas {
            id: magIcon
            x: 12; y: 14; width: 14; height: 14
            onPaint: {
                var ctx = getContext("2d")
                ctx.strokeStyle = "#8691A7"
                ctx.lineWidth = 2
                ctx.beginPath()
                ctx.arc(5, 5, 4, 0, Math.PI * 2)
                ctx.stroke()
                ctx.beginPath()
                ctx.moveTo(8, 9); ctx.lineTo(13, 14)
                ctx.stroke()
            }
        }
        TextInput {
            id: searchInput
            x: 36; y: 0; width: parent.width - 70; height: parent.height
            verticalAlignment: TextInput.AlignVCenter
            font.pixelSize: 14; color: theme.fg
            clip: true
            selectByMouse: true
            onTextChanged: launcher.filter = text
            Keys.onEscapePressed: {
                text = ""
                focus = false
            }
            Keys.onReturnPressed: { focus = false; launcher.activate() }
            Keys.onEnterPressed: { focus = false; launcher.activate() }
            Keys.onDownPressed: launcher.moveSelectionRows(1, root.gridCols)
            Keys.onUpPressed: launcher.moveSelectionRows(-1, root.gridCols)
            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: !parent.text && !parent.activeFocus
                text: "プロジェクトを検索   /"; color: theme.muted
                font.pixelSize: 13
            }
        }
        Connections {
            target: launcher
            function onFilterChanged() {
                if (searchInput.text !== launcher.filter)
                    searchInput.text = launcher.filter
            }
        }
        Rectangle {
            id: searchClear
            width: 24; height: 23; radius: 9
            x: parent.width - 33; y: 9
            visible: searchInput.text.length > 0
            color: clearMa.containsMouse ? "#E4E9F2" : "transparent"
            Text {
                anchors.centerIn: parent; text: "x"
                color: theme.dim; font.pixelSize: 15
            }
            MouseArea {
                id: clearMa; anchors.fill: parent; hoverEnabled: true
                onClicked: searchInput.text = ""
            }
        }
        MouseArea {
            id: searchMa; anchors.fill: parent; hoverEnabled: true
            propagateComposedEvents: true
            onClicked: { searchInput.forceActiveFocus(); mouse.accepted = false }
        }
    }

    // =========================================================
    // toolbar: nav buttons + breadcrumbs/path field
    // =========================================================
    component PillButton: Rectangle {
        property string label: ""
        property bool enabled: true
        property bool accent: false
        property bool hovered: ma.containsMouse
        implicitWidth: 36; implicitHeight: 36
        radius: height / 2
        color: !enabled ? "#F4F5F8"
             : accent ? (hovered ? theme.accentHot : theme.accent)
             : (hovered ? "#F0EEFF" : theme.panel)
        border.width: (!accent || !enabled) ? 1 : 0
        border.color: hovered && enabled ? "#C9B8F0" : "#DFE4EF"
        Text {
            anchors.centerIn: parent
            text: parent.label
            color: !parent.enabled ? theme.muted
                 : parent.accent ? "white" : theme.fg
            font.pixelSize: 14; font.bold: parent.accent && parent.enabled
        }
        MouseArea {
            id: ma; anchors.fill: parent; hoverEnabled: true
            enabled: parent.enabled
            onClicked: parent.clicked()
        }
        signal clicked()
    }

    PillButton {
        id: bBack; x: root.gridX; y: 86; label: "<"
        enabled: launcher.canGoBack
        onClicked: launcher.goBack()
    }
    PillButton {
        id: bForward; x: root.gridX + 42; y: 86; label: ">"
        enabled: launcher.canGoForward
        onClicked: launcher.goForward()
    }
    PillButton {
        id: bUp; x: root.gridX + 84; y: 86; label: "^"
        enabled: launcher.canGoUp
        onClicked: launcher.goUp()
    }

    Rectangle {
        id: pathBox
        x: root.gridX + 132; y: 86
        width: Math.max(60, root.width - root.gridX - 156)
        height: 36; radius: 10
        color: pathInput.visible ? "#F0EDFF" : theme.panel
        border.width: 1
        border.color: pathInput.visible ? theme.accent
            : (pathMa.containsMouse ? "#C9B8F0" : "#DFE5EE")
        Text {
            x: 12; anchors.verticalCenter: parent.verticalCenter
            text: "/"; color: theme.accent; font.pixelSize: 14; font.bold: true
        }
        // breadcrumb mode
        Item {
            anchors.left: parent.left; anchors.leftMargin: 30
            anchors.right: parent.right; anchors.rightMargin: 64
            anchors.verticalCenter: parent.verticalCenter
            height: 24; clip: true
            visible: !pathInput.visible
            Row {
                anchors.right: parent.right; height: parent.height
                spacing: 4
                Repeater {
                    model: launcher.crumbs
                    delegate: Row {
                        spacing: 4; height: parent.height
                        Text {
                            text: index + 1 < launcher.crumbs.length ? "›" : ""
                            color: theme.muted; font.pixelSize: 12
                            anchors.verticalCenter: parent.verticalCenter
                            visible: index > 0
                        }
                        Text {
                            text: modelData.label
                            color: index === launcher.crumbs.length - 1
                                   ? theme.strong : theme.dim
                            font.pixelSize: 12
                            font.bold: index === launcher.crumbs.length - 1
                            anchors.verticalCenter: parent.verticalCenter
                            MouseArea {
                                anchors.fill: parent
                                onClicked: launcher.openDir(modelData.path)
                            }
                        }
                    }
                }
            }
        }
        // edit mode
        TextInput {
            id: pathInput
            visible: false
            anchors.left: parent.left; anchors.leftMargin: 30
            anchors.right: parent.right; anchors.rightMargin: 64
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 12; color: theme.fg; clip: true
            selectByMouse: true
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
        Text {
            anchors.right: parent.right; anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: "⌘ L"; color: theme.muted; font.pixelSize: 11
        }
        MouseArea {
            id: pathMa; anchors.fill: parent; hoverEnabled: true
            propagateComposedEvents: true
            onClicked: { pathInput.open(); mouse.accepted = false }
        }
    }

    // =========================================================
    // section heading + view buttons
    // =========================================================
    Text {
        x: root.gridX; y: 144
        text: launcher.filter === "" ? "このフォルダ" : "検索結果"
        color: theme.strong; font.pixelSize: 18; font.bold: true
    }
    Text {
        x: root.gridX + 148; y: 151
        visible: root.width >= 900
        text: entryModel.rowCount() + " 項目"
        color: theme.dim; font.pixelSize: 12
    }
    PillButton {
        id: bIcons
        implicitWidth: 76; implicitHeight: 32
        x: bList.x - 6 - width; y: 140
        label: "アイコン"; accent: !root.listMode
        onClicked: launcher.listView = false
    }
    PillButton {
        id: bList
        implicitWidth: 64; implicitHeight: 32
        x: bSort.x - 10 - width; y: 140
        label: "リスト"; accent: root.listMode
        onClicked: launcher.listView = true
    }
    PillButton {
        id: bSort
        implicitWidth: 108; implicitHeight: 32
        x: bSettings.x - 8 - width; y: 140
        label: launcher.sortRecent ? "更新日順" : "名前順"
        onClicked: launcher.sortRecent = !launcher.sortRecent
    }
    PillButton {
        id: bSettings
        implicitWidth: 96; implicitHeight: 32
        x: root.width - 24 - width; y: 140
        label: "実行設定"; accent: settingsPanel.visible
        onClicked: settingsPanel.visible = !settingsPanel.visible
    }
    Connections {
        target: launcher
        function onToggleSettingsRequested() {
            settingsPanel.visible = !settingsPanel.visible
        }
    }

    // =========================================================
    // content area
    // =========================================================
    readonly property int contentTop: listMode ? 209 : 185
    readonly property int contentH: Math.max(80,
        height - contentTop - footerH - 12)

    // ----- Finder-style list
    Item {
        visible: root.listMode
        x: root.gridX; y: root.contentTop - 29
        width: root.width - root.gridX - 24 - (root.previewW ? root.previewW + 13 : 0)
        height: root.height - root.footerH - 10 - y
        Rectangle {
            anchors.fill: parent; color: theme.panel
        }
        Rectangle {
            x: 0; y: 28; width: parent.width; height: 1
            color: "#E5E9F2"
        }
        Text { x: 15; y: 7; text: "名前"; color: theme.dim; font.pixelSize: 12 }
        Text {
            x: parent.width - 180; y: 7; text: "変更日"
            color: theme.dim; font.pixelSize: 12
            visible: parent.width >= 420
        }
        Text {
            x: parent.width - 77; y: 7; text: "サイズ"
            color: theme.dim; font.pixelSize: 12
            visible: parent.width >= 420
        }
        Text {
            x: parent.width - 102; y: 7; text: "種類"
            color: theme.dim; font.pixelSize: 12
            visible: parent.width < 420
        }

        ListView {
            id: fileList
            anchors.fill: parent; anchors.topMargin: 29
            clip: true
            model: entryModel
            boundsBehavior: Flickable.StopAtBounds
            delegate: Item {
                width: fileList.width; height: 40
                readonly property bool sel:
                    model.path === launcher.selectedPath
                Rectangle {
                    x: 4; y: 2; width: parent.width - 8; height: parent.height - 4
                    color: sel ? theme.selectBg
                         : rowMa.containsMouse ? theme.rowHover
                         : (index % 2 ? "#FAFBFE" : "transparent")
                }
                // icon
                Item {
                    x: 12; y: 8; width: 30; height: 26
                    FolderGlyph {
                        anchors.centerIn: parent; scale: 0.55
                        visible: model.isDir
                    }
                    Image {
                        id: listThumbImg
                        anchors.fill: parent
                        visible: !model.isDir && model.thumbUrl !== ""
                        source: visible ? model.thumbUrl : ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                    }
                    Rectangle {
                        visible: !model.isDir &&
                                 (model.thumbUrl === "" || listThumbImg.status !== Image.Ready)
                        x: 1; y: 0; width: 27; height: 25; radius: 4
                        color: "#D7D1F2"
                        Text {
                            anchors.centerIn: parent; text: "S"
                            color: theme.accent; font.pixelSize: 12
                            font.bold: true
                        }
                    }
                }
                Text {
                    x: 53; y: 10; width: Math.max(16,
                        parent.width - (parent.width >= 420 ? 242 : 168))
                    text: model.displayName
                    color: sel ? theme.strong : theme.fg
                    font.pixelSize: 13; font.bold: sel
                    elide: Text.ElideRight
                }
                Text {
                    x: parent.width - 180; y: 12; width: 95
                    text: model.dateText; color: theme.dim
                    font.pixelSize: 11; elide: Text.ElideRight
                    visible: parent.width >= 420
                }
                Text {
                    x: parent.width - 77; y: 12; width: 69
                    text: model.sizeText; color: theme.dim
                    font.pixelSize: 11; elide: Text.ElideRight
                    visible: parent.width >= 420
                }
                Text {
                    x: parent.width - 102; y: 12; width: 89
                    text: model.isDir ? "フォルダ" : "Scratch"
                    color: theme.dim; font.pixelSize: 11
                    elide: Text.ElideRight
                    visible: parent.width < 420
                }
                Rectangle {
                    x: 0; y: parent.height - 1; width: parent.width; height: 1
                    color: "#F0F2F7"
                }
                MouseArea {
                    id: rowMa; anchors.fill: parent; hoverEnabled: true
                    onClicked: launcher.selectAt(index)
                    onDoubleClicked: launcher.activateAt(index)
                }
            }
            Rectangle {
                // thin custom scrollbar
                anchors.right: parent.right; anchors.rightMargin: 3
                y: fileList.visibleArea.yPosition * fileList.height
                width: 4; radius: 2
                height: Math.max(30,
                    fileList.visibleArea.heightRatio * fileList.height)
                color: "#A8B1C2"
                visible: fileList.visibleArea.heightRatio < 1
            }
        }
        Text {
            anchors.centerIn: parent
            visible: entryModel.rowCount() === 0
            text: launcher.filter === ""
                  ? "このフォルダにプロジェクトがありません"
                  : "一致するプロジェクトがありません"
            color: theme.dim; font.pixelSize: 15
        }
    }

    // ----- preview pane (list mode)
    Rectangle {
        id: previewPane
        visible: root.listMode && root.previewW > 0
        x: root.gridX + (root.width - root.gridX - 24 - root.previewW) + 13
        y: root.contentTop - 28
        width: Math.max(40, root.previewW)
        height: root.height - root.footerH - 11 - y
        color: theme.panel
        Rectangle {
            x: 0; y: 0; width: 1; height: parent.height
            color: "#E3E7F0"
        }
        Text {
            x: 18; y: 14; text: "プレビュー"; color: theme.strong
            font.pixelSize: 14; font.bold: true
        }
        Text {
            x: 18; y: 66; width: parent.width - 36
            visible: !launcher.hasSelection
            text: "ファイルを選択してください"
            color: theme.dim; font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Item {
            visible: launcher.hasSelection
            anchors.fill: parent
            Rectangle {
                id: pvBox
                width: Math.max(24, Math.min(parent.width - 36, 236))
                height: Math.min(width * 3 / 4, 180)
                x: (parent.width - width) / 2; y: 55
                radius: 10; color: theme.field; clip: true
                FolderGlyph {
                    anchors.centerIn: parent; scale: 1.6
                    visible: launcher.selectedIsDir
                }
                Image {
                    anchors.fill: parent
                    visible: !launcher.selectedIsDir &&
                             launcher.selectedThumb !== ""
                    source: visible ? launcher.selectedThumb : ""
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                Text {
                    anchors.centerIn: parent
                    visible: !launcher.selectedIsDir &&
                             pvImg.status !== Image.Ready
                    text: "読み込み中..."
                    color: theme.dim; font.pixelSize: 12
                }
                Image {
                    id: pvImg; visible: false
                    source: launcher.selectedThumb
                    asynchronous: true
                }
            }
            Text {
                x: 18; y: pvBox.y + pvBox.height + 22
                width: parent.width - 36
                text: launcher.selectedName; color: theme.strong
                font.pixelSize: 16; font.bold: true; elide: Text.ElideRight
            }
            Text {
                x: 18; y: pvBox.y + pvBox.height + 51
                width: parent.width - 36
                text: launcher.selectedKind; color: theme.dim
                font.pixelSize: 12; elide: Text.ElideRight
            }
            Rectangle {
                x: 18; y: pvBox.y + pvBox.height + 80
                width: parent.width - 36; height: 1
                color: "#E4E8F0"
            }
            Text {
                x: 18; y: pvBox.y + pvBox.height + 96
                text: "情報"; color: theme.strong
                font.pixelSize: 12; font.bold: true
            }
            Text {
                x: 18; y: pvBox.y + pvBox.height + 122
                width: parent.width - 36
                text: "種類  " + launcher.selectedKind
                color: theme.dim; font.pixelSize: 12; elide: Text.ElideRight
            }
            Text {
                x: 18; y: pvBox.y + pvBox.height + 145
                width: parent.width - 36
                text: launcher.selectedMeta
                color: theme.dim; font.pixelSize: 12; elide: Text.ElideRight
            }
        }
    }

    // ----- icon grid
    Item {
        visible: !root.listMode
        x: root.gridX; y: root.contentTop
        width: root.width - root.gridX - 24
        height: root.contentH
        GridView {
            id: fileGrid
            anchors.fill: parent
            clip: true
            model: entryModel
            cellWidth: root.cardW + root.gap
            cellHeight: root.cardH + root.gap
            boundsBehavior: Flickable.StopAtBounds
            delegate: Item {
                width: fileGrid.cellWidth - root.gap
                height: fileGrid.cellHeight - root.gap
                readonly property bool sel:
                    model.path === launcher.selectedPath
                readonly property bool hot: cardMa.containsMouse
                Rectangle {
                    x: 2; y: 5; width: parent.width; height: parent.height
                    radius: 16
                    color: hot ? "#33575E75" : "#15575E75"
                }
                Rectangle {
                    anchors.fill: parent; radius: 16; color: theme.panel
                    border.width: sel ? 2 : 1
                    border.color: sel ? theme.accent
                        : (hot ? "#C9B8F0" : "#E2E7F0")
                    Item {
                        x: 8; y: 8
                        width: parent.width - 16
                        height: Math.round(root.cardW * 0.75) - 12
                        clip: true
                        FolderGlyph {
                            anchors.centerIn: parent; scale: 1.4
                            visible: model.isDir
                        }
                        Rectangle {
                            visible: !model.isDir &&
                                     gridThumbImg.status !== Image.Ready
                            anchors.fill: parent; radius: 8
                            color: "#F2F5FA"
                            Rectangle {
                                width: 36; height: 28; radius: 5
                                anchors.centerIn: parent
                                color: "#DDE3EE"
                                Canvas {
                                    anchors.centerIn: parent
                                    width: 16; height: 14
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.fillStyle = "#B4BED0"
                                        ctx.beginPath()
                                        ctx.moveTo(3, 0); ctx.lineTo(3, 14)
                                        ctx.lineTo(16, 7)
                                        ctx.closePath(); ctx.fill()
                                    }
                                }
                            }
                        }
                        Image {
                            id: gridThumbImg
                            anchors.fill: parent
                            visible: !model.isDir && model.thumbUrl !== ""
                            source: visible ? model.thumbUrl : ""
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                        }
                    }
                    Text {
                        x: 12; y: Math.round(root.cardW * 0.75) - 12 + 14
                        width: parent.width - 24
                        text: model.isDir ? model.displayName : model.displayName
                        color: theme.strong; font.pixelSize: 14; font.bold: true
                        elide: Text.ElideRight
                    }
                    Text {
                        x: 12; y: Math.round(root.cardW * 0.75) - 12 + 32
                        width: parent.width - 24
                        text: model.isDir
                              ? "フォルダ"
                              : model.ext + "  /  " + model.sizeText +
                                (model.dateText === "—" ? "" : "  ·  " + model.dateText)
                        color: theme.dim; font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
                MouseArea {
                    id: cardMa; anchors.fill: parent; hoverEnabled: true
                    onClicked: launcher.selectAt(index)
                    onDoubleClicked: launcher.activateAt(index)
                }
            }
            Rectangle {
                anchors.right: parent.right; anchors.rightMargin: 0
                y: fileGrid.visibleArea.yPosition * fileGrid.height
                width: 4; radius: 2
                height: Math.max(30,
                    fileGrid.visibleArea.heightRatio * fileGrid.height)
                color: "#A8B1C2"
                visible: fileGrid.visibleArea.heightRatio < 1
            }
        }
        Text {
            anchors.centerIn: parent
            visible: entryModel.rowCount() === 0
            text: launcher.filter === ""
                  ? "このフォルダにプロジェクトがありません"
                  : "一致するプロジェクトがありません"
            color: theme.dim; font.pixelSize: 15
        }
        Text {
            anchors.centerIn: parent; anchors.verticalCenterOffset: 24
            visible: entryModel.rowCount() === 0 && launcher.filter === ""
            text: ".sb3 / .sb2 をこのフォルダに置いてください"
            color: theme.muted; font.pixelSize: 12
        }
    }

    // ----- selected project inspector (grid mode, fits under short folders)
    // Like the SDL launcher, it uses the space below the last card row and
    // hides when the folder has enough rows to fill the view.
    Rectangle {
        id: detailPanel
        readonly property int rowsH: Math.min(
            fileGrid.count * (root.cardH + root.gap),
            root.contentH)
        readonly property int availableH:
            root.contentH - rowsH - 12 - 18
        visible: !root.listMode && launcher.hasSelection && availableH >= 184
        x: root.gridX
        y: root.contentTop + rowsH + 12
        width: root.width - root.gridX - 24
        height: availableH
        radius: 18; color: theme.panel
        border.width: 1; border.color: "#E2E7F1"
        Rectangle {
            id: detailPreview
            width: Math.min(250, (parent.height - 28) * 4 / 3)
            height: width * 3 / 4
            x: 16; y: (parent.height - height) / 2
            radius: 11; color: theme.field; clip: true
            FolderGlyph {
                anchors.centerIn: parent; scale: 1.8
                visible: launcher.selectedIsDir
            }
            Image {
                id: detailThumbImg
                anchors.fill: parent
                visible: !launcher.selectedIsDir &&
                         launcher.selectedThumb !== ""
                source: visible ? launcher.selectedThumb : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            Text {
                anchors.centerIn: parent
                visible: !launcher.selectedIsDir &&
                         detailThumbImg.status !== Image.Ready
                text: "プレビューを読み込み中"
                color: theme.dim; font.pixelSize: 12
            }
        }
        Text {
            x: detailPreview.x + detailPreview.width + 25; y: 21
            text: launcher.selectedIsDir ? "FOLDER" : "PROJECT PREVIEW"
            color: theme.accent; font.pixelSize: 11; font.bold: true
        }
        Text {
            x: detailPreview.x + detailPreview.width + 25; y: 48
            width: Math.max(32, parent.width - x - 20)
            text: launcher.selectedName; color: theme.strong
            font.pixelSize: 19; font.bold: true; elide: Text.ElideRight
        }
        Text {
            x: detailPreview.x + detailPreview.width + 25; y: 83
            width: Math.max(32, parent.width - x - 20)
            text: launcher.selectedIsDir ? "フォルダを開いて内容を表示"
                                         : launcher.selectedMeta
            color: theme.dim; font.pixelSize: 13; elide: Text.ElideRight
        }
        Row {
            x: detailPreview.x + detailPreview.width + 25
            y: parent.height - 37; spacing: 8
            Rectangle {
                width: 8; height: 8; radius: 4; color: theme.green
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: launcher.selectedIsDir ? "Enter  フォルダを開く"
                                             : "Enter  実行   ·   E  編集"
                color: theme.fg; font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // =========================================================
    // folder glyph (shared)
    // =========================================================
    component FolderGlyph: Item {
        width: 72; height: 56
        Rectangle {
            x: 0; y: 10; width: 72; height: 46; radius: 9
            color: theme.folder
        }
        Rectangle {
            x: 0; y: 0; width: 30; height: 18; radius: 7
            color: theme.folderDark
        }
        Rectangle {
            x: 0; y: 8; width: 72; height: 20; radius: 8
            color: theme.folderDark
        }
        Rectangle {
            x: 2; y: 12; width: 68; height: 42; radius: 8
            color: theme.folderLight
        }
        Rectangle {
            x: 2; y: 12; width: 68; height: 8
            color: theme.folder
        }
    }

    // =========================================================
    // footer: status + action bar
    // =========================================================
    Rectangle {
        x: root.sideW; y: root.height - root.footerH
        width: root.width - root.sideW; height: root.footerH
        color: theme.panel
        Rectangle {
            x: 0; y: 0; width: parent.width; height: 1
            color: "#E1E6F0"
        }
        // status line
        Item {
            x: root.gridX - root.sideW; y: 14
            width: parent.width - (root.gridX - root.sideW) - 14
            height: 18
            Rectangle {
                width: 8; height: 8; radius: 4
                x: 5; y: 5
                color: launcher.statusIsError ? theme.warn : theme.green
                visible: launcher.statusText !== ""
            }
            Text {
                x: 17; y: 0; width: parent.width - 21
                text: launcher.statusText !== ""
                      ? launcher.statusText
                      : (launcher.hasSelection
                         ? launcher.selectedName + "  /  " + launcher.selectedMeta
                         : "プロジェクトを選んでください")
                color: launcher.statusText !== ""
                       ? (launcher.statusIsError ? theme.warn : theme.fg)
                       : (launcher.hasSelection ? theme.strong : theme.dim)
                font.pixelSize: 13; elide: Text.ElideRight
            }
        }
        // AOT progress bar
        Rectangle {
            x: root.gridX - root.sideW + 17; y: 41
            width: Math.max(0, parent.width - x - 21)
            height: 5; radius: 2; color: theme.field
            visible: launcher.aotProgress >= 0
            Rectangle {
                width: parent.width * Math.max(0, Math.min(1, launcher.aotProgress))
                height: parent.height; radius: 2; color: theme.accent
            }
        }
        // buttons
        PillButton {
            id: bQuit
            implicitWidth: 70; implicitHeight: 36
            x: root.width - 20 - width; y: root.height - 49
            label: "終了"
            onClicked: launcher.quitApp()
        }
        PillButton {
            id: bRun
            implicitWidth: 118; implicitHeight: 36
            x: bQuit.x - 8 - width; y: root.height - 49
            label: launcher.projectSelected ? "実行  ▶" : "フォルダを開く"
            accent: true
            enabled: launcher.hasSelection && launcher.playAvailable
            onClicked: launcher.activate()
        }
        PillButton {
            id: bEdit
            implicitWidth: 76; implicitHeight: 36
            x: bRun.x - 8 - width; y: root.height - 49
            label: "編集"
            enabled: launcher.projectSelected
            onClicked: launcher.openEditor()
        }
        PillButton {
            id: bAotRun
            implicitWidth: 88; implicitHeight: 36
            x: bEdit.x - 8 - width; y: root.height - 49
            label: "AOT 実行"
            enabled: launcher.projectSelected && launcher.jitAvailable
            onClicked: launcher.runAotProject()
        }
        PillButton {
            id: bExport
            implicitWidth: 112; implicitHeight: 36
            x: bAotRun.x - 8 - width; y: root.height - 49
            label: "AOT 書き出し"
            enabled: launcher.projectSelected && launcher.jitAvailable
            onClicked: launcher.exportAot()
        }
    }

    // =========================================================
    // settings sheet
    // =========================================================
    Item {
        id: settingsPanel
        visible: false
        anchors.fill: parent
        Rectangle {
            anchors.fill: parent; color: "#A216243B"
            MouseArea {
                anchors.fill: parent
                onClicked: settingsPanel.visible = false
            }
        }
        Rectangle {
            id: sheet
            width: Math.min(676, parent.width - 42)
            height: Math.min(505, parent.height - 32)
            anchors.centerIn: parent
            radius: 18; color: theme.panel
            border.width: 1; border.color: "#D9E0EC"
            MouseArea { anchors.fill: parent }   // swallow clicks
            Text {
                x: 24; y: 18; text: "実行設定"; color: theme.strong
                font.pixelSize: 21; font.bold: true
            }
            Text {
                x: 24; y: 48
                text: "選んだプロジェクトを実行するときに使用するフラグ"
                color: theme.dim; font.pixelSize: 12
            }
            PillButton {
                x: parent.width - 49; y: 16
                implicitWidth: 32; implicitHeight: 32
                label: "×"
                onClicked: settingsPanel.visible = false
            }
            // choice rows
            component ChoiceRow: Item {
                property string label: ""
                property var labels: []
                property int active: 0
                property bool enabled: true
                signal picked(int index)
                width: sheet.width - 52; height: 35
                Text {
                    x: 0; y: 9; width: 130
                    text: parent.label; color: theme.fg
                    font.pixelSize: 13; font.bold: true
                }
                Row {
                    x: 139; y: 0; spacing: 6
                    width: parent.width - 165; height: 35
                    Repeater {
                        model: parent.parent.labels.length
                        delegate: PillButton {
                            required property int index
                            required property var modelData
                            implicitWidth: Math.floor(
                                (parent.width - 6 * (parent.parent.labels.length - 1)) /
                                parent.parent.labels.length)
                            implicitHeight: 35
                            label: parent.parent.labels[index]
                            accent: index === parent.parent.active
                            enabled: parent.parent.enabled
                            onClicked: parent.parent.picked(index)
                        }
                    }
                }
            }
            Column {
                x: 26; y: 83; spacing: 26; width: sheet.width - 52
                ChoiceRow {
                    label: "画面倍率"; labels: ["1×", "2×", "3×", "4×"]
                    active: launcher.optScale - 1
                    onPicked: function(i) { launcher.optScale = i + 1 }
                }
                ChoiceRow {
                    label: "フレームレート"; labels: ["自動", "30", "60", "120"]
                    active: launcher.optFps === -1 ? 0
                          : launcher.optFps === 30 ? 1
                          : launcher.optFps === 60 ? 2 : 3
                    onPicked: function(i) {
                        launcher.optFps = [-1, 30, 60, 120][i]
                    }
                }
                ChoiceRow {
                    label: "JIT 実行方式"; labels: ["HotSpot", "Eager", "OFF"]
                    active: launcher.jitAvailable ? launcher.optJit : 2
                    enabled: launcher.jitAvailable
                    onPicked: function(i) { launcher.optJit = i }
                }
                ChoiceRow {
                    label: "遅延 JIT"; labels: ["自動", "ON", "OFF"]
                    active: launcher.jitAvailable ? launcher.optLazy : 2
                    enabled: launcher.jitAvailable && launcher.optJit !== 2
                    onPicked: function(i) { launcher.optLazy = i }
                }
                ChoiceRow {
                    label: "GPU 描画 (実験的)"; labels: ["通常", "Ray", "Path"]
                    active: launcher.optGpu + 1
                    onPicked: function(i) { launcher.optGpu = i - 1 }
                }
            }
            PillButton {
                x: 26; y: 394; implicitWidth: 185; implicitHeight: 36
                label: launcher.optTurbo ? "✓ Turbo 強制 ON" : "Turbo 自動"
                accent: launcher.optTurbo
                onClicked: launcher.optTurbo = !launcher.optTurbo
            }
            PillButton {
                x: 220; y: 394; implicitWidth: 226; implicitHeight: 36
                label: launcher.optDanger ? "✓ 危険モード有効" : "危険モード無効"
                accent: launcher.optDanger
                onClicked: launcher.optDanger = !launcher.optDanger
            }
            Text {
                x: 26; y: 446; width: sheet.width - 52
                text: launcher.optDanger
                      ? "注意: スクリプトの実行時間制限が解除されます"
                      : "設定は保存されます。危険モードだけは毎回無効に戻ります。"
                color: launcher.optDanger ? theme.warn : theme.dim
                font.pixelSize: 12; wrapMode: Text.Wrap
            }
        }
    }

    // =========================================================
    // busy overlay (AOT compile 中など)
    // =========================================================
    Rectangle {
        anchors.fill: parent
        visible: launcher.busy
        color: "#3316243B"
        MouseArea { anchors.fill: parent; enabled: parent.visible }
    }

    // =========================================================
    // drag & drop
    // =========================================================
    DropArea {
        anchors.fill: parent
        onDropped: function(drop) {
            if (drop.hasUrls && drop.urls.length > 0) {
                const p = decodeURIComponent(
                    String(drop.urls[0]).replace(/^file:\/\//, ""))
                launcher.openDir(p)
            }
        }
    }

    // =========================================================
    // keyboard
    // =========================================================
    focus: true
    Keys.onPressed: function(ev) {
        if (settingsPanel.visible) {
            if (ev.key === Qt.Key_Escape ||
                ev.key === Qt.Key_Return || ev.key === Qt.Key_Enter) {
                settingsPanel.visible = false
                ev.accepted = true
            }
            return
        }
        if (searchInput.activeFocus || pathInput.visible) return
        const ctrl = ev.modifiers & (Qt.ControlModifier | Qt.MetaModifier)
        const alt = ev.modifiers & Qt.AltModifier
        if (ctrl && ev.key === Qt.Key_Comma) {
            settingsPanel.visible = !settingsPanel.visible
        } else if (ev.key === Qt.Key_F4) {
            settingsPanel.visible = !settingsPanel.visible
        } else if (ctrl && (ev.key === Qt.Key_L || ev.key === Qt.Key_O)) {
            pathInput.open()
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
            launcher.moveSelectionRows(-Math.max(1,
                Math.floor(root.contentH / 40)), 1)
        } else if (ev.key === Qt.Key_PageDown) {
            launcher.moveSelectionRows(Math.max(1,
                Math.floor(root.contentH / 40)), 1)
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

    // selection follow + scripted scroll
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
