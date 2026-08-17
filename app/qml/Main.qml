import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtMultimedia

ApplicationWindow {
    id: root
    width: 1500
    height: 920
    minimumWidth: 1080
    minimumHeight: 700
    visible: true
    title: projectController.hasProject
        ? qsTr("Motus — %1%2").arg(projectController.projectName).arg(projectController.dirty ? " •" : "")
        : qsTr("Motus — Rough Cut")
    color: ink

    readonly property color ink: "#091018"
    readonly property color surface: "#111a24"
    readonly property color surfaceRaised: "#16222e"
    readonly property color surfaceSoft: "#1b2936"
    readonly property color line: "#2b3a48"
    readonly property color lineStrong: "#425465"
    readonly property color clay: "#e27a67"
    readonly property color claySoft: "#7f3f35"
    readonly property color cyan: "#62d3e8"
    readonly property color textPrimary: "#f0ede6"
    readonly property color textSecondary: "#9da8b5"
    readonly property color textFaint: "#617181"
    readonly property color danger: "#e46f74"
    readonly property int trackHeaderWidth: 154
    readonly property int timelineLeftPadding: 14
    property real playheadX: trackHeaderWidth + timelineLeftPadding +
        projectController.playheadFrame * timelineScale
    property real timelineScale: Math.max(0.16,
        Math.min(6, ((timelineViewport.width - trackHeaderWidth - 36) /
        Math.max(240, projectController.durationFrames)) * projectController.timelineZoom))
    readonly property bool editingText: mediaSearch.activeFocus || markerLabel.activeFocus ||
        moveFrame.activeFocus || (moveFrame.contentItem && moveFrame.contentItem.activeFocus) ||
        trimFrame.activeFocus || (trimFrame.contentItem && trimFrame.contentItem.activeFocus)
    property string pendingRelinkAssetId: ""

    function withAlpha(value, alpha) {
        return Qt.rgba(value.r, value.g, value.b, alpha)
    }

    function trackByKind(kind) {
        for (let index = 0; index < projectController.trackItems.length; ++index) {
            const item = projectController.trackItems[index]
            if (item.kind === kind) return item
        }
        return null
    }

    component VectorIcon: Item {
        id: vectorIcon
        required property string name
        property real iconOpacity: 0.78
        objectName: "motus-icon-" + name
        implicitWidth: 24
        implicitHeight: 24
        Image {
            anchors.centerIn: parent
            width: 24
            height: 24
            source: "qrc:/qt/qml/Motus/app/assets/icons/" + vectorIcon.name + ".svg"
            sourceSize.width: 48
            sourceSize.height: 48
            fillMode: Image.PreserveAspectFit
            asynchronous: false
            cache: true
            smooth: true
            mipmap: true
            opacity: vectorIcon.iconOpacity
        }
    }

    component IconButton: ToolButton {
        id: control
        required property string iconName
        required property string tip
        property bool active: false
        property color activeColor: root.clay
        property bool compact: false
        implicitWidth: compact ? 30 : 36
        implicitHeight: compact ? 30 : 36
        padding: 0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        display: AbstractButton.IconOnly
        Accessible.name: tip
        background: Rectangle {
            radius: 6
            color: control.active ? root.withAlpha(control.activeColor, 0.16)
                : control.down ? "#253746" : control.hovered ? root.surfaceSoft : "transparent"
            border.color: control.active ? control.activeColor
                : control.visualFocus ? root.cyan : "transparent"
            border.width: 1
        }
        contentItem: VectorIcon {
            name: control.iconName
            iconOpacity: !control.enabled ? 0.28
                : control.active || control.hovered || control.visualFocus ? 1.0 : 0.78
            anchors.centerIn: parent
        }
        ToolTip.delay: 450
        ToolTip.visible: (hovered || visualFocus) && tip.length > 0
        ToolTip.text: tip
    }

    component PanelLabel: Label {
        required property string caption
        text: caption.toUpperCase()
        color: root.textSecondary
        font.family: "Segoe UI Variable"
        font.pixelSize: 10
        font.weight: Font.DemiBold
        font.letterSpacing: 1.25
        leftPadding: 12
        rightPadding: 8
    }

    component SurfacePanel: Rectangle {
        color: root.surface
        border.color: root.line
        border.width: 1
        radius: 6
    }

    component EmptyHint: Column {
        property string iconName: "film"
        property string title: ""
        property string detail: ""
        spacing: 8
        VectorIcon { name: parent.iconName; iconOpacity: 0.38; anchors.horizontalCenter: parent.horizontalCenter }
        Label {
            text: parent.title
            color: root.textSecondary
            font.pixelSize: 12
            font.weight: Font.DemiBold
            anchors.horizontalCenter: parent.horizontalCenter
        }
        Label {
            text: parent.detail
            color: root.textFaint
            font.pixelSize: 10
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            width: 240
            anchors.horizontalCenter: parent.horizontalCenter
        }
    }

    menuBar: MenuBar {
        Menu {
            title: qsTr("File")
            Action { text: qsTr("New Project"); onTriggered: { projectController.newProject("Untitled"); saveDialog.open() } }
            Action { text: qsTr("Open…"); onTriggered: openDialog.open() }
            Action { text: qsTr("Save"); shortcut: StandardKey.Save; enabled: projectController.hasProject; onTriggered: projectController.save() }
            Action { text: qsTr("Save As…"); enabled: projectController.hasProject; onTriggered: saveDialog.open() }
            MenuSeparator {}
            Action { text: qsTr("Export simple rough cut…"); enabled: projectController.canPreview && !projectController.exporting; onTriggered: exportDialog.open() }
            Action { text: qsTr("Cancel export"); enabled: projectController.exporting; onTriggered: projectController.cancelExport() }
            MenuSeparator {}
            Action { text: qsTr("Write diagnostic MLT graph…"); enabled: projectController.hasProject; onTriggered: graphDialog.open() }
        }
        Menu {
            title: qsTr("Edit")
            Action { text: qsTr("Undo"); shortcut: StandardKey.Undo; enabled: !root.editingText && projectController.canUndo; onTriggered: projectController.undo() }
            Action { text: qsTr("Redo"); shortcut: StandardKey.Redo; enabled: !root.editingText && projectController.canRedo; onTriggered: projectController.redo() }
            MenuSeparator {}
            Action { text: qsTr("Split selected clip at playhead"); shortcut: "S"; enabled: !root.editingText && projectController.hasProject; onTriggered: projectController.splitAtFrame(projectController.playheadFrame) }
            Action { text: qsTr("Ripple-remove selected clip range"); shortcut: "Shift+Delete"; enabled: !root.editingText && projectController.hasSelectedClip; onTriggered: projectController.removeSelectedClip() }
            Action { text: qsTr("Ripple-delete in/out range"); enabled: projectController.hasInOutRange; onTriggered: projectController.rippleDeleteInOut() }
        }
        Menu {
            title: qsTr("View")
            Action { text: qsTr("Zoom In"); shortcut: "+"; enabled: !root.editingText; onTriggered: projectController.zoomIn() }
            Action { text: qsTr("Zoom Out"); shortcut: "-"; enabled: !root.editingText; onTriggered: projectController.zoomOut() }
            Action { text: qsTr("Fit Timeline"); shortcut: "0"; enabled: !root.editingText; onTriggered: projectController.resetZoom() }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 4

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 46
            color: root.surfaceRaised
            border.color: root.line
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 4
                Image {
                    source: "qrc:/qt/qml/Motus/app/assets/motus-mark.svg"
                    Layout.preferredWidth: 28
                    Layout.preferredHeight: 28
                    Layout.rightMargin: 4
                    fillMode: Image.PreserveAspectFit
                    Accessible.ignored: true
                }
                Label {
                    text: "MOTUS"
                    color: root.textPrimary
                    font.family: "Bahnschrift"
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    font.letterSpacing: 2.2
                    Layout.rightMargin: 8
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: root.line; Layout.leftMargin: 2; Layout.rightMargin: 4 }
                IconButton { iconName: "folder"; tip: qsTr("Open project"); onClicked: openDialog.open() }
                IconButton { iconName: "save"; tip: qsTr("Save project"); enabled: projectController.hasProject; onClicked: projectController.save() }
                IconButton { iconName: "import"; tip: qsTr("Reference media in place"); enabled: projectController.hasProject; onClicked: mediaDialog.open() }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: root.line; Layout.leftMargin: 4; Layout.rightMargin: 4 }
                IconButton { iconName: "undo"; tip: qsTr("Undo"); enabled: projectController.canUndo; onClicked: projectController.undo() }
                IconButton { iconName: "redo"; tip: qsTr("Redo"); enabled: projectController.canRedo; onClicked: projectController.redo() }
                IconButton { iconName: "cut"; tip: qsTr("Split selected clip at playhead"); enabled: projectController.hasProject; activeColor: root.clay; onClicked: projectController.splitAtFrame(projectController.playheadFrame) }
                IconButton { iconName: "delete"; tip: qsTr("Ripple-remove this clip's full time range across every unlocked track"); enabled: projectController.hasSelectedClip; activeColor: root.danger; onClicked: projectController.removeSelectedClip() }
                Item { Layout.fillWidth: true }
                ColumnLayout {
                    Layout.maximumWidth: 320
                    spacing: 0
                    Label {
                        Layout.fillWidth: true
                        text: projectController.hasProject ? projectController.projectName : qsTr("No project")
                        color: projectController.dirty ? root.clay : root.textPrimary
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideMiddle
                    }
                    Label {
                        Layout.fillWidth: true
                        text: projectController.hasProject ? projectController.sequenceName : qsTr("Rough-cut workspace")
                        color: root.textFaint
                        font.pixelSize: 9
                        horizontalAlignment: Text.AlignRight
                    }
                }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 24; color: root.line; Layout.leftMargin: 8; Layout.rightMargin: 6 }
                IconButton { iconName: "graph"; tip: qsTr("Write diagnostic MLT graph (not a finished export)"); enabled: projectController.hasProject; onClicked: graphDialog.open() }
                IconButton {
                    iconName: projectController.exporting ? "cancel" : "export"
                    tip: projectController.exporting ? qsTr("Cancel active export") : qsTr("Export supported rough cut to H.264/AAC MP4")
                    enabled: projectController.exporting || projectController.canPreview
                    active: projectController.exporting
                    activeColor: projectController.exporting ? root.danger : root.clay
                    onClicked: projectController.exporting ? projectController.cancelExport() : exportDialog.open()
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 510
            orientation: Qt.Horizontal

            SurfacePanel {
                SplitView.preferredWidth: 276
                SplitView.minimumWidth: 220
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        PanelLabel { caption: qsTr("Media") }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: projectController.assetItems.length
                            color: root.textFaint
                            font.pixelSize: 10
                            rightPadding: 3
                        }
                        IconButton {
                            compact: true
                            iconName: "refresh"
                            tip: qsTr("Refresh media integrity")
                            enabled: projectController.hasProject && projectController.assetItems.length > 0
                            onClicked: projectController.refreshMediaIntegrity()
                        }
                        IconButton { compact: true; iconName: "import"; tip: qsTr("Reference media in place"); enabled: projectController.hasProject; onClicked: mediaDialog.open() }
                        Item { Layout.preferredWidth: 4 }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        Layout.bottomMargin: 7
                        radius: 5
                        color: root.ink
                        border.color: mediaSearch.activeFocus ? root.cyan : root.line
                        TextField {
                            id: mediaSearch
                            anchors.fill: parent
                            anchors.leftMargin: 7
                            anchors.rightMargin: 7
                            placeholderText: qsTr("Filter media")
                            color: root.textPrimary
                            placeholderTextColor: root.textFaint
                            font.pixelSize: 11
                            background: Item {}
                            Accessible.name: qsTr("Filter media")
                        }
                    }
                    ListView {
                        id: assetList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 2
                        model: projectController.assetItems
                        delegate: Rectangle {
                            id: assetDelegate
                            required property var modelData
                            width: ListView.view.width
                            height: visible ? 60 : 0
                            visible: mediaSearch.text.length === 0 ||
                                modelData.name.toLowerCase().includes(mediaSearch.text.toLowerCase()) ||
                                modelData.path.toLowerCase().includes(mediaSearch.text.toLowerCase())
                            color: assetMouse.containsMouse ? root.surfaceSoft : "transparent"
                            Rectangle {
                                width: 38
                                height: 38
                                x: 9
                                y: 10
                                radius: 5
                                color: root.ink
                                border.color: modelData.provisional ? root.claySoft : root.line
                                VectorIcon {
                                    anchors.centerIn: parent
                                    name: modelData.hasVideo ? "film" : "wave"
                                    iconOpacity: modelData.online ? 0.78 : 0.36
                                }
                            }
                            Column {
                                x: 56
                                y: 10
                                width: parent.width - (modelData.relinkable ? 104 : 66)
                                spacing: 3
                                Label { width: parent.width; text: modelData.name; color: root.textPrimary; font.pixelSize: 11; elide: Text.ElideRight }
                                Label {
                                    width: parent.width
                                    text: modelData.status + " · " + root.formatCompactFrames(modelData.durationFrames)
                                    color: modelData.online ? root.textFaint : root.danger
                                    font.pixelSize: 9
                                    elide: Text.ElideRight
                                }
                            }
                            MouseArea { id: assetMouse; anchors.fill: parent; hoverEnabled: true; z: 0 }
                            IconButton {
                                z: 2
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                compact: true
                                iconName: "relink"
                                tip: modelData.missing ? qsTr("Relink missing media")
                                                       : qsTr("Accept a replacement for modified media")
                                visible: modelData.relinkable
                                enabled: visible
                                onClicked: {
                                    root.pendingRelinkAssetId = modelData.id
                                    relinkDialog.open()
                                }
                            }
                            ToolTip { visible: assetMouse.containsMouse; delay: 600; text: modelData.path }
                        }
                        EmptyHint {
                            anchors.centerIn: parent
                            visible: assetList.count === 0
                            iconName: "import"
                            title: projectController.hasProject ? qsTr("No media yet") : qsTr("Open a project")
                            detail: projectController.hasProject
                                ? qsTr("Reference files in place. Motus never alters originals.")
                                : qsTr("Create or open a project before adding footage.")
                        }
                    }
                }
            }

            SurfacePanel {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 480
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        PanelLabel { caption: qsTr("Program") }
                        Label {
                            text: qsTr("CLIP PREVIEW · EDIT TIMING STAYS FRAME-BASED")
                            color: root.textFaint
                            font.pixelSize: 8
                            font.letterSpacing: 0.8
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            text: projectController.playheadTimecode
                            color: root.cyan
                            font.family: "Cascadia Mono"
                            font.pixelSize: 13
                            rightPadding: 12
                        }
                    }
                    Rectangle {
                        id: previewSurface
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.margins: 8
                        color: "#05080c"
                        border.color: root.line
                        VideoOutput {
                            id: programVideo
                            anchors.fill: parent
                            anchors.margins: 1
                            fillMode: VideoOutput.PreserveAspectFit
                            Component.onCompleted: projectController.setVideoOutput(programVideo)
                        }
                        EmptyHint {
                            anchors.centerIn: parent
                            visible: !projectController.canPreview
                            iconName: "film"
                            title: projectController.hasProject ? qsTr("Clip preview unavailable") : qsTr("No active rough cut")
                            detail: projectController.hasProject
                                ? projectController.previewDetail
                                : qsTr("Create or open a project to begin arranging a rough cut.")
                        }
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredHeight: 46
                        spacing: 5
                        IconButton { iconName: "stepBack"; tip: qsTr("Previous frame"); enabled: projectController.hasProject; activeColor: root.cyan; onClicked: projectController.stepPlayhead(-1) }
                        IconButton {
                            iconName: projectController.playing ? "pause" : "play"
                            tip: projectController.playing ? qsTr("Pause selected-clip preview (Space)") : qsTr("Play selected-clip preview (Space)")
                            enabled: projectController.canPreview
                            active: projectController.playing
                            activeColor: root.cyan
                            onClicked: projectController.togglePlayback()
                        }
                        IconButton { iconName: "stepForward"; tip: qsTr("Next frame"); enabled: projectController.hasProject; activeColor: root.cyan; onClicked: projectController.stepPlayhead(1) }
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 12
                        Layout.rightMargin: 12
                        Layout.bottomMargin: 7
                        horizontalAlignment: Text.AlignHCenter
                        text: projectController.previewDetail
                        color: root.textFaint
                        font.pixelSize: 8
                        elide: Text.ElideRight
                    }
                }
            }

            SurfacePanel {
                SplitView.preferredWidth: 292
                SplitView.minimumWidth: 238
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    TabBar {
                        id: rightTabs
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        TabButton { text: qsTr("Inspector") }
                        TabButton { text: qsTr("Markers") }
                    }
                    StackLayout {
                        currentIndex: rightTabs.currentIndex
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Flickable {
                            contentHeight: inspectorColumn.implicitHeight + 24
                            clip: true
                            ColumnLayout {
                                id: inspectorColumn
                                x: 12
                                y: 12
                                width: parent.width - 24
                                spacing: 10
                                EmptyHint {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.topMargin: 70
                                    visible: !projectController.hasSelectedClip
                                    iconName: "cut"
                                    title: qsTr("Select a timeline clip")
                                    detail: qsTr("Selection is clay. Time and navigation stay cyan.")
                                }
                                Label { visible: projectController.hasSelectedClip; text: projectController.selectedClipName; color: root.textPrimary; font.pixelSize: 14; font.weight: Font.DemiBold; Layout.fillWidth: true; elide: Text.ElideRight }
                                Label { visible: projectController.hasSelectedClip; text: projectController.selectedClipKind + " · linked A/V edit"; color: root.clay; font.pixelSize: 9; font.letterSpacing: 0.6 }
                                Rectangle { visible: projectController.hasSelectedClip; Layout.fillWidth: true; Layout.preferredHeight: 1; color: root.line }
                                Label { visible: projectController.hasSelectedClip; text: qsTr("START FRAME"); color: root.textFaint; font.pixelSize: 9 }
                                SpinBox {
                                    id: moveFrame
                                    visible: projectController.hasSelectedClip
                                    Layout.fillWidth: true
                                    from: 0
                                    to: Math.max(0, projectController.durationFrames)
                                    editable: true
                                    value: projectController.selectedClipStartFrame
                                    Accessible.name: qsTr("Selected clip start frame")
                                }
                                Button {
                                    visible: projectController.hasSelectedClip
                                    Layout.fillWidth: true
                                    text: qsTr("Move linked clip")
                                    onClicked: projectController.moveSelectedToFrame(moveFrame.value)
                                }
                                Label { visible: projectController.hasSelectedClip; text: qsTr("TRIM AT FRAME"); color: root.textFaint; font.pixelSize: 9 }
                                SpinBox {
                                    id: trimFrame
                                    visible: projectController.hasSelectedClip
                                    Layout.fillWidth: true
                                    from: projectController.selectedClipStartFrame
                                    to: Math.max(from, projectController.selectedClipEndFrame)
                                    editable: true
                                    value: projectController.playheadFrame
                                    Accessible.name: qsTr("Selected clip trim frame")
                                }
                                RowLayout {
                                    visible: projectController.hasSelectedClip
                                    Layout.fillWidth: true
                                    Button { Layout.fillWidth: true; text: qsTr("Trim start"); onClicked: projectController.trimSelectedStartToFrame(trimFrame.value) }
                                    Button { Layout.fillWidth: true; text: qsTr("Trim end"); onClicked: projectController.trimSelectedEndToFrame(trimFrame.value) }
                                }
                                Label {
                                    visible: projectController.hasSelectedClip
                                    Layout.fillWidth: true
                                    text: qsTr("%1 → %2  ·  %3 frames")
                                        .arg(root.formatCompactFrames(projectController.selectedClipStartFrame))
                                        .arg(root.formatCompactFrames(projectController.selectedClipEndFrame))
                                        .arg(projectController.selectedClipDurationFrames)
                                    color: root.textSecondary
                                    font.family: "Cascadia Mono"
                                    font.pixelSize: 10
                                }
                                Button {
                                    visible: projectController.hasSelectedClip
                                    Layout.fillWidth: true
                                    text: qsTr("Ripple-remove clip range")
                                    onClicked: projectController.removeSelectedClip()
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Deletes this clip's full time range across every unlocked track")
                                }
                            }
                        }
                        ColumnLayout {
                            spacing: 0
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40
                                Layout.leftMargin: 8
                                TextField { id: markerLabel; Layout.fillWidth: true; placeholderText: qsTr("Marker label"); Accessible.name: qsTr("Marker label") }
                                IconButton { compact: true; iconName: "marker"; tip: qsTr("Add marker at playhead"); enabled: projectController.hasProject; activeColor: root.cyan; onClicked: { projectController.addMarker(projectController.playheadFrame, markerLabel.text); markerLabel.clear() } }
                            }
                            ListView {
                                id: markerList
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                clip: true
                                model: projectController.markerItems
                                delegate: Rectangle {
                                    id: clipDelegate
                                    required property var modelData
                                    width: ListView.view.width
                                    height: 46
                                    color: markerMouse.containsMouse ? root.surfaceSoft : "transparent"
                                    Rectangle { x: 10; y: 9; width: 3; height: 28; radius: 2; color: root.cyan }
                                    Column { x: 22; y: 7; width: parent.width - 64; spacing: 2
                                        Label { width: parent.width; text: modelData.label; color: root.textPrimary; font.pixelSize: 11; elide: Text.ElideRight }
                                        Label { text: root.formatCompactFrames(modelData.frame); color: root.cyan; font.family: "Cascadia Mono"; font.pixelSize: 9 }
                                    }
                                    MouseArea { id: markerMouse; anchors.fill: parent; hoverEnabled: true; onClicked: projectController.playheadFrame = modelData.frame }
                                    IconButton { anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter; compact: true; iconName: "clear"; tip: qsTr("Remove marker"); onClicked: projectController.removeMarker(modelData.id) }
                                }
                                EmptyHint { anchors.centerIn: parent; visible: markerList.count === 0; iconName: "marker"; title: qsTr("No markers"); detail: qsTr("Place the playhead, name a beat, and add it here.") }
                            }
                        }
                    }
                }
            }
        }

        SurfacePanel {
            id: timelinePanel
            Layout.fillWidth: true
            Layout.preferredHeight: 304
            Layout.minimumHeight: 246
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 44
                    Layout.leftMargin: 6
                    Layout.rightMargin: 8
                    spacing: 3
                    PanelLabel { caption: projectController.hasProject ? projectController.sequenceName : qsTr("Timeline") }
                    Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; color: root.line; Layout.leftMargin: 3; Layout.rightMargin: 3 }
                    IconButton { compact: true; iconName: "in"; tip: qsTr("Set in point (I)"); enabled: projectController.hasProject; active: projectController.hasInPoint; activeColor: root.cyan; onClicked: projectController.setInPoint(projectController.playheadFrame) }
                    IconButton { compact: true; iconName: "out"; tip: qsTr("Set out point (O)"); enabled: projectController.hasProject; active: projectController.hasOutPoint; activeColor: root.cyan; onClicked: projectController.setOutPoint(projectController.playheadFrame) }
                    IconButton { compact: true; iconName: "clear"; tip: qsTr("Clear in/out range"); enabled: projectController.hasInPoint || projectController.hasOutPoint; onClicked: projectController.clearInOut() }
                    IconButton { compact: true; iconName: "delete"; tip: qsTr("Ripple-delete marked in/out range"); enabled: projectController.hasInOutRange; activeColor: root.danger; onClicked: projectController.rippleDeleteInOut() }
                    Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 22; color: root.line; Layout.leftMargin: 3; Layout.rightMargin: 3 }
                    IconButton { compact: true; iconName: "marker"; tip: qsTr("Add marker at playhead"); enabled: projectController.hasProject; activeColor: root.cyan; onClicked: projectController.addMarker(projectController.playheadFrame, "") }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: projectController.hasInOutRange
                            ? qsTr("IN %1   OUT %2").arg(root.formatCompactFrames(projectController.inPointFrame)).arg(root.formatCompactFrames(projectController.outPointFrame))
                            : qsTr("FRAME %1").arg(projectController.playheadFrame)
                        color: root.cyan
                        font.family: "Cascadia Mono"
                        font.pixelSize: 10
                        rightPadding: 8
                    }
                    IconButton { compact: true; iconName: "magnet"; tip: qsTr("Snap to clip edges and markers"); active: projectController.snapEnabled; activeColor: root.cyan; onClicked: projectController.snapEnabled = !projectController.snapEnabled }
                    IconButton { compact: true; iconName: "zoomOut"; tip: qsTr("Zoom out"); onClicked: projectController.zoomOut() }
                    Label { text: Math.round(projectController.timelineZoom * 100) + "%"; color: root.textFaint; font.pixelSize: 9; Layout.preferredWidth: 38; horizontalAlignment: Text.AlignHCenter }
                    IconButton { compact: true; iconName: "zoomIn"; tip: qsTr("Zoom in"); onClicked: projectController.zoomIn() }
                    IconButton { compact: true; iconName: "fit"; tip: qsTr("Fit timeline"); onClicked: projectController.resetZoom() }
                }

                Flickable {
                    id: timelineViewport
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: Math.max(width, root.trackHeaderWidth + root.timelineLeftPadding * 2 +
                        Math.max(1, projectController.durationFrames) * root.timelineScale + 80)
                    contentHeight: height
                    boundsBehavior: Flickable.StopAtBounds

                    Rectangle {
                        id: timelineCanvas
                        width: timelineViewport.contentWidth
                        height: timelineViewport.height
                        color: "#0d151e"

                        Rectangle {
                            id: ruler
                            x: root.trackHeaderWidth
                            width: parent.width - x
                            height: 30
                            color: "#0b1219"
                            border.color: root.line
                            MouseArea {
                                anchors.fill: parent
                                onPressed: event => projectController.playheadFrame = Math.round((event.x - root.timelineLeftPadding) / root.timelineScale)
                                onPositionChanged: event => { if (pressed) projectController.playheadFrame = Math.round((event.x - root.timelineLeftPadding) / root.timelineScale) }
                            }
                            Repeater {
                                model: Math.max(1, Math.ceil(projectController.durationFrames / root.tickStep()) + 1)
                                delegate: Item {
                                    required property int index
                                    x: root.timelineLeftPadding + index * root.tickStep() * root.timelineScale
                                    width: 1
                                    height: ruler.height
                                    Rectangle { width: 1; height: 8; anchors.bottom: parent.bottom; color: root.lineStrong }
                                    Label { x: 4; y: 4; text: root.formatCompactFrames(index * root.tickStep()); color: root.textFaint; font.family: "Cascadia Mono"; font.pixelSize: 8 }
                                }
                            }
                        }

                        Repeater {
                            model: projectController.markerItems
                            delegate: Item {
                                required property var modelData
                                x: root.trackHeaderWidth + root.timelineLeftPadding + modelData.frame * root.timelineScale - 5
                                y: 20
                                z: 5
                                Rectangle { x: 4; width: 1; height: timelineCanvas.height - 20; color: root.withAlpha(root.cyan, 0.55) }
                                Rectangle { width: 10; height: 10; rotation: 45; color: root.cyan }
                                ToolTip.visible: markerTimelineMouse.containsMouse
                                ToolTip.text: modelData.label + " · " + root.formatCompactFrames(modelData.frame)
                                MouseArea { id: markerTimelineMouse; anchors.fill: parent; anchors.margins: -5; hoverEnabled: true; onClicked: projectController.playheadFrame = modelData.frame }
                            }
                        }

                        Rectangle {
                            visible: projectController.hasInOutRange
                            x: root.trackHeaderWidth + root.timelineLeftPadding + projectController.inPointFrame * root.timelineScale
                            y: 30
                            width: Math.max(1, (projectController.outPointFrame - projectController.inPointFrame) * root.timelineScale)
                            height: timelineCanvas.height - 30
                            color: root.withAlpha(root.cyan, 0.08)
                            border.color: root.withAlpha(root.cyan, 0.45)
                        }

                        Repeater {
                            model: projectController.trackItems
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                property int rowY: 30 + index * 82
                                x: 0
                                y: rowY
                                width: timelineCanvas.width
                                height: 82
                                color: index % 2 === 0 ? "#101923" : "#0d161f"
                                border.color: root.line
                                Rectangle {
                                    width: root.trackHeaderWidth
                                    height: parent.height
                                    color: root.surfaceRaised
                                    border.color: root.line
                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 7
                                        spacing: 2
                                        VectorIcon { name: modelData.kind === "video" ? "film" : "wave"; iconOpacity: 0.78 }
                                        Label { Layout.fillWidth: true; text: modelData.name; color: root.textPrimary; font.pixelSize: 10; elide: Text.ElideRight }
                                        IconButton {
                                            compact: true
                                            iconName: modelData.locked ? "lock" : "unlock"
                                            tip: modelData.locked ? qsTr("Unlock track") : qsTr("Lock track")
                                            active: modelData.locked
                                            activeColor: root.clay
                                            onClicked: projectController.setTrackLocked(modelData.id, !modelData.locked)
                                        }
                                        IconButton {
                                            compact: true
                                            iconName: modelData.kind === "video"
                                                ? (modelData.visible ? "eye" : "eyeOff")
                                                : (modelData.muted ? "mute" : "volume")
                                            tip: modelData.kind === "video"
                                                ? (modelData.visible ? qsTr("Hide video track") : qsTr("Show video track"))
                                                : (modelData.muted ? qsTr("Unmute audio track") : qsTr("Mute audio track"))
                                            active: modelData.kind === "video" ? !modelData.visible : modelData.muted
                                            activeColor: root.clay
                                            onClicked: {
                                                if (modelData.kind === "video") projectController.setTrackVisible(modelData.id, !modelData.visible)
                                                else projectController.setTrackMuted(modelData.id, !modelData.muted)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Repeater {
                            model: projectController.timelineClips
                            delegate: Rectangle {
                                required property var modelData
                                property int trackIndex: modelData.kind === "video" ? 0 : 1
                                x: root.trackHeaderWidth + root.timelineLeftPadding + modelData.startFrame * root.timelineScale
                                y: 40 + trackIndex * 82
                                width: Math.max(18, modelData.durationFrames * root.timelineScale)
                                height: 62
                                radius: 4
                                color: projectController.selectedClipId === modelData.id
                                    ? root.claySoft : modelData.kind === "video" ? "#263b49" : "#1d3541"
                                border.color: projectController.selectedClipId === modelData.id
                                    ? root.clay : modelData.provisional ? root.claySoft : root.lineStrong
                                border.width: projectController.selectedClipId === modelData.id ? 2 : 1
                                clip: true
                                Rectangle { width: 4; height: parent.height; color: modelData.offline ? root.danger : modelData.kind === "video" ? root.clay : root.cyan }
                                Column {
                                    x: 12
                                    y: 9
                                    width: parent.width - 20
                                    spacing: 4
                                    Label { width: parent.width; text: modelData.name; color: root.textPrimary; font.pixelSize: 10; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                    Label { width: parent.width; text: root.formatCompactFrames(modelData.startFrame) + "  ·  " + modelData.durationFrames + "f"; color: root.textSecondary; font.family: "Cascadia Mono"; font.pixelSize: 8; elide: Text.ElideRight }
                                }
                                MouseArea {
                                    id: clipMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: event => {
                                        projectController.selectClip(modelData.id)
                                        projectController.playheadFrame = Math.round(modelData.startFrame + event.x / root.timelineScale)
                                    }
                                }
                                ToolTip {
                                    visible: clipMouse.containsMouse
                                    delay: 550
                                    text: modelData.name + "\n" + root.formatCompactFrames(modelData.startFrame) + " → " + root.formatCompactFrames(modelData.endFrame) + (modelData.provisional ? "\nPending real media probe" : "")
                                }
                            }
                        }

                        Rectangle {
                            x: root.playheadX
                            y: 22
                            width: 1
                            height: timelineCanvas.height - 22
                            color: root.cyan
                            z: 10
                        }
                        Rectangle {
                            x: root.playheadX - 5
                            y: 20
                            width: 11
                            height: 10
                            radius: 2
                            color: root.cyan
                            z: 11
                        }

                        EmptyHint {
                            anchors.centerIn: parent
                            anchors.horizontalCenterOffset: root.trackHeaderWidth / 2
                            visible: projectController.timelineClips.length === 0
                            iconName: "film"
                            title: qsTr("Empty rough cut")
                            detail: qsTr("Reference probed media from the Media panel. Video with audio appends as a linked pair.")
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: "#0b1219"
            border.color: root.line
            Accessible.role: Accessible.StatusBar
            Accessible.name: projectController.status
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                Rectangle { Layout.preferredWidth: 6; Layout.preferredHeight: 6; radius: 3; color: root.cyan; Accessible.ignored: true }
                Label { text: projectController.status; color: root.textSecondary; font.pixelSize: 10; elide: Text.ElideRight; Layout.fillWidth: true }
                ProgressBar {
                    visible: projectController.exporting
                    Layout.preferredWidth: 130
                    from: 0
                    to: 1
                    value: projectController.exportProgress
                    Accessible.name: qsTr("Export progress")
                    ToolTip.visible: hovered
                    ToolTip.text: projectController.exportDetail
                }
                Label {
                    visible: projectController.probing
                    text: qsTr("PROBING %1").arg(projectController.activeProbeCount)
                    color: root.clay
                    font.pixelSize: 9
                }
                Label {
                    text: projectController.hasProject
                        ? qsTr("%1 MEDIA · %2 CLIPS · %3 FRAMES").arg(projectController.assetItems.length).arg(projectController.timelineClips.length).arg(projectController.durationFrames)
                        : ""
                    color: root.textFaint
                    font.pixelSize: 9
                }
            }
        }
    }

    function tickStep() {
        const pixelsPerFrame = timelineScale
        if (pixelsPerFrame >= 3) return 10
        if (pixelsPerFrame >= 1) return 30
        if (pixelsPerFrame >= 0.4) return 90
        return 300
    }

    function formatCompactFrames(frame) {
        return projectController.formatTimecode(Math.max(0, Math.round(frame)))
    }

    FileDialog {
        id: openDialog
        title: qsTr("Open Motus project")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Motus projects (*.veproj)")]
        onAccepted: projectController.openProject(selectedFile)
    }
    FileDialog {
        id: saveDialog
        title: qsTr("Save Motus project")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "veproj"
        nameFilters: [qsTr("Motus projects (*.veproj)")]
        onAccepted: projectController.saveAs(selectedFile)
    }
    FileDialog {
        id: mediaDialog
        title: qsTr("Reference media in place")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Audio and video files (*.mp4 *.mov *.mkv *.mxf *.avi *.wav *.mp3 *.m4a *.flac)"), qsTr("All files (*)")]
        onAccepted: projectController.appendMediaReference(selectedFile)
    }
    FileDialog {
        id: relinkDialog
        title: qsTr("Relink source media")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Audio and video files (*.mp4 *.mov *.mkv *.mxf *.avi *.wav *.mp3 *.m4a *.flac)"), qsTr("All files (*)")]
        onAccepted: {
            if (root.pendingRelinkAssetId.length > 0)
                projectController.relinkMedia(root.pendingRelinkAssetId, selectedFile)
            root.pendingRelinkAssetId = ""
        }
        onRejected: root.pendingRelinkAssetId = ""
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export supported rough cut")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mp4"
        nameFilters: [qsTr("H.264/AAC video (*.mp4)")]
        onAccepted: projectController.exportSequence(selectedFile)
    }
    FileDialog {
        id: graphDialog
        title: qsTr("Write diagnostic MLT graph")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "mlt"
        nameFilters: [qsTr("MLT XML (*.mlt *.xml)")]
        onAccepted: projectController.generateMltGraph(selectedFile)
    }
    Connections { target: projectController; function onSavePathRequired() { saveDialog.open() } }

    Shortcut { sequence: "I"; enabled: !root.editingText; onActivated: projectController.setInPoint(projectController.playheadFrame) }
    Shortcut { sequence: "O"; enabled: !root.editingText; onActivated: projectController.setOutPoint(projectController.playheadFrame) }
    Shortcut { sequence: "M"; enabled: !root.editingText; onActivated: projectController.addMarker(projectController.playheadFrame, "") }
    Shortcut { sequence: "Left"; enabled: !root.editingText; onActivated: projectController.stepPlayhead(-1) }
    Shortcut { sequence: "Right"; enabled: !root.editingText; onActivated: projectController.stepPlayhead(1) }
    Shortcut { sequence: "Escape"; enabled: !root.editingText; onActivated: projectController.clearSelection() }
    Shortcut { sequence: "Space"; enabled: !root.editingText && projectController.canPreview; onActivated: projectController.togglePlayback() }
}
