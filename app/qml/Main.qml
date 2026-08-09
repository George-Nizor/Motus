import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1500
    height: 920
    visible: true
    title: qsTr("Video Editor — Rough Cut")
    color: "#17191d"

    property color panel: "#23262c"
    property color edge: "#353a43"
    property color accent: "#46a0ff"

    menuBar: MenuBar {
        Menu { title: qsTr("File"); Action { text: qsTr("New Project") }
            Action { text: qsTr("Open…") }; Action { text: qsTr("Save"); shortcut: StandardKey.Save }
            MenuSeparator {}; Action { text: qsTr("Export…") }
        }
        Menu { title: qsTr("Edit"); Action { text: qsTr("Undo"); shortcut: StandardKey.Undo }
            Action { text: qsTr("Redo"); shortcut: StandardKey.Redo }
            MenuSeparator {}; Action { text: qsTr("Split"); shortcut: "S" }
            Action { text: qsTr("Ripple Delete"); shortcut: "Shift+Delete" }
        }
        Menu { title: qsTr("View"); Action { text: qsTr("Zoom In"); shortcut: "+" }
            Action { text: qsTr("Zoom Out"); shortcut: "-" }
        }
    }

    component PanelTitle: Label {
        required property string caption
        text: caption.toUpperCase(); color: "#aeb6c2"; font.pixelSize: 11; font.bold: true
        padding: 8
    }
    component EmptyHint: Label {
        required property string message
        text: message; color: "#7f8792"; horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter; wrapMode: Text.WordWrap
    }
    component DarkPanel: Rectangle { color: root.panel; border.color: root.edge; radius: 2 }

    ColumnLayout {
        anchors.fill: parent; spacing: 4
        SplitView {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredHeight: 590
            orientation: Qt.Horizontal

            DarkPanel {
                SplitView.preferredWidth: 270; SplitView.minimumWidth: 190
                ColumnLayout { anchors.fill: parent; spacing: 0
                    PanelTitle { caption: qsTr("Media") }
                    ToolBar { Layout.fillWidth: true
                        RowLayout { anchors.fill: parent
                            ToolButton { text: "+"; ToolTip.text: qsTr("Import media"); ToolTip.visible: hovered }
                            TextField { Layout.fillWidth: true; placeholderText: qsTr("Filter assets") }
                        }
                    }
                    EmptyHint { Layout.fillWidth: true; Layout.fillHeight: true
                        message: qsTr("Drop video, audio, images, or screen recordings here") }
                }
            }

            SplitView {
                orientation: Qt.Vertical; SplitView.fillWidth: true
                DarkPanel {
                    SplitView.fillHeight: true; SplitView.minimumHeight: 280
                    ColumnLayout { anchors.fill: parent; spacing: 0
                        RowLayout { Layout.fillWidth: true
                            PanelTitle { caption: qsTr("Program") }
                            Item { Layout.fillWidth: true }
                            Label { text: "00:00:00:00"; color: "#bac2cc"; rightPadding: 10 }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: "#08090b"
                            EmptyHint { anchors.centerIn: parent; width: 300
                                message: qsTr("Import media to start a rough cut") }
                        }
                        RowLayout { Layout.alignment: Qt.AlignHCenter
                            ToolButton { text: "|◀" }; ToolButton { text: "◀" }
                            ToolButton { text: "▶" }; ToolButton { text: "▶|" }
                        }
                    }
                }
                DarkPanel {
                    SplitView.preferredHeight: 220; SplitView.minimumHeight: 120
                    ColumnLayout { anchors.fill: parent; spacing: 0
                        TabBar { id: contextTabs; Layout.fillWidth: true
                            TabButton { text: qsTr("Transcript & Cleanup") }
                            TabButton { text: qsTr("Source") }
                        }
                        StackLayout { currentIndex: contextTabs.currentIndex; Layout.fillWidth: true; Layout.fillHeight: true
                            EmptyHint { message: qsTr("Analyze selected dialogue to review safe silence and filler suggestions") }
                            EmptyHint { message: qsTr("Select a media-bin item to set source in and out points") }
                        }
                    }
                }
            }

            DarkPanel {
                SplitView.preferredWidth: 290; SplitView.minimumWidth: 210
                ColumnLayout { anchors.fill: parent; spacing: 0
                    TabBar { id: rightTabs; Layout.fillWidth: true
                        TabButton { text: qsTr("Inspector") }; TabButton { text: qsTr("Jobs") }
                    }
                    StackLayout { currentIndex: rightTabs.currentIndex; Layout.fillWidth: true; Layout.fillHeight: true
                        EmptyHint { message: qsTr("Select a clip to edit its properties") }
                        EmptyHint { message: qsTr("Background probes, proxies, waveforms, analysis, and renders appear here") }
                    }
                }
            }
        }

        DarkPanel {
            Layout.fillWidth: true; Layout.preferredHeight: 300; Layout.minimumHeight: 200
            ColumnLayout { anchors.fill: parent; spacing: 0
                RowLayout { Layout.fillWidth: true
                    PanelTitle { caption: qsTr("Timeline — Rough Cut") }
                    Item { Layout.fillWidth: true }
                    ToolButton { text: qsTr("Snap"); checkable: true; checked: true }
                    Slider { from: 0; to: 1; value: 0.35; Layout.preferredWidth: 130 }
                }
                Rectangle { Layout.fillWidth: true; height: 24; color: "#1b1e23"
                    Label { anchors.centerIn: parent; text: "00:00        00:10        00:20        00:30"; color: "#79818c" }
                }
                Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: "#1c1f24"
                    EmptyHint { anchors.centerIn: parent; message: qsTr("Append or insert a source selection") }
                }
            }
        }
    }

    Shortcut { sequence: "J"; onActivated: console.log("reverse playback") }
    Shortcut { sequence: "K"; onActivated: console.log("pause") }
    Shortcut { sequence: "L"; onActivated: console.log("forward playback") }
    Shortcut { sequence: "I"; onActivated: console.log("mark in") }
    Shortcut { sequence: "O"; onActivated: console.log("mark out") }
    Shortcut { sequence: "Left"; onActivated: console.log("previous frame") }
    Shortcut { sequence: "Right"; onActivated: console.log("next frame") }
}
