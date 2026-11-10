import QtQml.Models
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com.kdab.dockwidgets as KDDW

import Pointcaster 1.0

KDDW.DockWidget {
    id: root
    uniqueName: "devicesWindow"
    title: "Devices"

    property var workspace: null
    property int currentDeviceIndex: workspace ? workspace.selectedDeviceIndex : 0

    readonly property string selectedNodeKind: workspace ? workspace.selectedNodeKind : ""
    readonly property bool hasSelection: selectedNodeKind === "device" || selectedNodeKind === "group"

    signal nodeSelected

    function init() {
        if (workspace) {
            workspace.triggerDeviceDiscovery();
            deviceSelectionList.selectDevice(workspace.selectedDeviceIndex);
            currentDeviceIndex = workspace.selectedDeviceIndex;
        }
    }

    Component.onCompleted: init()

    Connections {
        target: workspace
        function onNewWorkspaceLoaded() {
            init();
        }
    }

    // when a new device is added, make sure its selected
    Connections {
        target: workspace
        function onDeviceAdded() {
            deviceSelectionList.selectDevice(workspace.deviceAdapters.length - 1);
            currentDeviceIndex = workspace.deviceAdapters.length - 1;
        }
    }

    // reset scroll position when switching devices
    onCurrentDeviceIndexChanged: {
        if (deviceConfigScrollView.contentItem)
            deviceConfigScrollView.contentItem.contentY = 0;
    }

    Item {
        anchors.fill: parent

        Rectangle {
            id: background
            anchors.fill: parent
            color: ThemeColors.base
        }

        ColumnLayout {
            id: innerContent

            property var kddockwidgets_min_size: Qt.size(Math.round(225 * Scaling.uiScale), Math.round(500 * Scaling.uiScale))

            anchors {
                fill: parent
                topMargin: Math.round(10 * Scaling.uiScale)
                bottomMargin: Math.round(10 * Scaling.uiScale)
                leftMargin: Math.round(8 * Scaling.uiScale)
                rightMargin: Math.round(8 * Scaling.uiScale)
            }
            spacing: Math.round(10 * Scaling.uiScale)

            DevicesToolBar {
                id: devicesToolBar
                workspace: root.workspace
                Layout.fillWidth: true
            }

            Item {
                id: deviceListContainer

                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(Workspace.deviceListHeight * Scaling.uiScale)

                DeviceSelectionList {
                    id: deviceSelectionList
                    workspace: root.workspace

                    anchors {
                        top: parent.top
                        left: parent.left
                        right: parent.right
                        bottom: resizeHandle.top
                    }

                    onActivated: function (index) {
                        root.currentDeviceIndex = index;
                        root.nodeSelected();
                    }
                }

                TapHandler {
                    onTapped: root.workspace.selectedOperatorAdapter = null
                }

                Rectangle {
                    id: resizeHandle

                    anchors {
                        left: parent.left
                        right: parent.right
                        bottom: parent.bottom
                    }

                    height: Math.round(5 * Scaling.uiScale)
                    color: "transparent"

                    Rectangle {
                        anchors.centerIn: parent

                        width: parent.width
                        height: (resizeMouseArea.containsMouse || resizeMouseArea.dragging) ? Math.round(3 * Scaling.uiScale) : Math.max(1, Math.round(1 * Scaling.uiScale))

                        color: resizeMouseArea.dragging ? ThemeColors.mid : (resizeMouseArea.containsMouse ? ThemeColors.middark : ThemeColors.almostdark)

                        Behavior on height {
                            NumberAnimation {
                                duration: Math.round(120 * Scaling.uiScale)
                                easing.type: Easing.InCubic
                            }
                        }

                        Behavior on color {
                            ColorAnimation {
                                duration: Math.round(90 * Scaling.uiScale)
                                easing.type: Easing.InCubic
                            }
                        }
                    }

                    MouseArea {
                        id: resizeMouseArea

                        anchors.fill: parent

                        cursorShape: Qt.SizeVerCursor
                        hoverEnabled: true

                        property bool dragging: false

                        onPressed: dragging = true
                        onReleased: dragging = false
                        onCanceled: dragging = false

                        onPositionChanged: {
                            if (!dragging)
                                return;

                            Workspace.deviceListHeight = Math.max((80 * Scaling.uiScale), Math.round(deviceListContainer.height + mouseY));
                        }
                    }
                }
            }

            DeviceControlRow {
                id: deviceControlRow
                visible: root.selectedNodeKind === "device"
                adapter: deviceSelectionList.selectedDevice
                Layout.fillWidth: true
            }

            Timeline {
                id: sequenceTimeline
                visible: root.selectedNodeKind === "device" && sequenceTimeline.seq !== null
                adapter: deviceSelectionList.selectedDevice
                Layout.fillWidth: true
            }

            PaddedScrollView {
                id: deviceConfigScrollView
                visible: root.selectedNodeKind === "device"

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: Math.round(140 * Scaling.uiScale)

                clip: true

                Column {
                    width: deviceConfigScrollView.availableWidth
                    anchors.topMargin: Math.round(8 * Scaling.uiScale)

                    Repeater {
                        model: root.workspace ? root.workspace.deviceAdapters : 0

                        Column {
                            id: deviceDelegate
                            required property var modelData
                            required property int index
                            width: parent.width
                            visible: index === root.currentDeviceIndex

                            ConfigurationEditor {
                                id: deviceConfigEditor
                                configAdapter: deviceDelegate.modelData
                                workspace: root.workspace
                                flattenFields: false
                                width: parent.width
                            }

                            spacing: deviceConfigEditor.groupSpacing

                            OperatorPipelineEditor {
                                workspace: root.workspace
                                operators: deviceDelegate.modelData && deviceDelegate.modelData.operatorAdapters ? deviceDelegate.modelData.operatorAdapters : []
                                pipelineAdapter: deviceDelegate.modelData && deviceDelegate.modelData.operator_pipelineAdapter ? deviceDelegate.modelData.operator_pipelineAdapter : []
                                hostPath: deviceDelegate.modelData ? String(deviceDelegate.modelData.configPath) : ""
                                width: parent.width

                                onAddOperatorRequested: operatorType => root.workspace.addOperatorToDevice(deviceDelegate.index, operatorType)
                                onRemoveOperatorRequested: operatorIndex => root.workspace.removeOperatorFromDevice(deviceDelegate.index, operatorIndex)
                            }
                        }
                    }
                }
            }

            Item {
                id: groupTimeline
                // the group's transport drives its descendant devices, so it
                // only means something when one of them holds a sequence
                visible: root.selectedNodeKind === "group" && groupTimeline.seq !== null && root.workspace.selectedGroupHasSequence
                Layout.fillWidth: true
                implicitHeight: visible ? groupTimelineLayout.implicitHeight : 0

                readonly property var seq: root.workspace && root.workspace.selectedDeviceGroupAdapter
                                           ? root.workspace.selectedDeviceGroupAdapter.sequenceAdapter
                                           : null
                readonly property bool playing: seq ? seq.playing : false
                readonly property int currentFrame: seq ? seq.current_frame : 0

                ColumnLayout {
                    id: groupTimelineLayout
                    anchors.fill: parent
                    spacing: Math.round(4 * Scaling.uiScale)

                    RowLayout {
                        spacing: Math.round(6 * Scaling.uiScale)
                        Layout.fillWidth: true

                        IconButton {
                            tooltip: "Stop"
                            iconSource: FontAwesome.icon("solid/stop")
                            iconSize: Math.round(11 * Scaling.uiScale)
                            onClicked: {
                                groupTimeline.seq.set("playing", false);
                                groupTimeline.seq.set("current_frame", 0);
                            }
                        }

                        IconButton {
                            tooltip: "Play"
                            iconSource: FontAwesome.icon("solid/play")
                            iconSize: Math.round(11 * Scaling.uiScale)
                            enabled: !groupTimeline.playing
                            opacity: enabled ? 1.0 : 0.4
                            onClicked: groupTimeline.seq.set("playing", true)
                        }

                        IconButton {
                            tooltip: "Pause"
                            iconSource: FontAwesome.icon("solid/pause")
                            iconSize: Math.round(11 * Scaling.uiScale)
                            enabled: groupTimeline.playing
                            opacity: enabled ? 1.0 : 0.4
                            onClicked: groupTimeline.seq.set("playing", false)
                        }

                        Item { Layout.fillWidth: true }

                        DragInt {
                            boundValue: groupTimeline.currentFrame
                            minValue: 0
                            implicitWidth: Math.round(64 * Scaling.uiScale)
                            onCommitValue: frame => groupTimeline.seq.set("current_frame", frame)
                        }
                    }
                }
            }

            PaddedScrollView {
                id: groupConfigScrollView
                visible: root.selectedNodeKind === "group"
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: Math.round(140 * Scaling.uiScale)
                clip: true

                Column {
                    width: groupConfigScrollView.availableWidth
                    anchors.topMargin: Math.round(8 * Scaling.uiScale)

                    ConfigurationEditor {
                        configAdapter: root.workspace ? root.workspace.selectedDeviceGroupAdapter : null
                        workspace: root.workspace
                        flattenFields: false
                        width: parent.width
                    }
                }
            }

            Item {
                id: emptySelectionSpacer
                visible: !root.hasSelection
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }
    }
}
