import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Pointcaster 1.0

Item {
    id: root

    property var workspace: null
    property var currentItem: list.currentItem
    property var selectedDevice: null

    property int indentStep: Math.round(14 * Scaling.uiScale)

    readonly property real deviceRowHeight: Math.max(Math.round(30 * Scaling.uiScale), Math.ceil(Scaling.pointSize * 2.1))
    readonly property real groupRowHeight: root.deviceRowHeight

    // shared sizing for the tree, rails align disclosure controls into columns
    readonly property color treeRailColor: ThemeColors.mid
    readonly property real railThickness: Math.max(1, Math.round(1 * Scaling.uiScale))
    readonly property real rowEdgeMargin: Math.round(8 * Scaling.uiScale)
    readonly property real rowItemSpacing: Math.round(8 * Scaling.uiScale)
    readonly property real chevronSize: Math.round(11 * Scaling.uiScale)

    // drag state, shared across delegates
    property int dragOverIndex: -1
    property string dragOverZone: "" // "before" | "after" | "into"
    property bool dropToRoot: false

    width: parent.width

    signal activated(int index)

    function selectDevice(deviceIndex) {
        if (deviceIndex < 0)
            return;
        const rows = workspace ? workspace.deviceTreeRows : [];
        for (let i = 0; i < rows.length; ++i) {
            if (rows[i].kind === "device" && rows[i].deviceIndex === deviceIndex) {
                setSelectedIndex(i, deviceIndex);
                return;
            }
        }
    }

    function setSelectedIndex(rowIndex, deviceIndex) {
        list.currentIndex = rowIndex;
        if (deviceIndex < 0)
            return;
        workspace.selectedDeviceIndex = deviceIndex;
        selectedDevice = workspace.deviceAdapters[deviceIndex];
        activated(deviceIndex);
    }

    // id of the next row sharing the same parent and kind, the insert-before
    // anchor for reordering within a parent
    function nextSiblingId(rowIndex, parentId) {
        const rows = workspace ? workspace.deviceTreeRows : [];
        for (let i = rowIndex + 1; i < rows.length; ++i) {
            if (rows[i].parentId === parentId)
                return String(rows[i].id);
        }
        return "";
    }

    // drop zone from the cursor's vertical position within a row
    function zoneFor(rowKind, localY, h) {
        if (rowKind === "group") {
            if (localY < h * 0.28)
                return "before";
            if (localY > h * 0.72)
                return "after";
            return "into";
        }
        return localY < h * 0.5 ? "before" : "after";
    }

    // resolves a scene point to a row index and zone via the list geometry
    function rowAndZoneAt(sceneX, sceneY) {
        if (!workspace)
            return {
                index: -1,
                inList: false,
                zone: "",
                row: null
            };
        const vp = list.mapFromItem(null, sceneX, sceneY);
        const inList = vp.x >= 0 && vp.x <= list.width && vp.y >= 0 && vp.y <= list.height;
        if (!inList)
            return {
                index: -1,
                inList: false,
                zone: "",
                row: null
            };
        const cy = list.contentY + vp.y;
        const idx = list.indexAt(list.width / 2, cy);
        if (idx < 0)
            return {
                index: -1,
                inList: true,
                zone: "",
                row: null
            };
        const rows = workspace.deviceTreeRows;
        const row = rows[idx];
        const item = list.itemAtIndex(idx);
        const localY = item ? (cy - item.y) : 0;
        const h = item ? item.height : 1;
        return {
            index: idx,
            inList: true,
            zone: zoneFor(row.kind, localY, h),
            row: row
        };
    }

    // called continuously while a row is being dragged
    function updateDragTarget(sceneX, sceneY, sourceId) {
        const info = rowAndZoneAt(sceneX, sceneY);
        if (info.index >= 0 && !(info.row && String(info.row.id) === sourceId)) {
            dragOverIndex = info.index;
            dragOverZone = info.zone;
            dropToRoot = false;
        } else {
            dragOverIndex = -1;
            dropToRoot = info.inList; // empty space inside the list means root
        }
    }

    // called once on release
    function finishDrag(sourceId, sourceKind) {
        const idx = dragOverIndex;
        const zone = dragOverZone;
        const toRoot = dropToRoot;
        dragOverIndex = -1;
        dropToRoot = false;
        if (idx >= 0) {
            const row = workspace.deviceTreeRows[idx];
            if (row)
                performDrop(sourceId, sourceKind, row, idx, zone);
        } else if (toRoot) {
            workspace.moveDeviceNode(sourceId, "", "");
        }
    }

    function performDrop(sourceId, sourceKind, targetRow, targetRowIndex, zone) {
        if (!sourceId || sourceId.length === 0)
            return;
        if (sourceId === String(targetRow.id))
            return;

        if (zone === "into") {
            workspace.moveDeviceNode(sourceId, String(targetRow.id), "");
            return;
        }

        const parent = String(targetRow.parentId);
        const before = (zone === "before") ? String(targetRow.id) : nextSiblingId(targetRowIndex, parent);
        if (before === sourceId)
            return;
        workspace.moveDeviceNode(sourceId, parent, before);
    }

    //
    // REUSABLE ROW PARTS
    //

    // inline editable label, falls back to a greyed placeholder. commits on
    // enter or focus loss and reports the new text through committed
    component EditableLabel: Rectangle {
        id: labelRoot

        property string value: ""
        property string placeholder: ""
        property real placeholderOpacity: 0.4
        property bool editing: false
        property string startText: ""

        signal committed(string text)

        function beginEdit() {
            startText = field.text;
            editing = true;
            field.forceActiveFocus();
            field.selectAll();
        }

        function commit() {
            if (!editing)
                return;
            const text = field.text;
            editing = false;
            if (text !== startText)
                committed(text);
        }

        Layout.fillWidth: true
        Layout.fillHeight: true
        color: "transparent"
        border.color: field.activeFocus ? ThemeColors.highlight : "transparent"
        border.width: 1

        TextInput {
            id: field
            anchors.fill: parent
            verticalAlignment: TextEdit.AlignVCenter
            font: Scaling.uiFont
            color: ThemeColors.text
            selectionColor: ThemeColors.highlight
            selectedTextColor: ThemeColors.highlightedText

            enabled: labelRoot.editing
            selectByMouse: labelRoot.editing

            opacity: labelRoot.value.length > 0 ? 1.0 : labelRoot.placeholderOpacity

            onEditingFinished: labelRoot.commit()
            onActiveFocusChanged: if (!activeFocus && labelRoot.editing)
                labelRoot.commit()
        }

        readonly property string displayText: value.length > 0 ? value : placeholder
        Binding {
            target: field
            property: "text"
            value: labelRoot.displayText
            when: !labelRoot.editing
            restoreMode: Binding.RestoreNone
        }
    }

    // reorder drop hints at the top and bottom edges of a row
    component InsertMarkers: Item {
        id: markersRoot

        property bool showBefore: false
        property bool showAfter: false

        Rectangle {
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            height: Math.max(2, Math.round(2 * Scaling.uiScale))
            color: ThemeColors.highlight
            visible: markersRoot.showBefore
        }
        Rectangle {
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
            }
            height: Math.max(2, Math.round(2 * Scaling.uiScale))
            color: ThemeColors.highlight
            visible: markersRoot.showAfter
        }
    }

    // context menu for node operations, target set before popup
    Menu {
        id: contextMenu

        property string targetGroupId: ""
        property string targetNodeId: ""
        property string targetNodeKind: "" // "group" | "device" | ""

        property bool onGroup: targetNodeKind === "group"
        property bool onDevice: targetNodeKind === "device"

        // we keep a manually updated open bool so we can filter
        // taphandlers that might try to open an already opened menu
        // (if multiple handlers are on top of each other)
        property bool open: false
        Connections {
            target: contextMenu
            function onOpened() {
                contextMenu.open = true;
            }
            function onClosed() {
                contextMenu.open = false;
            }
        }

        MenuItem {
            id: duplicateItem
            text: "Duplicate"
            visible: contextMenu.targetNodeId.length > 0
            height: visible ? implicitHeight : 0
            onTriggered: root.workspace.duplicateDeviceNode(contextMenu.targetNodeId)
        }

        MenuItem {
            id: deleteDeviceItem
            text: "Delete device"
            visible: contextMenu.onDevice
            height: visible ? implicitHeight : 0
            onTriggered: root.workspace.deleteDevice(contextMenu.targetNodeId)
        }

        MenuItem {
            id: deleteGroupKeepDevicesItem
            text: "Delete group && keep devices"
            visible: contextMenu.onGroup
            height: visible ? implicitHeight : 0
            onTriggered: root.workspace.deleteDeviceGroup(contextMenu.targetGroupId)
        }

        MenuSeparator {
            visible: duplicateItem.visible || deleteDeviceItem.visible || deleteGroupKeepDevicesItem.visible
        }

        MenuItem {
            text: "New group"
            onTriggered: root.workspace.createDeviceGroup(contextMenu.targetGroupId)
        }
    }

    //
    // ROW LOADER
    //

    Component {
        id: rowLoader

        Loader {
            required property var modelData
            required property int index

            width: list.width
            height: modelData.kind === "group" ? root.groupRowHeight : root.deviceRowHeight
            sourceComponent: modelData.kind === "group" ? groupDelegate : deviceDelegate
            onLoaded: {
                item.row = modelData;
                item.rowIndex = index;
            }
        }
    }

    //
    // GROUP DELEGATE
    //

    Component {
        id: groupDelegate

        Item {
            id: groupArea

            property var row: ({})
            property int rowIndex: -1
            property string nodeId: row.id !== undefined ? String(row.id) : ""
            property string nodeKind: "group"
            property int depth: row.depth || 0
            readonly property real rowHeight: root.groupRowHeight

            width: list.width
            height: rowHeight
            z: groupDrag.active ? 1 : 0

            Rectangle {
                id: content
                width: list.width
                height: groupArea.rowHeight

                property bool hovered: hoverHandler.hovered
                property bool selected: root.workspace && root.workspace.selectedNodeKind === "group" && root.workspace.selectedNodeId === groupArea.nodeId
                readonly property bool intoTarget: root.dragOverIndex === groupArea.rowIndex && root.dragOverZone === "into"

                readonly property bool outlined: selected && !(root.workspace && root.workspace.selectedOperatorAdapter)

                HoverHandler {
                    id: hoverHandler
                }

                color: intoTarget ? ThemeColors.mid : (hovered ? ThemeColors.midlight : (selected ? ThemeColors.mid : ThemeColors.almostdark))
                border.color: (intoTarget || outlined) ? ThemeColors.highlight : ThemeColors.almostdark
                border.width: (intoTarget || outlined) ? Math.max(1, Math.round(1 * Scaling.uiScale)) : 0

                states: State {
                    when: groupDrag.active
                    ParentChange {
                        target: content
                        parent: dragLayer
                        x: 0
                    }
                }

                DragHandler {
                    id: groupDrag
                    target: null
                    enabled: !groupLabel.editing
                    xAxis.enabled: false
                    yAxis.enabled: true
                    onActiveChanged: {
                        if (active)
                            content.y = groupArea.mapToItem(dragLayer, 0, 0).y;
                        else
                            root.finishDrag(groupArea.nodeId, groupArea.nodeKind);
                    }
                    onCentroidChanged: {
                        if (active) {
                            content.y = groupArea.mapToItem(dragLayer, 0, 0).y + activeTranslation.y;
                            root.updateDragTarget(centroid.scenePosition.x, centroid.scenePosition.y, groupArea.nodeId);
                        }
                    }
                }

                TapHandler {
                    id: groupTap
                    enabled: !groupLabel.editing
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onTapped: (point, button) => {
                        list.forceActiveFocus();
                        if (button === Qt.LeftButton) {
                            root.workspace.selectNode(groupArea.nodeId);
                            list.currentIndex = groupArea.rowIndex;
                        } else if (button === Qt.RightButton) {
                            console.log(groupArea.nodeId);
                            contextMenu.targetGroupId = groupArea.nodeId;
                            contextMenu.targetNodeId = groupArea.nodeId;
                            contextMenu.targetNodeKind = "group";
                            contextMenu.popup();
                        }
                    }
                    onDoubleTapped: (point, button) => {
                        if (button !== Qt.LeftButton)
                            return;
                        const p = groupLabel.mapFromItem(content, point.position.x, point.position.y);
                        if (p.x >= 0 && p.x <= groupLabel.width && p.y >= 0 && p.y <= groupLabel.height)
                            groupLabel.beginEdit();
                    }
                }

                InsertMarkers {
                    anchors.fill: parent
                    showBefore: root.dragOverIndex === groupArea.rowIndex && root.dragOverZone === "before"
                    showAfter: root.dragOverIndex === groupArea.rowIndex && root.dragOverZone === "after"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: root.rowEdgeMargin
                    anchors.rightMargin: root.rowEdgeMargin
                    spacing: root.rowItemSpacing

                    // indent rails plus the chevron column, explicit width so
                    // depth offsets the row. each rail centres on its level's
                    // chevron, the chevron sits in the next column
                    Item {
                        Layout.fillHeight: true
                        Layout.preferredWidth: (groupArea.depth + 1) * root.indentStep

                        Repeater {
                            model: groupArea.depth
                            delegate: Rectangle {
                                required property int index
                                x: index * root.indentStep + Math.round((root.indentStep - root.railThickness) / 2)
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: root.railThickness
                                color: root.treeRailColor
                                opacity: 0.4
                            }
                        }

                        Image {
                            id: chevron
                            x: groupArea.depth * root.indentStep + Math.round((root.indentStep - width) / 2)
                            anchors.verticalCenter: parent.verticalCenter
                            width: root.chevronSize
                            height: root.chevronSize
                            fillMode: Image.PreserveAspectFit
                            sourceSize: Qt.size(root.chevronSize, root.chevronSize)
                            source: groupArea.row.collapsed ? FontAwesome.icon("solid/caret-right") : FontAwesome.icon("solid/caret-down")
                            opacity: 0.6

                            TapHandler {
                                onTapped: {
                                    list.forceActiveFocus();
                                    root.workspace.setDeviceGroupCollapsed(groupArea.nodeId, !groupArea.row.collapsed);
                                }
                            }
                        }
                    }

                    EditableLabel {
                        id: groupLabel
                        value: (groupArea.row.label && String(groupArea.row.label).length > 0) ? String(groupArea.row.label) : ""
                        placeholder: groupArea.nodeId
                        onCommitted: text => root.workspace.setDeviceGroupLabel(groupArea.nodeId, text)
                    }

                    // trailing control column, shared alignment with device rows
                    ListToggleButton {
                        iconOn: FontAwesome.icon("solid/toggle-on")
                        iconOff: FontAwesome.icon("solid/toggle-off")
                        checked: groupArea.row.sessionEnabled === true
                        gated: !groupArea.row.effectiveSessionEnabled
                        tip: "Toggle group in this session"
                        onToggled: root.workspace.setSessionDeviceEnabled(groupArea.nodeId, checked)
                    }

                }
            }
        }
    }

    //
    // DEVICE DELEGATE
    //

    Component {
        id: deviceDelegate

        Item {
            id: dragArea

            property var row: ({})
            property int rowIndex: -1
            property int deviceIndex: row.deviceIndex !== undefined ? row.deviceIndex : -1
            property var modelData: deviceIndex >= 0 && root.workspace ? root.workspace.deviceAdapters[deviceIndex] : null
            property string nodeId: row.id !== undefined ? String(row.id) : ""
            property string nodeKind: "device"
            property int depth: row.depth || 0
            readonly property real rowHeight: root.deviceRowHeight

            width: list.width
            height: rowHeight
            z: deviceDrag.active ? 1 : 0

            Rectangle {
                id: content

                property bool selected: root.workspace && root.workspace.selectedNodeKind === "device" && root.workspace.selectedNodeId === dragArea.nodeId
                property bool hovered: hoverHandler.hovered

                // the device stays the selected node while its panel is open,
                // but an operator picked inside that panel takes over the
                // outline and the scene gizmo along with it
                readonly property bool outlined: selected && !(root.workspace && root.workspace.selectedOperatorAdapter)

                HoverHandler {
                    id: hoverHandler
                }

                width: list.width
                height: dragArea.rowHeight

                color: hovered ? ThemeColors.midlight : selected ? ThemeColors.mid : ThemeColors.almostdark
                border.color: ThemeColors.highlight
                border.width: outlined ? Math.max(1, Math.round(1 * Scaling.uiScale)) : 0

                states: State {
                    when: deviceDrag.active
                    ParentChange {
                        target: content
                        parent: dragLayer
                        x: 0
                    }
                }

                DragHandler {
                    id: deviceDrag
                    target: null
                    enabled: !deviceLabel.editing
                    xAxis.enabled: false
                    yAxis.enabled: true
                    onActiveChanged: {
                        if (active)
                            content.y = dragArea.mapToItem(dragLayer, 0, 0).y;
                        else
                            root.finishDrag(dragArea.nodeId, dragArea.nodeKind);
                    }
                    onCentroidChanged: {
                        if (active) {
                            content.y = dragArea.mapToItem(dragLayer, 0, 0).y + activeTranslation.y;
                            root.updateDragTarget(centroid.scenePosition.x, centroid.scenePosition.y, dragArea.nodeId);
                        }
                    }
                }

                TapHandler {
                    id: deviceTap
                    enabled: !deviceLabel.editing
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onTapped: (point, button) => {
                        list.forceActiveFocus();
                        if (button === Qt.LeftButton) {
                            setSelectedIndex(dragArea.rowIndex, dragArea.deviceIndex);
                        } else if (button === Qt.RightButton) {
                            contextMenu.targetGroupId = String(dragArea.row.parentId || "");
                            contextMenu.targetNodeId = dragArea.nodeId;
                            contextMenu.targetNodeKind = "device";
                            contextMenu.popup();
                        }
                    }
                    onDoubleTapped: (point, button) => {
                        if (button !== Qt.LeftButton || !dragArea.modelData)
                            return;
                        const p = deviceLabel.mapFromItem(content, point.position.x, point.position.y);
                        if (p.x >= 0 && p.x <= deviceLabel.width && p.y >= 0 && p.y <= deviceLabel.height)
                            deviceLabel.beginEdit();
                    }
                }

                InsertMarkers {
                    anchors.fill: parent
                    showBefore: root.dragOverIndex === dragArea.rowIndex && root.dragOverZone === "before"
                    showAfter: root.dragOverIndex === dragArea.rowIndex && root.dragOverZone === "after"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: root.rowEdgeMargin
                    anchors.rightMargin: root.rowEdgeMargin
                    spacing: root.rowItemSpacing

                    // indent rails plus the status column, the dot sits one
                    // level in from the parent group's chevron
                    Item {
                        Layout.fillHeight: true
                        Layout.preferredWidth: (dragArea.depth + 1) * root.indentStep

                        Repeater {
                            model: dragArea.depth
                            delegate: Rectangle {
                                required property int index
                                x: index * root.indentStep + Math.round((root.indentStep - root.railThickness) / 2)
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: root.railThickness
                                color: root.treeRailColor
                                opacity: 0.4
                            }
                        }

                        Rectangle {
                            id: statusCircle
                            x: dragArea.depth * root.indentStep + Math.round((root.indentStep - width) / 2)
                            anchors.verticalCenter: parent.verticalCenter
                            width: 6 * Scaling.uiScale
                            height: 6 * Scaling.uiScale
                            radius: 3.5 * Scaling.uiScale
                            color: {
                                const m = dragArea.modelData;
                                if (!m || m.status === undefined)
                                    return ThemeColors.middark;
                                if (m.pluginNullState)
                                    return ThemeColors.error;
                                switch (m.status) {
                                case Enum.WorkspaceDeviceStatus.Loaded:
                                    return ThemeColors.neutralSuccess;
                                case Enum.WorkspaceDeviceStatus.Active:
                                    return ThemeColors.success;
                                case Enum.WorkspaceDeviceStatus.Missing:
                                    return ThemeColors.error;
                                default:
                                    return ThemeColors.middark;
                                }
                            }
                        }
                    }

                    EditableLabel {
                        id: deviceLabel
                        placeholderOpacity: 0.5
                        value: (dragArea.row.label && String(dragArea.row.label).length > 0) ? String(dragArea.row.label) : ""
                        placeholder: dragArea.nodeId
                        onCommitted: text => root.workspace.setDeviceLabel(dragArea.nodeId, text)
                    }

                    // device type, kept inline just left of the control column
                    Text {
                        id: deviceTypeText
                        Layout.alignment: Qt.AlignVCenter
                        elide: Text.ElideRight
                        text: dragArea.modelData ? (dragArea.modelData.displayName() + (dragArea.modelData.pluginNullState ? " (Unloaded)" : "")) : ""
                        color: ThemeColors.text
                        opacity: dragArea.modelData && dragArea.modelData.pluginNullState ? .25 : .5
                        font: Scaling.uiFont
                    }

                    // trailing control column, aligned with group rows
                    ListToggleButton {
                        iconOn: FontAwesome.icon("solid/toggle-on")
                        iconOff: FontAwesome.icon("solid/toggle-off")
                        baseOpacity: 0.5
                        hoverOpacity: 0.7
                        checked: dragArea.row.sessionEnabled === true
                        gated: checked && !dragArea.row.effectiveSessionEnabled
                        tip: "Toggle device in this session"
                        onToggled: root.workspace.setSessionDeviceEnabled(dragArea.nodeId, checked)
                    }
                }
            }
        }
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        color: ThemeColors.alternateBase
        border.color: root.dropToRoot ? ThemeColors.highlight : ThemeColors.mid
        border.width: 1
        clip: true

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: 1

            model: root.workspace ? root.workspace.deviceTreeRows : []
            focus: true
            activeFocusOnTab: true
            keyNavigationEnabled: true
            // disabled so per-row vertical DragHandlers don't fight the flick;
            // scrollbar + wheel still scroll
            interactive: false
            boundsBehavior: Flickable.StopAtBounds

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: event => {
                    if (list.contentHeight <= list.height)
                        return;
                    const dy = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y;
                    list.contentY = Math.max(0, Math.min(list.contentHeight - list.height, list.contentY - dy));
                }
            }

            // keyboard: up/down moves, enter/space selects a device row
            Keys.onPressed: event => {
                if (event.key === Qt.Key_Up) {
                    list.decrementCurrentIndex();
                    list.positionViewAtIndex(list.currentIndex, ListView.Contain);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Down) {
                    list.incrementCurrentIndex();
                    list.positionViewAtIndex(list.currentIndex, ListView.Contain);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                    if (list.currentIndex >= 0) {
                        const r = list.model[list.currentIndex];
                        if (r && r.kind === "device")
                            setSelectedIndex(list.currentIndex, r.deviceIndex);
                    }
                    event.accepted = true;
                }
            }

            delegate: rowLoader

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    // right-click on empty list space (underneath the listview items)
                    if (!contextMenu.open) {
                        contextMenu.targetGroupId = "";
                        contextMenu.targetNodeId = "";
                        contextMenu.targetNodeKind = "";
                        contextMenu.popup();
                    }
                }
            }
        }

        // currently being dragged rows are parented to this so they appear on top of everything else
        Item {
            id: dragLayer
            anchors.fill: parent
            z: 1
            opacity: 0.5
        }
    }
}
