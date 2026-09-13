import QtQuick
import QtQuick.Dialogs
import QtQuick.Controls
import QtQuick.Layouts

import Pointcaster 1.0

Column {
    id: root

    required property var configAdapter
    required property var workspace

    property bool flattenFields: true

    property bool showTypeHeader: true

    readonly property int labelColumnWidth: Workspace.labelColumnWidth
    property int minLabelColumnWidth: Math.round(40 * Scaling.uiScale)
    property int minValueColumnWidth: Math.round(170 * Scaling.uiScale)

    property int groupSpacing: Math.round(8 * Scaling.uiScale)
    property int groupInnerPaddingY: Math.round(4 * Scaling.uiScale)

    property int fieldIndent: Math.round(8 * Scaling.uiScale)

    readonly property int boundsTagWidth: Math.round(26 * Scaling.uiScale)

    readonly property string configPath: configAdapter.configPath

    spacing: groupSpacing

    Repeater {
        model: configAdapter ? configAdapter.childPathGroups : []
        delegate: configurationNode
    }

    Component {
        id: configurationNode

        Rectangle {
            id: nodeRoot

            readonly property string defaultPath: modelData[0]

            readonly property bool nested: defaultPath.includes("/")
            readonly property string parentConfigName: root.configAdapter.parentConfigurationName(defaultPath)
            readonly property string parentKey: root.configPath && parentConfigName ? root.configPath + "/" + parentConfigName : ""

            readonly property bool flattened: root.flattenFields
            readonly property bool fieldsVisible: flattened || expanded

            property string headerText: parentConfigName
            property int headerHeight: Math.round(26 * Scaling.uiScale)
            property int fieldHeight: Math.round(30 * Scaling.uiScale)

            // how the group sits before anyone has folded it, from @folded
            readonly property bool defaultExpanded: root.configAdapter ? !root.configAdapter.isFoldedByDefault(defaultPath) : true

            readonly property bool expanded: {
                if (flattened)
                    return true;
                if (!parentKey)
                    return defaultExpanded;
                const storedValue = root.workspace.foldedPropertyPaths[parentKey];
                return storedValue === undefined ? defaultExpanded : storedValue;
            }

            width: root.width
            implicitHeight: contentColumn.implicitHeight

            property var visiblePaths: nodeRoot.visibleFieldPaths()

            function visibleFieldPaths() {
                if (!root.configAdapter)
                    return [];
                return modelData.filter(function (path) {
                    const hidden = root.configAdapter.isHidden(path);
                    // for now just hide variant type selection
                    const variant = root.configAdapter.isVariant(path);
                    return !hidden && !variant;
                });
            }

            Connections {
                target: root.configAdapter
                function onStructureChanged() {
                    nodeRoot.visiblePaths = nodeRoot.visibleFieldPaths();
                }
            }

            visible: fieldRepeater.count > 0

            color: flattened ? "transparent" : ThemeColors.dark
            border.width: (!flattened && expanded) ? Math.max(1, Math.round(1 * Scaling.uiScale)) : 0
            border.color: ThemeColors.almostdark

            Column {
                id: contentColumn
                width: parent.width
                topPadding: nodeRoot.flattened ? root.groupInnerPaddingY : 0

                // Header
                Rectangle {
                    id: nodeHeader
                    width: parent.width
                    height: nodeRoot.headerHeight
                    visible: fieldRepeater.count > 0 && (root.showTypeHeader || nodeRoot.nested)

                    color: nodeRoot.flattened ? "transparent" : (headerMouseArea.containsMouse ? ThemeColors.middark : (nodeRoot.expanded ? ThemeColors.almostdark : ThemeColors.dark))

                    border.color: ThemeColors.almostdark
                    border.width: (!nodeRoot.flattened && !nodeRoot.expanded) ? Math.max(1, Math.round(1 * Scaling.uiScale)) : 0

                    MouseArea {
                        id: headerMouseArea
                        anchors.fill: parent
                        enabled: !nodeRoot.flattened
                        hoverEnabled: enabled
                        onPressed: headerMouseArea.forceActiveFocus()
                        onClicked: {
                            if (nodeRoot.parentKey)
                                root.workspace.setFoldedProperty(nodeRoot.parentKey, !nodeRoot.expanded);
                        }
                    }

                    Rectangle {
                        visible: nodeRoot.flattened
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
                            id: headerArrowIcon
                            visible: !nodeRoot.flattened
                            width: visible ? Math.round(12 * Scaling.uiScale) : 0
                            anchors.verticalCenter: parent.verticalCenter
                            fillMode: Image.PreserveAspectFit
                            source: nodeRoot.expanded ? FontAwesome.icon("solid/caret-down") : FontAwesome.icon("solid/caret-right")
                            opacity: 0.75
                        }

                        Text {
                            text: nodeRoot.headerText
                            elide: Text.ElideRight
                            font: Scaling.uiFont
                            color: ThemeColors.text
                            width: parent.width - parent.leftPadding - parent.rightPadding - (headerArrowIcon.visible ? headerArrowIcon.width + parent.spacing : 0)
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }

                // Fields
                Repeater {
                    id: fieldRepeater

                    model: nodeRoot.visiblePaths

                    delegate: Item {
                        id: fieldRow

                        readonly property bool readOnly: root.configAdapter ? root.configAdapter.isDisabled(modelData) : false
                        readonly property bool isOutput: root.configAdapter ? root.configAdapter.isOutput(modelData) : false
                        readonly property bool isStream: root.configAdapter ? root.configAdapter.isStream(modelData) : false
                        readonly property string typeName: root.configAdapter ? root.configAdapter.typeName(modelData).toLowerCase() : ""
                        readonly property bool isBounds: fieldRow.typeName.includes("position_bounds")
                        readonly property bool isRadius: fieldRow.typeName.includes("radius")

                        // a switched value carries its own on/off beside it,
                        // and nothing draws it in the scene while it is off
                        readonly property bool isToggleable: fieldRow.typeName.includes("toggleable")

                        property bool switchedOn: fieldRow.switchedOnOrDefault()
                        function switchedOnOrDefault() {
                            if (!fieldRow.isToggleable)
                                return true;
                            const held = root.configAdapter ? root.configAdapter.value(modelData) : null;
                            return !!(held && held.active);
                        }

                        Connections {
                            target: root.configAdapter
                            function onFieldChanged(changedPath) {
                                if (String(changedPath) === modelData)
                                    fieldRow.switchedOn = fieldRow.switchedOnOrDefault();
                            }
                        }

                        readonly property real textOpacity: readOnly ? 0.66 : 1.0

                        readonly property int rowSpan: Math.max(1, Math.ceil((valueColumn.implicitHeight - 1) / nodeRoot.fieldHeight))

                        visible: nodeRoot.fieldsVisible
                        height: visible ? nodeRoot.fieldHeight * fieldRow.rowSpan : 0
                        width: contentColumn.width

                        RowLayout {
                            anchors.fill: parent
                            spacing: 0

                            Text {
                                id: label
                                text: StringUtils.titleFromSnake(StringUtils.leafName(modelData))
                                color: ThemeColors.text
                                font: Scaling.fieldLabelFont
                                opacity: fieldRow.textOpacity

                                rightPadding: fieldRow.isBounds ? root.boundsTagWidth : 0

                                Layout.preferredWidth: root.labelColumnWidth
                                Layout.minimumWidth: root.minLabelColumnWidth
                                clip: true

                                Layout.leftMargin: root.fieldIndent

                                Layout.alignment: fieldRow.isToggleable ? Qt.AlignTop : Qt.AlignVCenter
                                Layout.topMargin: fieldRow.isToggleable ? Math.round((nodeRoot.fieldHeight - implicitHeight) / 2) : 0

                                InfoToolTip {
                                    visible: labelHover.hovered
                                    textValue: root.configPath ? `${root.configPath}/${modelData}` : modelData
                                }
                                HoverHandler {
                                    id: labelHover
                                }
                            }

                            Item {
                                Layout.fillWidth: true
                                Layout.minimumWidth: root.minValueColumnWidth
                                Layout.fillHeight: true

                                Rectangle {
                                    id: outputDot

                                    readonly property int inset: Math.round(6 * Scaling.uiScale)

                                    visible: fieldRow.isOutput
                                    width: Math.round(6 * Scaling.uiScale)
                                    height: width
                                    radius: width / 2
                                    color: ThemeColors.yellow

                                    anchors {
                                        left: parent.left
                                        leftMargin: inset
                                        top: parent.top
                                        topMargin: Math.round((nodeRoot.fieldHeight - height) / 2)
                                    }
                                }

                                ListToggleButton {
                                    id: renderToggle

                                    visible: fieldRow.isStream || fieldRow.isBounds || fieldRow.isRadius
                                    width: Math.round(iconSize * 0.9)
                                    height: Math.round(iconSize * 0.9)

                                    iconOn: FontAwesome.icon("solid/eye")
                                    iconOff: FontAwesome.icon("solid/eye-slash")
                                    checked: fieldRow.rendered
                                    gated: !fieldRow.switchedOn
                                    tip: fieldRow.switchedOn ? qsTr("Render in the session view") : qsTr("Switch the field on to render it")

                                    anchors {
                                        right: parent.right
                                        rightMargin: Math.round(8 * Scaling.uiScale)
                                        top: parent.top
                                        topMargin: Math.round((nodeRoot.fieldHeight - height) / 2)
                                    }

                                    onToggled: {
                                        if (checked) {
                                            root.workspace.addRenderPath(fieldRow.fieldPath);
                                        } else {
                                            root.workspace.removeRenderPath(fieldRow.fieldPath);
                                        }
                                    }
                                }

                                // a switched field reads as one row until it
                                // is turned on, and unfolds the rows carrying
                                // its actual value underneath once it is
                                Column {
                                    id: valueColumn

                                    anchors {
                                        left: parent.left
                                        right: parent.right
                                        top: parent.top
                                        leftMargin: outputDot.visible ? outputDot.inset * 2 + outputDot.width : outputDot.inset
                                        rightMargin: renderToggle.visible ? outputDot.inset * 2 + renderToggle.width : 0
                                    }

                                    Item {
                                        id: switchStrip

                                        visible: fieldRow.isToggleable
                                        width: parent.width
                                        height: visible ? nodeRoot.fieldHeight : 0

                                        CheckBox {
                                            id: fieldSwitch

                                            enabled: !fieldRow.readOnly
                                            checked: fieldRow.switchedOn

                                            anchors {
                                                left: parent.left
                                                verticalCenter: parent.verticalCenter
                                            }

                                            onToggled: root.configAdapter.set(modelData, {
                                                active: checked
                                            })
                                        }

                                        // the same caret a folded group wears,
                                        // saying there is something under this
                                        // row the checkbox is holding shut
                                        Image {
                                            anchors {
                                                left: fieldSwitch.right
                                                verticalCenter: parent.verticalCenter
                                            }
                                            width: Math.round(10 * Scaling.uiScale)
                                            height: width
                                            fillMode: Image.PreserveAspectFit
                                            source: fieldRow.switchedOn ? FontAwesome.icon("solid/caret-down") : FontAwesome.icon("solid/caret-right")
                                            opacity: fieldRow.switchedOn ? 0.75 : 0.4
                                        }
                                    }

                                    Loader {
                                        id: valueContainer

                                        property string path: modelData
                                        readonly property string typeName: fieldRow.typeName

                                        // an unswitched field keeps its value,
                                        // it just stops showing it
                                        visible: fieldRow.switchedOn
                                        width: parent.width
                                        height: visible ? Math.max(nodeRoot.fieldHeight, implicitHeight) : 0
                                        opacity: fieldRow.textOpacity

                                        asynchronous: false

                                        sourceComponent: {
                                            if (root.configAdapter.isStream(modelData))
                                                return streamEditor;
                                            if (root.configAdapter.isEnum(modelData))
                                                return enumEditor;
                                            if (typeName === "int" || typeName === "int32" || typeName === "int32_t" || typeName === "integer" || typeName === "toggleable<int>")
                                                return intEditor;
                                            if (typeName === "float" || typeName === "float32" || typeName === "float32_t" || typeName === "double" || typeName === "real" || typeName === "number")
                                                return floatEditor;
                                            if (typeName.includes("length"))
                                                return floatEditor;
                                            if (typeName.includes("radius"))
                                                return radiusEditor;
                                            if (typeName.includes("pc::float3") || typeName.includes("float3"))
                                                return float3Editor;
                                            if (typeName.includes("position_bounds"))
                                                return positionBoundsEditor;
                                            if (typeName.includes("position"))
                                                return float3Editor;
                                            if (typeName === "bool")
                                                return boolEditor;
                                            return stringEditor;
                                        }
                                    }
                                }

                                // frames the editor for published fields
                                Rectangle {
                                    anchors.fill: parent
                                    visible: published
                                    color: ThemeColors.withAlpha(stateColor, 0.07)
                                    border.width: Math.max(1, Math.round(1 * Scaling.uiScale))
                                    border.color: ThemeColors.withAlpha(stateColor, 0.55)
                                }
                            }
                        }

                        Column {
                            visible: fieldRow.isBounds && fieldRow.switchedOn
                            x: label.x + label.width - width
                            y: nodeRoot.fieldHeight - Math.round(15 * Scaling.uiScale) + (fieldRow.isToggleable ? nodeRoot.fieldHeight : 0)
                            width: root.boundsTagWidth

                            Repeater {
                                model: [qsTr("min"), qsTr("max")]

                                Item {
                                    width: root.boundsTagWidth
                                    height: nodeRoot.fieldHeight

                                    Rectangle {
                                        anchors.right: parent.right
                                        width: parent.width
                                        height: Math.round(15 * Scaling.uiScale)
                                        radius: Math.round(3 * Scaling.uiScale)
                                        color: ThemeColors.withAlpha(ThemeColors.almostdark, 0.6)
                                        opacity: fieldRow.textOpacity

                                        Text {
                                            anchors.centerIn: parent
                                            text: modelData
                                            font: Scaling.fieldTagFont
                                            color: ThemeColors.readOnlyText
                                        }
                                    }
                                }
                            }
                        }

                        // grab handle sits over the seam between the two cells
                        Item {
                            id: dividerHandle

                            readonly property int hitWidth: Math.round(9 * Scaling.uiScale)

                            x: label.x + label.width - Math.round(hitWidth / 2)
                            width: hitWidth
                            height: parent.height

                            // the line sits entirely on the label side of the
                            // seam, so it never covers the value frame's border
                            // and thickens leftwards on hover
                            Rectangle {
                                x: Math.round(parent.width / 2) - width
                                height: parent.height
                                width: (dividerMouseArea.containsMouse || dividerMouseArea.dragging) ? Math.round(3 * Scaling.uiScale) : Math.max(1, Math.round(1 * Scaling.uiScale))
                                color: dividerMouseArea.dragging ? ThemeColors.mid : (dividerMouseArea.containsMouse ? ThemeColors.middark : ThemeColors.almostdark)

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
                                id: dividerMouseArea
                                anchors.fill: parent
                                cursorShape: Qt.SplitHCursor
                                hoverEnabled: true
                                property bool dragging: false

                                onPressed: {
                                    dragging = true;
                                    dividerMouseArea.forceActiveFocus();
                                }
                                onReleased: dragging = false
                                onCanceled: dragging = false

                                onPositionChanged: {
                                    if (!dragging)
                                        return;
                                    Workspace.labelColumnWidth = Math.round(dividerHandle.x + mouseX - label.x);
                                }
                            }
                        }

                        // context menu properties outside the Menu...
                        // to not break ContextMenu lazy loading
                        readonly property string fieldPath: root.configPath ? `${root.configPath}/${modelData}` : ""
                        readonly property bool published: fieldPath.length > 0 && !!root.workspace && root.workspace.publishPaths.includes(fieldPath)
                        readonly property bool pushed: published && root.workspace.pushPaths.includes(fieldPath)
                        readonly property bool rendered: fieldPath.length > 0 && !!root.workspace && root.workspace.renderPaths.includes(fieldPath)
                        readonly property color stateColor: pushed ? ThemeColors.green : ThemeColors.blue

                        ContextMenu.menu: Menu {

                            onOpened: {
                                itemAt(0).checked = published;
                                itemAt(1).checked = pushed;
                            }

                            MenuItem {
                                text: qsTr("Publish")
                                checkable: true
                                enabled: fieldPath.length > 0
                                onTriggered: {
                                    if (checked) {
                                        root.workspace.addPublishPath(fieldPath);
                                    } else {
                                        root.workspace.removePublishPath(fieldPath);
                                    }
                                }
                            }
                            MenuItem {
                                text: qsTr("Push updates")
                                checkable: true
                                enabled: published
                                opacity: enabled ? 1 : 0.5
                                onTriggered: {
                                    if (checked) {
                                        root.workspace.addPushPath(fieldPath);
                                    } else {
                                        root.workspace.removePushPath(fieldPath);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    //
    // FIELD EDITOR COMPONENTS
    //

    Component {
        id: stringEditor

        RowLayout {
            width: parent.width
            spacing: Math.round(Scaling.uiScale * 5)

            TextField {
                id: valueField
                font: Scaling.uiFont

                Layout.fillWidth: true
                clip: true

                background: Rectangle {
                    color: "transparent"
                    border.color: valueField.activeFocus ? ThemeColors.highlight : "transparent"
                    border.width: Math.max(1, Math.round(1 * Scaling.uiScale))
                    radius: 0
                }

                text: root.configAdapter ? String(root.configAdapter.value(path)) : ""
                readOnly: root.configAdapter ? (root.configAdapter.isDisabled(path) || root.configAdapter.isFileOpener(path)) : false

                // hide the live TextInput text while not editing, the Text overlay below renders instead
                color: activeFocus ? (readOnly ? ThemeColors.readOnlyText : ThemeColors.text) : "transparent"

                // select everything on focus-in so typing replaces the contents by default
                onActiveFocusChanged: {
                    if (activeFocus)
                        Qt.callLater(function () {
                            valueField.selectAll();
                        });
                }

                onEditingFinished: {
                    if (!root.configAdapter)
                        return;
                    root.configAdapter.set(path, text);
                }

                // Enter commits and ends the edit,
                // Escape reverts to the stored value and ends the edit
                onAccepted: valueField.focus = false
                Keys.onEscapePressed: {
                    text = root.configAdapter ? String(root.configAdapter.value(path)) : "";
                    focus = false;
                }

                Connections {
                    target: root.configAdapter
                    function onFieldChanged(changedPath) {
                        if (String(changedPath) !== path)
                            return;
                        valueField.text = root.configAdapter ? String(root.configAdapter.value(path)) : "";
                    }
                }

                // elided display when not editing the string
                Text {
                    anchors.fill: parent
                    anchors.leftMargin: valueField.leftPadding
                    anchors.rightMargin: valueField.rightPadding
                    verticalAlignment: Text.AlignVCenter
                    visible: !valueField.activeFocus
                    text: valueField.text
                    font: valueField.font
                    color: valueField.readOnly ? ThemeColors.readOnlyText : ThemeColors.text
                    elide: Text.ElideLeft
                }

                InfoToolTip {
                    textValue: valueField.text
                }
            }

            IconButton {
                id: fileOpenButton
                visible: root.configAdapter ? root.configAdapter.isFileOpener(path) : false

                tooltip: "Open file..."

                iconSource: FontAwesome.icon("solid/file-import")
                iconSize: Math.round(12 * Scaling.uiScale)
                topPadding: Math.round(4 * Scaling.uiScale)
                bottomPadding: Math.round(4 * Scaling.uiScale)
                leftPadding: Math.round(5 * Scaling.uiScale)
                rightPadding: Math.round(5 * Scaling.uiScale)

                onClicked: fileOpenDialog.open()
            }

            FileDialog {
                id: fileOpenDialog
                nameFilters: [qsTr("PLY files (*.ply)"), qsTr("All files (*)")]
                onAccepted: root.configAdapter.set(path, selectedFile)
            }

            IconButton {
                id: folderOpenButton
                visible: root.configAdapter ? root.configAdapter.isFolderOpener(path) : false

                tooltip: "Open folder..."

                iconSource: FontAwesome.icon("solid/folder-open")
                iconSize: Math.round(12 * Scaling.uiScale)
                topPadding: Math.round(4 * Scaling.uiScale)
                bottomPadding: Math.round(4 * Scaling.uiScale)
                leftPadding: Math.round(5 * Scaling.uiScale)
                rightPadding: Math.round(5 * Scaling.uiScale)

                onClicked: folderOpenDialog.open()
            }

            FolderDialog {
                id: folderOpenDialog
                onAccepted: root.configAdapter.set(path, selectedFolder)
            }

            IconButton {
                id: reloadButton
                visible: root.configAdapter ? root.configAdapter.isFileOpener(path) || root.configAdapter.isFolderOpener(path) : false

                tooltip: "Reload"

                iconSource: FontAwesome.icon("solid/arrows-rotate")
                iconSize: Math.round(12 * Scaling.uiScale)
                topPadding: Math.round(4 * Scaling.uiScale)
                bottomPadding: Math.round(4 * Scaling.uiScale)
                leftPadding: Math.round(5 * Scaling.uiScale)
                rightPadding: Math.round(5 * Scaling.uiScale)

                // onClicked: fileOpenDialog.open()

            }
        }
    }

    // For the writeable output streams attached to a config
    Component {
        id: streamEditor

        RowLayout {
            id: streamRow

            anchors.fill: parent
            spacing: Math.round(5 * Scaling.uiScale)

            property int elementCount: root.configAdapter ? root.configAdapter.value(path) : -1

            Text {
                text: streamRow.elementCount
                font: Scaling.uiFont
                color: ThemeColors.text

                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
            }

            Text {
                text: root.configAdapter ? root.configAdapter.streamType(path) : ""
                font: Scaling.uiFont
                color: ThemeColors.readOnlyText
                elide: Text.ElideRight

                Layout.alignment: Qt.AlignVCenter
                Layout.rightMargin: Math.round(6 * Scaling.uiScale)
            }

            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    streamRow.elementCount = Number(root.configAdapter.value(path));
                }
            }
        }
    }

    Component {
        id: intEditor
        DragInt {
            id: intEditorControl
            font: Scaling.uiFont

            minValue: root.configAdapter ? root.configAdapter.minMax(path)[0] : undefined
            maxValue: root.configAdapter ? root.configAdapter.minMax(path)[1] : undefined
            defaultValue: root.configAdapter ? intEditorControl.numberOf(root.configAdapter.defaultValue(path)) : undefined

            enabled: root.configAdapter ? !root.configAdapter.isDisabled(path) : true

            // a Toggleable arrives as a map carrying the switch alongside the
            // number, a plain int arrives as itself
            function numberOf(held) {
                if (held === undefined || held === null)
                    return 0;
                const n = Number(held.value !== undefined ? held.value : held);
                return isNaN(n) ? 0 : Math.trunc(n);
            }

            boundValue: root.configAdapter ? intEditorControl.numberOf(root.configAdapter.value(path)) : 0

            onPreviewValue: function (value) {
                root.configAdapter.setPreview(path, value);
            }

            onCommitValue: function (value) {
                root.configAdapter.set(path, value);
            }

            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    var n = Number(root.configAdapter.value(path));
                    intEditorControl.boundValue = isNaN(n) ? 0 : Math.trunc(n);
                }
            }
        }
    }

    Component {
        id: floatEditor
        DragFloat {
            id: floatEditorControl
            font: Scaling.uiFont

            minValue: root.configAdapter ? root.configAdapter.minMax(path)[0] : undefined
            maxValue: root.configAdapter ? root.configAdapter.minMax(path)[1] : undefined
            defaultValue: root.configAdapter ? root.configAdapter.defaultValue(path) : undefined

            enabled: root.configAdapter ? !root.configAdapter.isDisabled(path) : true

            boundValue: {
                var n = root.configAdapter ? Number(root.configAdapter.value(path)) : 0.0;
                return isNaN(n) ? 0.0 : n;
            }

            onPreviewValue: function (value) {
                root.configAdapter.setPreview(path, value);
            }

            onCommitValue: function (value) {
                root.configAdapter.set(path, value);
            }

            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    var n = Number(root.configAdapter.value(path));
                    floatEditorControl.boundValue = isNaN(n) ? 0.0 : n;
                }
            }
        }
    }

    Component {
        id: radiusEditor
        DragFloat {
            id: radiusControl
            font: Scaling.uiFont

            minValue: root.configAdapter ? root.configAdapter.minMax(path)[0] : undefined
            maxValue: root.configAdapter ? root.configAdapter.minMax(path)[1] : undefined
            defaultValue: root.configAdapter ? radiusControl.numberOf(root.configAdapter.defaultValue(path)) : undefined

            enabled: root.configAdapter ? !root.configAdapter.isDisabled(path) : true

            function numberOf(held) {
                if (held === undefined || held === null)
                    return 0.0;
                const n = Number(held.value !== undefined ? held.value : held);
                return isNaN(n) ? 0.0 : n;
            }

            boundValue: root.configAdapter ? radiusControl.numberOf(root.configAdapter.value(path)) : 0.0

            onPreviewValue: function (value) {
                root.configAdapter.setPreview(path, value);
            }

            onCommitValue: function (value) {
                root.configAdapter.set(path, value);
            }

            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    radiusControl.boundValue = radiusControl.numberOf(root.configAdapter.value(path));
                }
            }
        }
    }

    Component {
        id: float3Editor
        DragFloat3 {
            id: float3
            font: Scaling.uiFont
            axisFont: Scaling.uiSmallFont

            minValue: root.configAdapter ? root.configAdapter.minMax(path)[0] : undefined
            maxValue: root.configAdapter ? root.configAdapter.minMax(path)[1] : undefined
            defaultValue: root.configAdapter ? root.configAdapter.defaultValue(path) : undefined

            enabled: root.configAdapter ? !root.configAdapter.isDisabled(path) : true

            boundValue: {
                if (!root.configAdapter)
                    return Qt.vector3d(0, 0, 0);
                const value = root.configAdapter.value(path);
                return Qt.vector3d(Number(value.x) || 0, Number(value.y) || 0, Number(value.z) || 0);
            }

            onPreviewValue: function (vector) {
                if (!root.configAdapter)
                    return;
                root.configAdapter.setPreview(path, vector);
            }

            onCommitValue: function (vector) {
                if (!root.configAdapter)
                    return;
                root.configAdapter.set(path, vector);
            }

            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    const value = root.configAdapter.value(path);
                    float3.boundValue = Qt.vector3d(Number(value.x) || 0, Number(value.y) || 0, Number(value.z) || 0);
                }
            }
        }
    }

    Component {
        id: positionBoundsEditor
        PositionBoundsEditor {
            id: positionBounds
            font: Scaling.uiFont
            axisFont: Scaling.uiSmallFont

            defaultValue: root.configAdapter ? root.configAdapter.defaultValue(path) : undefined

            enabled: root.configAdapter ? !root.configAdapter.isDisabled(path) : true

            boundValue: root.configAdapter ? root.configAdapter.value(path) : undefined

            onPreviewValue: function (b) {
                if (!root.configAdapter)
                    return;
                root.configAdapter.setPreview(path, b);
            }

            onCommitValue: function (b) {
                if (!root.configAdapter)
                    return;
                root.configAdapter.set(path, b);
            }

            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    positionBounds.boundValue = root.configAdapter.value(path);
                }
            }
        }
    }

    Component {
        id: enumEditor
        EnumSelector {
            id: enumSelector
            font: Scaling.uiFont

            enabled: root.configAdapter ? !root.configAdapter.isDisabled(path) : true

            options: root.configAdapter ? root.configAdapter.enumOptions(path) : undefined
            boundValue: {
                var n = root.configAdapter ? Number(root.configAdapter.value(path)) : 0;
                return isNaN(n) ? 0 : Math.trunc(n);
            }

            onCommitValue: function (value) {
                root.configAdapter.set(path, value);
            }

            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    var n = Number(root.configAdapter.value(path));
                    enumSelector.boundValue = isNaN(n) ? 0 : Math.trunc(n);
                }
            }
        }
    }

    Component {
        id: boolEditor

        CheckBox {
            id: boolCheckBox
            enabled: root.configAdapter ? !root.configAdapter.isDisabled(path) : true
            checked: root.configAdapter ? !!root.configAdapter.value(path) : false
            onCheckedChanged: function () {
                root.configAdapter.set(path, checked);
                // if its a button, we only want it to be momentarily checked
                if (root.configAdapter.isButton(path)) {
                    boolCheckBox.checked = false;
                }
            }
            Connections {
                target: root.configAdapter
                function onFieldChanged(changedPath) {
                    if (String(changedPath) !== path)
                        return;
                    boolCheckBox.checked = !!root.configAdapter.value(path);
                }
            }
        }
    }
}
