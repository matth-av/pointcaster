import QtQuick
import QtQuick3D
import QtQuick3D.Helpers
import Pointcaster 1.0

Item {
    id: root

    required property QtObject deviceAdapter
    required property bool live
    required property bool enablePicking
    required property bool active
    property var markerPositions: []
    property int pairCount: 0

    signal picked(vector3d position)

    property int _lastMouseX: 0
    property int _lastMouseY: 0

    // index-encoded view for picking
    View3D {
        id: pickView
        anchors.fill: parent
        renderMode: View3D.Offscreen
        visible: root.enablePicking

        Model {
            geometry: geo
            materials: [
                PointIndexMaterial {
                    uPointSize: viewController.shaderPointSize
                    uViewportHeight: viewController.shaderViewportHeight
                }
            ]
        }

        PerspectiveCamera {
            id: pickCamera
            position: cameraNode.scenePosition
            rotation: cameraNode.sceneRotation
            fieldOfView: cameraNode.fieldOfView
            clipNear: cameraNode.clipNear
            clipFar: cameraNode.clipFar
        }

        environment: SceneEnvironment {
            clearColor: Qt.rgba(1, 1, 1, 1)
            backgroundMode: SceneEnvironment.Color
            antialiasingMode: SceneEnvironment.NoAA
            tonemapMode: SceneEnvironment.TonemapModeNone
        }
    }

    // regular color point cloud view
    View3D {
        id: cloudView
        anchors.fill: parent
        camera: cameraNode

        Model {
            geometry: PointCloudGeometry {
                id: geo
                pointCloudAdapter: root.deviceAdapter
            }
            materials: [
                PointCloudMaterial {
                    uPointSize: viewController.shaderPointSize
                    uViewportHeight: viewController.shaderViewportHeight
                }
            ]
        }

        Node {
            id: orbitOrigin
            position: geo.boundsCenter

            PerspectiveCamera {
                id: cameraNode
                z: 150
            }
        }

        Node {
            id: markerContainer
        }

        // hover cursor
        Model {
            id: hoverMarker
            source: "#Sphere"
            visible: false
            scale: Qt.vector3d(0.02, 0.02, 0.02)
            materials: [
                PrincipledMaterial {
                    baseColor: ThemeColors.highlight
                    lighting: PrincipledMaterial.NoLighting
                }
            ]
        }

        OrbitViewController {
            id: viewController
            anchors.fill: parent
            camera: cameraNode
            origin: orbitOrigin
        }

        environment: SceneEnvironment {
            clearColor: ThemeColors.shadow
            backgroundMode: SceneEnvironment.Color
        }
    }

    // completed pair markers
    Item {
        anchors.fill: cloudView

        Repeater {
            model: root.pairCount

            Rectangle {
                property vector3d vp: {
                    // reference these to rebind when camera moves
                    cameraNode.scenePosition;
                    cameraNode.sceneRotation;
                    if (index >= root.markerPositions.length)
                        return Qt.vector3d(0, 0, -1);
                    return cameraNode.mapToViewport(root.markerPositions[index]);
                }

                x: vp.x * parent.width - width / 2
                y: vp.y * parent.height - height - Math.round(6 * Scaling.uiScale)
                visible: vp.z > 0

                width: label.implicitWidth + Math.round(8 * Scaling.uiScale)
                height: label.implicitHeight + Math.round(4 * Scaling.uiScale)
                radius: Math.round(3 * Scaling.uiScale)
                color: Qt.rgba(0, 0, 0, 0.7)

                Text {
                    id: label
                    anchors.centerIn: parent
                    text: (index + 1).toString()
                    font: Scaling.monoFont
                    color: "white"
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    onMarkerPositionsChanged: rebuildMarkers()

    function rebuildMarkers() {
        for (var i = markerContainer.children.length - 1; i >= 0; --i)
            markerContainer.children[i].destroy();

        for (var j = 0; j < markerPositions.length; ++j) {
            markerComponent.createObject(markerContainer, {
                position: markerPositions[j]
            });
        }
    }

    Component {
        id: markerComponent

        Model {
            source: "#Sphere"
            scale: Qt.vector3d(0.02, 0.02, 0.02)
            materials: [
                PrincipledMaterial {
                    baseColor: ThemeColors.red
                    lighting: PrincipledMaterial.NoLighting
                }
            ]
        }
    }

    Connections {
        target: root.deviceAdapter
        function onPointCloudUpdated() {
            if (root.live)
                geo.updateGeometry();
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: root.active && root.enablePicking
        propagateComposedEvents: true

        onPositionChanged: mouse => {
            if (!root.active || !root.enablePicking)
                return;
            root._lastMouseX = Math.round(mouse.x);
            root._lastMouseY = Math.round(mouse.y);
            hoverThrottle.restart();
        }

        onDoubleClicked: mouse => {
            if (!root.active || !root.enablePicking)
                return;
            hoverThrottle.stop();
            hoverMarker.visible = false;
            clickPicker.pick(pickView, Math.round(mouse.x), Math.round(mouse.y));
        }

        onExited: {
            hoverMarker.visible = false;
            hoverThrottle.stop();
        }
    }

    Timer {
        id: hoverThrottle
        interval: 15 // ms
        onTriggered: hoverPicker.pick(pickView, root._lastMouseX, root._lastMouseY)
    }

    ColorPicker {
        id: hoverPicker
        onPicked: {
            const index = ((hoverPicker.r << 24) | (hoverPicker.g << 16) | (hoverPicker.b << 8) | hoverPicker.a) >>> 0;
            if (index === 0xFFFFFFFF) {
                hoverMarker.visible = false;
                return;
            }
            hoverMarker.position = geo.pointPosition(index);
            hoverMarker.visible = true;
        }
    }

    ColorPicker {
        id: clickPicker
        onPicked: {
            const index = ((clickPicker.r << 24) | (clickPicker.g << 16) | (clickPicker.b << 8) | clickPicker.a) >>> 0;
            if (index === 0xFFFFFFFF)
                return;
            root.picked(geo.pointPosition(index));
        }
    }

    // active highlight border
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.width: root.active ? Math.round(Scaling.uiScale * 2) : 0
        border.color: ThemeColors.highlight
        visible: root.active
    }

    function snapshot() {
        geo.updateGeometry();
        orbitOrigin.position = geo.boundsCenter;
    }

    function reset() {
        geo.reset();
        orbitOrigin.position = {};
        markerPositions = [];
        hoverMarker.visible = false;
    }
}
