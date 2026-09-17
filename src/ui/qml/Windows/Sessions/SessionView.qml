import QtQuick
import QtQuick.Controls
import QtQuick3D
import QtQuick3D.Helpers

import Pointcaster 1.0

import Gizmo3D

Item {
    id: root

    required property var workspace
    required property var sessionAdapter
    required property var deviceAdapters
    property var cameraAdapter: null

    readonly property bool isSelectedSession: (workspace && sessionAdapter) ? String(workspace.selectedSessionId) === String(sessionAdapter.id) : false

    function requestSelection() {
        if (workspace && sessionAdapter && !root.isSelectedSession)
            workspace.selectedSessionId = String(sessionAdapter.id);
    }

    property var selectedDeviceAdapter: (root.workspace && deviceAdapters && root.workspace.selectedNodeKind === "device" && deviceAdapters.length > 0) ? root.deviceAdapters[root.workspace.selectedDeviceIndex] : null
    property var selectedGroupAdapter: (root.workspace && root.workspace.selectedNodeKind === "group") ? root.workspace.selectedDeviceGroupAdapter : null
    property var selectedTransformAdapter: selectedDeviceAdapter || selectedGroupAdapter
    property var selectedOperatorAdapter: root.workspace ? root.workspace.selectedOperatorAdapter ? root.workspace.selectedOperatorAdapter.configAdapter : null : null

    property var selectionPosition: selectionPositionOrDefault()
    function selectionPositionOrDefault() {
        if (selectedOperatorAdapter) {
            var pc = selectedOperatorAdapter.value("camera/position");
            if (pc === undefined || pc === null || pc.x === undefined)
                return Qt.vector3d(0, 0, 0);
            return Qt.vector3d(pc.x * 100, pc.y * 100, pc.z * 100);
        }
        if (selectedTransformAdapter) {
            var lp = selectedTransformAdapter.value("transform/position"); // metres, local
            var worldM = selectionParentWorld.times(Qt.vector3d(lp.x, lp.y, lp.z));
            return Qt.vector3d(worldM.x * 100, worldM.y * 100, worldM.z * 100);
        }
        return Qt.vector3d(0, 0, 0);
    }

    // Look-at position for operators that expose camera/look_at_position.
    property var selectionLookAtPosition: selectionLookAtPositionOrDefault()
    function selectionLookAtPositionOrDefault() {
        if (!selectedOperatorAdapter)
            return Qt.vector3d(0, 0, 0);
        var v = selectedOperatorAdapter.value("camera/look_at_position");
        if (v === undefined || v === null || v.x === undefined)
            return Qt.vector3d(0, 0, 0);
        return Qt.vector3d(v.x * 100, v.y * 100, v.z * 100);
    }

    readonly property bool _selectionHasGizmoTarget: {
        if (selectedOperatorAdapter) {
            var pc = selectedOperatorAdapter.value("camera/position");
            return pc !== undefined && pc !== null && pc.x !== undefined;
        }
        if (selectedTransformAdapter) {
            var lp = selectedTransformAdapter.value("transform/position");
            return lp !== undefined && lp !== null && lp.x !== undefined;
        }
        return false;
    }

    property bool selectionHasBounds: selectionHasBoundsOrDefault()
    function selectionHasBoundsOrDefault() {
        if (selectedOperatorAdapter) {
            if (!selectedOperatorAdapter.value("active"))
                return false;
            var ob = selectedOperatorAdapter.value("bounds");
            return ob !== undefined && ob !== null && ob.min !== undefined;
        }
        if (!selectedTransformAdapter)
            return false;
        var b = selectedTransformAdapter.value("transform/bounds");
        if (b === undefined || b === null || b.min === undefined)
            return false;
        return b.active !== false;
    }

    // the first radius the selected configuration carries
    property string selectionRadiusPath: selectionRadiusPathOrEmpty()
    function selectionRadiusPathOrEmpty() {
        const adapter = selectedOperatorAdapter || selectedTransformAdapter;
        if (!adapter || !adapter.radiusPaths)
            return "";
        const paths = adapter.radiusPaths();
        return paths && paths.length > 0 ? paths[0] : "";
    }

    property bool selectionHasRadius: selectionHasRadiusOrDefault()
    function selectionHasRadiusOrDefault() {
        if (!selectionRadiusPath)
            return false;
        const adapter = selectedOperatorAdapter || selectedTransformAdapter;
        const held = adapter.value(selectionRadiusPath);
        if (held === undefined || held === null)
            return false;
        // same rule the box follows: a switch that is off hides the drawing
        return held.active !== false;
    }

    readonly property bool _selectionHasLookAt: {
        if (!selectedOperatorAdapter)
            return false;
        var v = selectedOperatorAdapter.value("camera/look_at_position");
        return v !== undefined && v !== null && v.x !== undefined;
    }

    property quaternion selectionRotation: selectionRotationOrDefault()
    function selectionRotationOrDefault() {
        if (!selectedTransformAdapter)
            return Qt.quaternion(1, 0, 0, 0);
        var euler = selectedTransformAdapter.value("transform/rotation");
        var parentQ = TransformUtils.rotationFromMatrix(selectionParentWorld);
        return parentQ.times(TransformUtils.quaternionFromEuler(euler));
    }

    property matrix4x4 selectionParentWorld: Qt.matrix4x4()

    property matrix4x4 selectionOwnWorld: Qt.matrix4x4()

    function updateSelectionParentWorld() {
        if (root.workspace && selectedTransformAdapter) {
            var id = root.workspace.selectedNodeId;
            if (id && id.length > 0) {
                selectionParentWorld = root.workspace.nodeAncestorWorldMatrix(id);
                selectionOwnWorld = root.workspace.nodeWorldMatrix(id);
                return;
            }
        }
        selectionParentWorld = Qt.matrix4x4();
        selectionOwnWorld = Qt.matrix4x4();
    }

    signal selectionTransformUpdate

    onSelectedTransformAdapterChanged: root.selectionTransformUpdate()

    onSelectionTransformUpdate: {
        updateSelectionParentWorld();
        selectionPosition = selectionPositionOrDefault();
        selectionLookAtPosition = selectionLookAtPositionOrDefault();
        selectionRotation = selectionRotationOrDefault();
        selectionHasBounds = selectionHasBoundsOrDefault();
        selectionRadiusPath = selectionRadiusPathOrEmpty();
        selectionHasRadius = selectionHasRadiusOrDefault();
    }

    Connections {
        target: root.workspace
        function onSelectedDeviceIndexChanged() {
            root.selectionTransformUpdate();
        }
        function onSelectedOperatorAdapterChanged() {
            root.selectionTransformUpdate();
        }
        function onSelectedNodeChanged() {
            root.selectionTransformUpdate();
        }
        function onSelectedDeviceGroupAdapterChanged() {
            root.selectionTransformUpdate();
        }
    }

    Connections {
        target: selectedTransformAdapter
        function onFieldChanged(path) {
            if (path.includes("transform")) {
                root.selectionTransformUpdate();
                return;
            }
            // flicking a distance on or off changes what the scene draws
            if (path === root.selectionRadiusPath)
                root.selectionHasRadius = root.selectionHasRadiusOrDefault();
        }
    }

    // deactivating an operator takes its box away, the same way switching
    // its crop bounds off does for a device
    Connections {
        target: root.selectedOperatorAdapter
        ignoreUnknownSignals: true
        function onFieldChanged(path) {
            if (path === "active")
                root.selectionHasBounds = root.selectionHasBoundsOrDefault();
        }
    }

    // this view draws the rendered streams belonging to its own session and devices
    property var renderPathsForSession: []

    function refreshRenderPathsForSession() {
        if (!workspace || !sessionAdapter) {
            root.renderPathsForSession = [];
            return;
        }
        const sessionId = String(sessionAdapter.id);
        root.renderPathsForSession = workspace.renderPaths.filter(path => workspace.sessionDrawsRenderPath(sessionId, path));
    }

    Connections {
        target: root.workspace
        function onRenderPathsChanged() {
            root.refreshRenderPathsForSession();
        }
        // a device switching in or out of a session changes what it draws
        function onSessionAdaptersChanged() {
            root.refreshRenderPathsForSession();
        }
        function onDeviceAdaptersChanged() {
            root.refreshRenderPathsForSession();
        }
    }

    // one unit circle, scaled to whatever distance each radius disc draws
    readonly property int discSegments: 96
    readonly property var discPositions: {
        const points = [Qt.vector3d(0, 0, 0)];
        for (let i = 0; i < root.discSegments; i++) {
            const angle = i / root.discSegments * Math.PI * 2;
            points.push(Qt.vector3d(Math.cos(angle), 0, Math.sin(angle)));
        }
        return points;
    }
    readonly property var discFillIndexes: {
        const indexes = [];
        for (let i = 0; i < root.discSegments; i++) {
            indexes.push(0, 1 + i, 1 + (i + 1) % root.discSegments);
        }
        return indexes;
    }
    readonly property var discRimIndexes: {
        const indexes = [];
        for (let i = 0; i < root.discSegments; i++) {
            indexes.push(1 + i, 1 + (i + 1) % root.discSegments);
        }
        return indexes;
    }

    readonly property real defaultCameraDistance: 250
    readonly property vector3d defaultOrbitOriginPosition: Qt.vector3d(0, 0, 0)
    readonly property quaternion defaultOrbitOriginRotation: {
        const pitch = -17;
        const yaw = 0;
        const pitchRad = pitch * Math.PI / 180;
        const yawRad = yaw * Math.PI / 180;
        const qPitch = Qt.quaternion(Math.cos(pitchRad * 0.5), Math.sin(pitchRad * 0.5), 0, 0);
        const qYaw = Qt.quaternion(Math.cos(yawRad * 0.5), 0, Math.sin(yawRad * 0.5), 0);
        return qYaw.times(qPitch);
    }

    property bool showBorder: root.isSelectedSession && workspace && workspace.sessionAdapters.length > 1
    property color borderColor: ThemeColors.highlight

    // guards to prevent config<->UI updates from immediately re-committing (causing undo/redo feedback loops)
    property bool _applyingConfigCameraTransform: false
    property bool _applyingConfigCameraToggles: false
    property bool _committing_config: false

    function _vector3dFromAdapterPosition(p) {
        // p is expected to be QVector3D-ish in QML (has x/y/z)
        if (!p)
            return Qt.vector3d(0, 0, 0);

        return Qt.vector3d(Number(p.x) || 0, Number(p.y) || 0, Number(p.z) || 0);
    }

    function _quaternionFromAdapterRotation(r) {
        if (!r)
            return Qt.quaternion(1, 0, 0, 0);

        return Qt.quaternion(Number(r.scalar) || 1, Number(r.x) || 0, Number(r.y) || 0, Number(r.z) || 0);
    }

    function applyCameraTogglesFromConfig() {
        if (!root.cameraAdapter)
            return;

        root._applyingConfigCameraToggles = true;
        sessionControls.viewLocked = !!root.cameraAdapter.locked;
        sessionControls.gridEnabled = !!root.cameraAdapter.show_grid;
        sessionControls.orthographicEnabled = !!root.cameraAdapter.orthographic;
        // drop the guard next tick so any bindings/animations settle first
        Qt.callLater(function () {
            root._applyingConfigCameraToggles = false;
        });
    }

    function applyCameraTransformFromConfig() {
        if (!root.cameraAdapter)
            return;

        root._applyingConfigCameraTransform = true;
        orbitOrigin.position = _vector3dFromAdapterPosition(root.cameraAdapter.position);
        orbitOrigin.rotation = _quaternionFromAdapterRotation(root.cameraAdapter.rotation);
        camera.z = root.cameraAdapter.distance;
        // drop the guard next tick so any bindings/animations settle first
        Qt.callLater(function () {
            root._applyingConfigCameraTransform = false;
        });
    }

    function commitCameraTransformToConfig() {
        if (!root.cameraAdapter)
            return;

        if (root._applyingConfigCameraTransform)
            return;

        root._committing_config = true;
        Qt.callLater(function () {
            root._committing_config = false;
        });
        root.cameraAdapter.set_position(orbitOrigin.position);
        root.cameraAdapter.set_rotation(orbitOrigin.rotation);
        root.cameraAdapter.set_distance(camera.z);
    }

    function refreshFromAdapter() {
        cameraAdapter = sessionAdapter ? sessionAdapter.cameraAdapter : null;
        if (!cameraAdapter)
            return;
        // initial pull: config -> UI
        applyCameraTogglesFromConfig();
        applyCameraTransformFromConfig();
        // snap projection immediately on startup / adapter swap
        camera.setBlend(sessionControls.orthographicEnabled ? 1 : 0, false);
    }

    anchors.fill: parent
    Component.onCompleted: {
        refreshFromAdapter();
        refreshRenderPathsForSession();
    }
    onSessionAdapterChanged: {
        refreshFromAdapter();
        refreshRenderPathsForSession();
    }

    // config -> UI
    Connections {
        function onLockedChanged() {
            applyCameraTogglesFromConfig();
        }

        function onShow_gridChanged() {
            applyCameraTogglesFromConfig();
        }

        function onOrthographicChanged() {
            applyCameraTogglesFromConfig();
            // config-driven change gets no animation
            camera.setBlend(sessionControls.orthographicEnabled ? 1 : 0, false);
        }

        function onPositionChanged() {
            if (root._committing_config)
                return;

            applyCameraTransformFromConfig();
        }

        function onRotationChanged() {
            if (root._committing_config)
                return;

            applyCameraTransformFromConfig();
        }

        function onDistanceChanged() {
            if (root._committing_config)
                return;

            applyCameraTransformFromConfig();
        }

        target: root.cameraAdapter
        enabled: !!root.cameraAdapter
    }

    // UI -> config (toggles)
    Connections {
        function onViewLockedChanged() {
            if (!root.cameraAdapter || root._applyingConfigCameraToggles)
                return;

            const desired = !!sessionControls.viewLocked;
            const current = !!root.cameraAdapter.locked;
            if (desired === current)
                return;

            root.cameraAdapter.set_locked(desired);
        }

        function onGridEnabledChanged() {
            if (!root.cameraAdapter || root._applyingConfigCameraToggles)
                return;

            const desired = !!sessionControls.gridEnabled;
            const current = !!root.cameraAdapter.show_grid;
            if (desired === current)
                return;

            root.cameraAdapter.set_show_grid(desired);
        }

        function onOrthographicEnabledChanged() {
            // Animate only for user-driven toggles; config-driven updates should snap.
            const targetBlend = sessionControls.orthographicEnabled ? 1 : 0;
            camera.setBlend(targetBlend, !root._applyingConfigCameraToggles);
            if (!root.cameraAdapter || root._applyingConfigCameraToggles)
                return;

            const desired = !!sessionControls.orthographicEnabled;
            const current = !!root.cameraAdapter.orthographic;
            if (desired === current)
                return;

            root.cameraAdapter.orthographic = desired;
        }

        target: sessionControls
    }

    View3D {
        id: view

        property var selectedObject: null
        readonly property quaternion gizmoBasis: Qt.quaternion(1, 0, 0, 0)
        readonly property quaternion gizmoBasisInv: gizmoBasis.conjugated()

        function orbitToGizmoRotation(q) {
            return gizmoBasis.times(q).times(gizmoBasisInv);
        }

        function gizmoToOrbitRotation(q) {
            return gizmoBasisInv.times(q).times(gizmoBasis);
        }

        anchors.fill: parent
        camera: camera

        // Primary selection proxy — used by selectionGizmo for position/rotation/scale.
        Node {
            id: selectionProxy
            x: selectionGizmo.dragging ? selectionGizmo.dragPosition.x : selectionPosition.x
            y: selectionGizmo.dragging ? selectionGizmo.dragPosition.y : selectionPosition.y
            z: selectionGizmo.dragging ? selectionGizmo.dragPosition.z : selectionPosition.z
            rotation: selectionGizmo.dragging ? selectionGizmo.dragOrientation : selectionRotation
        }

        // Look-at proxy — used by lookAtGizmo when the operator exposes
        // camera/look_at_position.
        Node {
            id: lookAtProxy
            x: lookAtGizmo.dragging ? lookAtGizmo.dragPosition.x : selectionLookAtPosition.x
            y: lookAtGizmo.dragging ? lookAtGizmo.dragPosition.y : selectionLookAtPosition.y
            z: lookAtGizmo.dragging ? lookAtGizmo.dragPosition.z : selectionLookAtPosition.z
        }

        Node {
            id: sessionCloudNode

            property var adapter: root.workspace && root.sessionAdapter ? root.workspace.sessionPointCloudAdapterFor(root.sessionAdapter.id) : null

            Model {
                geometry: PointCloudGeometry {
                    id: sessionGeo
                    pointCloudAdapter: sessionCloudNode.adapter
                    enabled: sessionCloudNode.adapter !== null
                }
                materials: [
                    PointCloudMaterial {
                        uPointSize: viewController.shaderPointSize
                        uViewportHeight: viewController.shaderViewportHeight
                        uHasPointScale: sessionGeo.hasPointScale
                    }
                ]
            }

            Connections {
                target: sessionCloudNode.adapter
                enabled: sessionCloudNode.adapter !== null
                ignoreUnknownSignals: true
                function onPointCloudUpdated() {
                    sessionGeo.updateGeometry();
                }
            }
        }

        // the paths switched on for rendering
        Repeater3D {
            model: root.renderPathsForSession

            Node {
                id: streamNode

                readonly property string streamPath: modelData
                readonly property var streamSource: root.workspace ? root.workspace.streamSourceFor(streamNode.streamPath) : null
                readonly property bool isBounds: streamInstances.kind === StreamInstancing.Bounds
                readonly property bool isDisc: streamNode.streamSource ? streamNode.streamSource.isRadius : false
                readonly property real discRadius: streamNode.streamSource ? streamNode.streamSource.radiusMetres * 100 : 0

                function refreshParentWorld() {
                    // a box is cropped in its owner's parent space, while a
                    // distance is measured from where the owner itself sits
                    const parentWorld = root.workspace ? (streamNode.isDisc ? root.workspace.renderPathOwnerWorld(streamNode.streamPath) : root.workspace.renderPathParentWorld(streamNode.streamPath)) : Qt.matrix4x4();
                    // the transform carries its translation in metres
                    streamNode.position = TransformUtils.positionFromMatrix(parentWorld).times(100);
                    streamNode.rotation = TransformUtils.rotationFromMatrix(parentWorld);
                    streamNode.scale = TransformUtils.scaleFromMatrix(parentWorld);
                }

                Component.onCompleted: streamNode.refreshParentWorld()

                StreamInstancing {
                    id: streamInstances
                    streamAdapter: (streamNode.streamSource && !streamNode.isDisc) ? streamNode.streamSource.streamAdapter() : null
                    color: streamNode.isBounds ? ThemeColors.yellow : ThemeColors.highlight
                    // only the solid boxes need sorting against each other
                    hasTransparency: streamInstances.kind !== StreamInstancing.Voxels
                    depthSortingEnabled: streamInstances.kind !== StreamInstancing.Voxels
                }

                // the radial disc model itself
                Node {
                    visible: streamNode.isDisc && streamNode.discRadius > 0
                    scale: Qt.vector3d(streamNode.discRadius, 1, streamNode.discRadius)

                    Model {
                        geometry: ProceduralMesh {
                            positions: root.discPositions
                            indexes: root.discFillIndexes
                        }

                        materials: [
                            PrincipledMaterial {
                                baseColor: ThemeColors.yellow
                                opacity: 0.05
                                alphaMode: PrincipledMaterial.Blend
                                lighting: PrincipledMaterial.NoLighting
                                cullMode: Material.NoCulling
                            }
                        ]
                    }

                    Model {
                        geometry: ProceduralMesh {
                            primitiveMode: ProceduralMesh.Lines
                            positions: root.discPositions
                            indexes: root.discRimIndexes
                        }

                        materials: [
                            PrincipledMaterial {
                                baseColor: ThemeColors.yellow
                                lighting: PrincipledMaterial.NoLighting
                            }
                        ]
                    }
                }

                Model {
                    source: "#Cube"
                    visible: !streamNode.isDisc && streamInstances.kind !== StreamInstancing.Voxels

                    instancing: streamInstances

                    materials: [
                        PrincipledMaterial {
                            baseColor: "white"
                            opacity: streamNode.isBounds ? 0.05 : 0.35
                            alphaMode: PrincipledMaterial.Blend
                            lighting: PrincipledMaterial.NoLighting
                            cullMode: Material.NoCulling
                        }
                    ]
                }

                Model {
                    visible: !streamNode.isDisc && streamInstances.kind !== StreamInstancing.Aabbs

                    geometry: ProceduralMesh {
                        primitiveMode: ProceduralMesh.Lines
                        positions: [
                            Qt.vector3d(-50, -50, -50),
                            Qt.vector3d(50, -50, -50),
                            Qt.vector3d(50, -50, 50),
                            Qt.vector3d(-50, -50, 50),
                            Qt.vector3d(-50, 50, -50),
                            Qt.vector3d(50, 50, -50),
                            Qt.vector3d(50, 50, 50),
                            Qt.vector3d(-50, 50, 50)
                        ]
                        indexes: [
                            0, 1, 1, 2, 2, 3, 3, 0, // bottom
                            4, 5, 5, 6, 6, 7, 7, 4, // top
                            0, 4, 1, 5, 2, 6, 3, 7  // the uprights
                        ]
                    }

                    instancing: streamInstances

                    materials: [
                        PrincipledMaterial {
                            baseColor: "white"
                            lighting: PrincipledMaterial.NoLighting
                        }
                    ]
                }

                Connections {
                    target: sessionCloudNode.adapter
                    enabled: sessionCloudNode.adapter !== null
                    ignoreUnknownSignals: true
                    function onPointCloudUpdated() {
                        streamInstances.updateInstances();
                    }
                }

                Connections {
                    target: streamNode.streamSource
                    ignoreUnknownSignals: true
                    function onContentChanged() {
                        streamInstances.updateInstances();
                        streamNode.refreshParentWorld();
                    }
                }
            }
        }

        // Repeater3D {
        //     model: root.deviceAdapters

        //     Node {
        //         id: deviceNode

        //         property bool isSelected: index === root.workspace.selectedDeviceIndex && !root.selectedOperatorAdapter

        //         // TODO
        //         // visual offset during gizmo drag (translation only for now cause that's easier than figuring out how to update rotation origins lol)
        //         x: isSelected && selectionGizmo.dragging ? selectionGizmo.dragPosition.x - selectionPosition.x : 0
        //         y: isSelected && selectionGizmo.dragging ? selectionGizmo.dragPosition.y - selectionPosition.y : 0
        //         z: isSelected && selectionGizmo.dragging ? selectionGizmo.dragPosition.z - selectionPosition.z : 0

        //         Model {
        //             id: model

        //             property string model: modelData ? modelData.id + "_model" : ""
        //             property string geo: model + "_geo"

        //             pickable: true

        //             geometry: PointCloudGeometry {
        //                 id: geo
        //                 pointCloudAdapter: modelData.pointCloudAdapter() ?? []
        //                 enabled: modelData.render
        //             }

        //             materials: [
        //                 PointCloudMaterial {
        //                     uPointSize: viewController.shaderPointSize
        //                 }
        //             ]
        //         }

        //         Connections {
        //             target: modelData
        //             function onPointCloudUpdated() {
        //                 geo.updateGeometry();
        //             }
        //         }
        //     }
        // }

        Connections {
            target: selectedDeviceAdapter
            function onFieldChanged(path) {
                // TODO might want to debounce
                if (path.includes("transform")) {
                    root.selectionTransformUpdate();
                }
            }
        }

        Connections {
            target: selectedOperatorAdapter
            function onFieldChanged(path) {
                if (path.includes("camera")) {
                    root.selectionTransformUpdate();
                }
            }
        }

        // Repeater3D {
        //     model: root.deviceAdapters

        //     Node {
        //         id: deviceNode

        //         property bool isSelected: index === root.workspace.selectedDeviceIndex && !root.selectedOperatorAdapter

        //         // TODO
        //         // visual offset during gizmo drag (translation only for now cause that's easier than figuring out how to update rotation origins lol)
        //         x: isSelected && selectionGizmo.dragging ? selectionGizmo.dragPosition.x - selectionPosition.x : 0
        //         y: isSelected && selectionGizmo.dragging ? selectionGizmo.dragPosition.y - selectionPosition.y : 0
        //         z: isSelected && selectionGizmo.dragging ? selectionGizmo.dragPosition.z - selectionPosition.z : 0

        //         Model {
        //             id: model

        //             property string model: modelData ? modelData.id + "_model" : ""
        //             property string geo: model + "_geo"

        //             pickable: true

        //             geometry: PointCloudGeometry {
        //                 id: geo
        //                 pointCloudAdapter: modelData.pointCloudAdapter() ?? []
        //                 enabled: modelData.render
        //             }

        //             materials: [
        //                 PointCloudMaterial {
        //                     uPointSize: viewController.shaderPointSize
        //                 }
        //             ]
        //         }

        //         Connections {
        //             target: modelData
        //             function onPointCloudUpdated() {
        //                 geo.updateGeometry();
        //             }
        //         }
        //     }
        // }

        environment: SceneEnvironment {
            clearColor: AppSettings.backgroundColor
            backgroundMode: SceneEnvironment.Color
            depthPrePassEnabled: false
            // fog: Fog { enabled: false }

            antialiasingMode: {
                switch (AppSettings.antialiasing) {
                case AppSettings.SSAA:
                    return SceneEnvironment.SSAA;
                case AppSettings.MSAA:
                    return SceneEnvironment.MSAA;
                case AppSettings.ProgressiveAA:
                    return SceneEnvironment.ProgressiveAA;
                default:
                    return SceneEnvironment.NoAA;
                }
            }
        }

        // ---------- CAMERA ----------
        Node {
            id: orbitOrigin

            position: root.defaultOrbitOriginPosition
            rotation: root.defaultOrbitOriginRotation

            SessionCamera {
                id: camera
            }
        }

        OrbitViewController {
            id: viewController
            anchors.fill: parent
            camera: camera
            origin: orbitOrigin
            enabled: !sessionControls.orbitRotationRunning && !sessionControls.viewLocked
            onMouseHeldChanged: {
                if (mouseHeld) {
                    root.requestSelection();
                    return;
                }
                if (sessionControls.orbitRotationRunning)
                    return;

                // on release:
                root.commitCameraTransformToConfig();
            }
            onScrollingChanged: {
                // scrolling reads as navigation, not as picking a session
                if (scrolling || sessionControls.orbitRotationRunning)
                    return;
                root.commitCameraTransformToConfig();
            }
            onDragActiveChanged: {
                if (dragActive) {
                    root.requestSelection();
                    return;
                }
                // Commit once when the interaction finishes.
                root.commitCameraTransformToConfig();
            }
        }

        // a node for camera orbit gizmo to bind to
        Node {
            id: gizmoTarget
        }

        Binding {
            target: gizmoTarget
            property: "rotation"
            value: view.orbitToGizmoRotation(camera.sceneRotation.conjugated())
        }

        // ---------- PICKING + FOCUS ----------
        TapHandler {
            target: view
            acceptedButtons: Qt.LeftButton
            onTapped: (p, b) => {
                root.requestSelection();
                const hit = view.pick(p.position.x, p.position.y).objectHit;
                view.selectedObject = hit || null;
            }
            onDoubleTapped: (p, b) => {
                if (sessionControls.viewLocked)
                    return;

                const result = view.pick(p.position.x, p.position.y);
                const hit = result.objectHit;
                if (!hit)
                    return;

                focusAnimation.from = orbitOrigin.position;
                focusAnimation.to = hit.position;
                focusAnimation.start();
            }
        }

        Model {
            id: groundGrid
            visible: sessionControls.gridEnabled

            scale: Qt.vector3d(1000, 1000, 0)
            eulerRotation: Qt.vector3d(90, 0, 0)

            property real metres: Math.round(AppSettings.gridSizeMetres)
            geometry: GridGeometry {
                horizontalLines: Math.round(groundGrid.metres + 1)
                verticalLines: Math.round(groundGrid.metres + 1)
            }

            materials: [
                PrincipledMaterial {
                    baseColor: ThemeColors.midlight
                    lighting: PrincipledMaterial.NoLighting
                    lineWidth: Math.round(Scaling.uiScale * 2)
                }
            ]
        }

        Model {
            id: xAxis
            visible: sessionControls.gridEnabled
            eulerRotation: Qt.vector3d(90, 0, 0)
            scale: Qt.vector3d(groundGrid.metres * 500, 1, 1)
            x: groundGrid.metres * 25

            geometry: GridGeometry {
                horizontalLines: 1
                verticalLines: 1
            }

            materials: [
                PrincipledMaterial {
                    baseColor: ThemeColors.red
                    lighting: PrincipledMaterial.NoLighting
                    lineWidth: Math.round(Scaling.uiScale * 4)
                }
            ]
        }

        Model {
            id: zAxis
            visible: sessionControls.gridEnabled
            eulerRotation: Qt.vector3d(90, 0, 00)
            scale: Qt.vector3d(1, groundGrid.metres * 500, 1)
            z: groundGrid.metres * 25

            geometry: GridGeometry {
                horizontalLines: 1
                verticalLines: 1
            }

            materials: [
                PrincipledMaterial {
                    baseColor: ThemeColors.blue
                    lighting: PrincipledMaterial.NoLighting
                    lineWidth: Math.round(Scaling.uiScale * 4)
                }
            ]
        }
    }

    // session window GUI overlaid on top of the View3D
    SessionControls {
        id: sessionControls
        workspace: root.workspace
        sessionView: root
        anchors.fill: parent
        view3d: view
        gizmoTarget: gizmoTarget
        orbitOrigin: orbitOrigin
        z: 120
        onRequestHomeCamera: {
            if (sessionControls.viewLocked)
                return;

            focusAnimation.stop();
            homeOrbitOriginAnim.stop();
            homeOrbitOriginPositionAnim.from = orbitOrigin.position;
            homeOrbitOriginRotationAnim.from = orbitOrigin.rotation;
            homeOrbitOriginDistanceAnim.from = camera.z;
            homeOrbitOriginAnim.start();
        }
    }

    // Primary transform gizmo: position/rotation/scale for devices; position
    // (camera/position) for operators.
    SelectionGizmo {
        id: selectionGizmo
        visible: root.isSelectedSession && root._selectionHasGizmoTarget && !sessionControls.viewLocked && sessionControls.gizmoEnabled
        view3d: view
        targetNode: selectionProxy
        mode: GizmoEnums.Mode.Both
        targetAdapter: root.selectedOperatorAdapter || root.selectedTransformAdapter
        cameraTarget: root.selectedOperatorAdapter !== null
        cameraPositionPath: "camera/position"
        parentWorldTransform: root.selectedOperatorAdapter ? Qt.matrix4x4() : root.selectionParentWorld
        cameraProjection: camera.projection
        z: 99
    }

    // Look-at gizmo: translation-only handle for camera/look_at_position.
    // Only visible when the selected operator exposes that path.
    // Verify GizmoEnums.Mode.Translate matches your GizmoEnums enum value name.
    SelectionGizmo {
        id: lookAtGizmo
        visible: root.isSelectedSession && root._selectionHasLookAt && !sessionControls.viewLocked && sessionControls.gizmoEnabled
        view3d: view
        targetNode: lookAtProxy
        mode: GizmoEnums.Mode.Translate
        targetAdapter: root.selectedOperatorAdapter
        cameraTarget: true
        cameraPositionPath: "camera/look_at_position"
        cameraProjection: camera.projection
        z: 99
    }

    readonly property bool _boundsGizmoActive: root.isSelectedSession && root.selectionHasBounds && !sessionControls.viewLocked && sessionControls.gizmoEnabled

    // Bounds gizmo: push/pull box for an operator's bounds, or a device's crop
    // bounds. Operators see the cloud already world-transformed, so their box
    // needs no parent transform.
    SelectionBoundsGizmo {
        id: boundsGizmo
        visible: root._boundsGizmoActive
        view3d: view
        targetAdapter: root.selectedOperatorAdapter || root.selectedTransformAdapter
        boundsPath: root.selectedOperatorAdapter ? "bounds" : "transform/bounds"
        parentWorldTransform: root.selectedOperatorAdapter ? Qt.matrix4x4() : root.selectionParentWorld
        z: 99
    }

    SelectionRadiusGizmo {
        id: radiusGizmo
        visible: root.isSelectedSession && root.selectionHasRadius && !sessionControls.viewLocked && sessionControls.gizmoEnabled
        view3d: view
        targetAdapter: root.selectedOperatorAdapter || root.selectedTransformAdapter
        radiusPath: root.selectionRadiusPath
        parentWorldTransform: root.selectedOperatorAdapter ? Qt.matrix4x4() : root.selectionOwnWorld
        z: 99
    }

    // Camera move animations
    Vector3dAnimation {
        id: focusAnimation

        target: orbitOrigin
        property: "position"
        duration: 350
        easing.type: Easing.OutQuart
    }

    ParallelAnimation {
        id: homeOrbitOriginAnim
        alwaysRunToEnd: true
        onFinished: {
            orbitOrigin.position = root.defaultOrbitOriginPosition;
            orbitOrigin.rotation = root.defaultOrbitOriginRotation;
            camera.z = root.defaultCameraDistance;
            root.commitCameraTransformToConfig();
        }

        Vector3dAnimation {
            id: homeOrbitOriginPositionAnim
            to: root.defaultOrbitOriginPosition
            target: orbitOrigin
            property: "position"
            duration: 420
            easing.type: Easing.OutCubic
        }

        PropertyAnimation {
            id: homeOrbitOriginRotationAnim
            to: root.defaultOrbitOriginRotation
            target: orbitOrigin
            property: "rotation"
            duration: 420
            easing.type: Easing.OutCubic
        }

        NumberAnimation {
            id: homeOrbitOriginDistanceAnim
            to: root.defaultCameraDistance
            target: camera
            property: "z"
            duration: 420
            easing.type: Easing.OutCubic
        }
    }

    // view border is inset on top of content
    Rectangle {
        anchors.fill: view
        color: "transparent"
        border.width: root.showBorder ? 1 : 0
        border.color: root.borderColor
        z: 99
        enabled: false
    }
}
