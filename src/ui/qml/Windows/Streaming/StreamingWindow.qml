import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import com.kdab.dockwidgets as KDDW

import Pointcaster 1.0

KDDW.DockWidget {
    id: root
    uniqueName: "streamingWindow"
    title: "Streaming"

    property var workspace: null

    readonly property int selectedChannelIndex: workspace ? workspace.selectedStreamChannelIndex : -1
    readonly property string selectedChannelAddress: (workspace && selectedChannelIndex >= 0) ? workspace.streamChannels.channelAddress(selectedChannelIndex) : ""

    Item {
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            color: ThemeColors.base
        }

        ColumnLayout {
            id: innerContent

            property var kddockwidgets_min_size: Qt.size(Math.round(225 * Scaling.uiScale), Math.round(300 * Scaling.uiScale))

            anchors {
                fill: parent
                topMargin: Math.round(10 * Scaling.uiScale)
                bottomMargin: Math.round(10 * Scaling.uiScale)
                leftMargin: Math.round(8 * Scaling.uiScale)
                rightMargin: Math.round(8 * Scaling.uiScale)
            }
            spacing: Math.round(10 * Scaling.uiScale)

            // broadcast channels, each toggleable on/off
            Item {
                id: streamChannelListContainer

                Layout.fillWidth: true
                Layout.preferredHeight: Math.round(Workspace.streamChannelListHeight * Scaling.uiScale)

                StreamChannelList {
                    id: streamChannelList
                    workspace: root.workspace

                    anchors {
                        top: parent.top
                        left: parent.left
                        right: parent.right
                        bottom: streamChannelListResizeHandle.top
                    }
                }

                Rectangle {
                    id: streamChannelListResizeHandle

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
                        height: (streamChannelListResizeMouseArea.containsMouse || streamChannelListResizeMouseArea.dragging) ? Math.round(3 * Scaling.uiScale) : Math.max(1, Math.round(1 * Scaling.uiScale))

                        color: streamChannelListResizeMouseArea.dragging ? ThemeColors.mid : (streamChannelListResizeMouseArea.containsMouse ? ThemeColors.middark : ThemeColors.almostdark)

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
                        id: streamChannelListResizeMouseArea

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

                            Workspace.streamChannelListHeight = Math.max((80 * Scaling.uiScale), Math.round(streamChannelListContainer.height + mouseY));
                        }
                    }
                }
            }

            // the point streamer's own configuration, then settings and
            // connected clients for the selected channel
            PaddedScrollView {
                id: streamerDetailsScrollView
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: Math.round(50 * Scaling.uiScale)
                clip: true

                Column {
                    width: streamerDetailsScrollView.availableWidth
                    anchors.topMargin: Math.round(8 * Scaling.uiScale)
                    spacing: streamerConfigEditor.groupSpacing

                    ConfigurationEditor {
                        id: streamerConfigEditor
                        width: parent.width
                        visible: !!(root.workspace && root.workspace.pointStreamerAdapter)
                        configAdapter: root.workspace ? root.workspace.pointStreamerAdapter : null
                        workspace: root.workspace
                        flattenFields: false
                    }

                    Text {
                        width: parent.width
                        topPadding: Math.round(8 * Scaling.uiScale)
                        text: root.selectedChannelIndex >= 0 ? ("settings and connected clients for \"" + root.selectedChannelAddress + "\" todo") : "select a channel to view its settings"
                        color: ThemeColors.placeholderText
                        font: Scaling.uiFont
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }
    }
}
