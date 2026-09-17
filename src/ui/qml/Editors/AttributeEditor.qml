import QtQuick

import Pointcaster 1.0

// Editor for a std::vector<AttributeConfiguration> member
Rectangle {
    id: root

    required property var configAdapter
    required property var workspace

    required property string path
    required property string configPath

    required property bool flattened

    required property int minLabelColumnWidth
    required property int fieldIndent
    required property int groupInnerPaddingY

    readonly property string parentKey: root.configPath ? root.configPath + "/" + root.path : ""

    readonly property var rows: (root.configAdapter && root.configAdapter.attributeList) ? root.configAdapter.attributeList : null
    readonly property int rowCount: root.rows ? root.rows.count : 0

    readonly property bool expanded: {
        if (root.flattened)
            return true;
        if (!root.parentKey)
            return true;
        const storedValue = root.workspace.foldedPropertyPaths[root.parentKey];
        return storedValue === undefined ? true : storedValue;
    }

    property int headerHeight: Math.round(26 * Scaling.uiScale)
    property int fieldHeight: Math.round(30 * Scaling.uiScale)

    readonly property int labelWidth: Math.max(root.minLabelColumnWidth, Workspace.labelColumnWidth)

    implicitHeight: attributeColumn.implicitHeight

    readonly property bool hasContent: root.rowCount > 0
    visible: root.hasContent

    color: root.flattened ? "transparent" : ThemeColors.dark
    border.width: (!root.flattened && root.expanded) ? Math.max(1, Math.round(1 * Scaling.uiScale)) : 0
    border.color: ThemeColors.almostdark

    Column {
        id: attributeColumn
        width: parent.width
        topPadding: root.flattened ? root.groupInnerPaddingY : 0

        Rectangle {
            id: attributeHeader
            width: parent.width
            height: root.headerHeight

            color: root.flattened ? "transparent" : (attributeHeaderMouse.containsMouse ? ThemeColors.middark : (root.expanded ? ThemeColors.almostdark : ThemeColors.dark))

            border.color: ThemeColors.almostdark
            border.width: (!root.flattened && !root.expanded) ? Math.max(1, Math.round(1 * Scaling.uiScale)) : 0

            MouseArea {
                id: attributeHeaderMouse
                anchors.fill: parent
                enabled: !root.flattened
                hoverEnabled: enabled
                onPressed: attributeHeaderMouse.forceActiveFocus()
                onClicked: {
                    if (root.parentKey)
                        root.workspace.setFoldedProperty(root.parentKey, !root.expanded);
                }
            }

            Rectangle {
                visible: root.flattened
                height: Math.max(1, Math.round(1 * Scaling.uiScale))
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                color: ThemeColors.almostdark
            }

            Row {
                anchors.fill: parent
                leftPadding: root.fieldIndent
                rightPadding: root.fieldIndent
                spacing: Math.round(5 * Scaling.uiScale)

                Image {
                    id: attributeHeaderArrow
                    visible: !root.flattened
                    width: visible ? Math.round(12 * Scaling.uiScale) : 0
                    anchors.verticalCenter: parent.verticalCenter
                    fillMode: Image.PreserveAspectFit
                    source: root.expanded ? FontAwesome.icon("solid/caret-down") : FontAwesome.icon("solid/caret-right")
                    opacity: 0.75
                }

                Text {
                    text: root.configAdapter ? root.configAdapter.parentConfigurationName(root.path) : ""
                    elide: Text.ElideRight
                    font: Scaling.uiFont
                    color: ThemeColors.text
                    width: parent.width - parent.leftPadding - parent.rightPadding - (attributeHeaderArrow.visible ? attributeHeaderArrow.width + parent.spacing : 0)
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        Repeater {
            model: root.rows

            delegate: Item {
                id: attributeRow

                required property int index
                required property string name
                required property string sourceType
                required property int target
                required property int precision
                required property real rangeMin
                required property real rangeMax

                readonly property bool targeted: attributeRow.target !== 0
                readonly property bool quantised: attributeRow.precision !== 0

                width: attributeColumn.width
                height: root.expanded ? root.fieldHeight * (attributeRow.targeted ? 3 : 1) : 0
                visible: root.expanded
                clip: true

                Column {
                    anchors.fill: parent

                    Item {
                        width: parent.width
                        height: root.fieldHeight

                        Text {
                            id: attributeLabel

                            text: attributeRow.sourceType ? `${attributeRow.name} (${attributeRow.sourceType})` : attributeRow.name
                            color: ThemeColors.text
                            font: Scaling.fieldLabelFont
                            elide: Text.ElideRight

                            x: root.fieldIndent
                            width: root.labelWidth
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        EnumSelector {
                            id: targetSelector

                            clip: true

                            font: Scaling.uiFont
                            options: [
                                {
                                    "text": "None",
                                    "value": 0
                                },
                                {
                                    "text": "Point Scale",
                                    "value": 1
                                }
                            ]
                            boundValue: attributeRow.target

                            anchors {
                                left: parent.left
                                leftMargin: root.fieldIndent + root.labelWidth + Math.round(6 * Scaling.uiScale)
                                right: parent.right
                                rightMargin: root.fieldIndent
                                verticalCenter: parent.verticalCenter
                            }

                            onCommitValue: function (value) {
                                root.rows.setTarget(attributeRow.index, value);
                            }
                        }
                    }

                    Item {
                        width: parent.width
                        height: root.fieldHeight
                        visible: attributeRow.targeted

                        Text {
                            text: "Precision"
                            color: ThemeColors.text
                            font: Scaling.fieldLabelFont
                            elide: Text.ElideRight

                            x: root.fieldIndent * 2
                            width: root.labelWidth - root.fieldIndent
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        EnumSelector {
                            id: precisionSelector

                            clip: true

                            font: Scaling.uiFont
                            options: [
                                {
                                    "text": "Full (float32)",
                                    "value": 0
                                },
                                {
                                    "text": "16-bit",
                                    "value": 1
                                },
                                {
                                    "text": "8-bit",
                                    "value": 2
                                }
                            ]
                            boundValue: attributeRow.precision

                            anchors {
                                left: parent.left
                                leftMargin: root.fieldIndent + root.labelWidth + Math.round(6 * Scaling.uiScale)
                                right: parent.right
                                rightMargin: root.fieldIndent
                                verticalCenter: parent.verticalCenter
                            }

                            onCommitValue: function (value) {
                                root.rows.setPrecision(attributeRow.index, value);
                            }
                        }
                    }

                    Item {
                        width: parent.width
                        height: root.fieldHeight
                        visible: attributeRow.targeted

                        Text {
                            text: "Range"
                            color: ThemeColors.text
                            font: Scaling.fieldLabelFont
                            elide: Text.ElideRight
                            opacity: attributeRow.quantised ? 1.0 : 0.5

                            x: root.fieldIndent * 2
                            width: root.labelWidth - root.fieldIndent
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Row {
                            id: rangeRow

                            readonly property real componentSpacing: Math.round(6 * Scaling.uiScale)
                            readonly property real eachWidth: Math.max(0, (width - componentSpacing) / 2)

                            spacing: rangeRow.componentSpacing
                            clip: true
                            opacity: attributeRow.quantised ? 1.0 : 0.4

                            anchors {
                                left: parent.left
                                leftMargin: root.fieldIndent + root.labelWidth + Math.round(6 * Scaling.uiScale)
                                right: parent.right
                                rightMargin: root.fieldIndent
                                verticalCenter: parent.verticalCenter
                            }

                            DragFloat {
                                font: Scaling.uiFont
                                enabled: attributeRow.quantised
                                boundValue: attributeRow.rangeMin
                                width: rangeRow.eachWidth

                                onCommitValue: function (value) {
                                    root.rows.setRange(attributeRow.index, value, attributeRow.rangeMax);
                                }
                            }

                            DragFloat {
                                font: Scaling.uiFont
                                enabled: attributeRow.quantised
                                boundValue: attributeRow.rangeMax
                                width: rangeRow.eachWidth

                                onCommitValue: function (value) {
                                    root.rows.setRange(attributeRow.index, attributeRow.rangeMin, value);
                                }
                            }
                        }
                    }
                }

                Item {
                    id: attributeDivider

                    readonly property int hitWidth: Math.round(9 * Scaling.uiScale)

                    x: root.fieldIndent + root.labelWidth - Math.round(hitWidth / 2)
                    width: attributeDivider.hitWidth
                    height: parent.height

                    Rectangle {
                        x: Math.round(parent.width / 2) - width
                        height: parent.height
                        width: (attributeDividerMouse.containsMouse || attributeDividerMouse.dragging) ? Math.round(3 * Scaling.uiScale) : Math.max(1, Math.round(1 * Scaling.uiScale))
                        color: attributeDividerMouse.dragging ? ThemeColors.mid : (attributeDividerMouse.containsMouse ? ThemeColors.middark : ThemeColors.almostdark)

                        Behavior on width {
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
                        id: attributeDividerMouse
                        anchors.fill: parent
                        cursorShape: Qt.SplitHCursor
                        hoverEnabled: true
                        property bool dragging: false

                        onPressed: {
                            attributeDividerMouse.dragging = true;
                            attributeDividerMouse.forceActiveFocus();
                        }
                        onReleased: attributeDividerMouse.dragging = false
                        onCanceled: attributeDividerMouse.dragging = false

                        onPositionChanged: {
                            if (!attributeDividerMouse.dragging)
                                return;
                            Workspace.labelColumnWidth = Math.round(attributeDivider.x + mouseX - root.fieldIndent);
                        }
                    }
                }
            }
        }
    }
}
