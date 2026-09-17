import QtQuick
import QtQuick3D
import QtQuick3D.Helpers
import QtQuick.Controls
import QtQuick.Layouts
import Pointcaster 1.0

Item {
    id: root

    required property QtObject primaryAdapter
    required property QtObject secondaryAdapter
    required property QtObject alignmentController

    // 0 = "color" (PointCloudMaterial), >1 = solid color index
    property int primaryColorMode: 0
    property int secondaryColorMode: 1

    readonly property var solidColors: [ThemeColors.greyBlue, ThemeColors.red, ThemeColors.green, ThemeColors.blue, ThemeColors.yellow]
    readonly property var solidColorNames: ["Grey", "Red", "Green", "Blue", "Yellow"]

    View3D {
        id: view
        anchors.fill: parent
        camera: cameraNode

        // primary cloud with regular color
        Model {
            visible: root.primaryColorMode === 0
            geometry: primaryGeo
            materials: [
                PointCloudMaterial {
                    uPointSize: viewController.shaderPointSize
                    uViewportHeight: viewController.shaderViewportHeight
                }
            ]
        }

        // primary cloud with solid color
        Model {
            visible: root.primaryColorMode !== 0
            geometry: primaryGeo
            materials: [
                PointCloudMaterial {
                    fragmentShader: "qrc:/qt/qml/Pointcaster/PointCloud/Shaders/PointSolid.frag"
                    uPointSize: viewController.shaderPointSize
                    uViewportHeight: viewController.shaderViewportHeight
                    property color uColor: root.primaryColorMode > 0 ? root.solidColors[root.primaryColorMode - 1] : "white"
                }
            ]
        }

        PointCloudGeometry {
            id: primaryGeo
            pointCloudAdapter: root.primaryAdapter
        }

        // secondary cloud is wrapped in a node that controls its transformation based on the result of the alignment controller
        Node {
            id: secondaryTransformNode
            position: root.alignmentController.resultPosition
            rotation: root.alignmentController.resultRotation

            // secondary cloud with regular color
            Model {
                visible: root.secondaryColorMode === 0
                geometry: secondaryGeo
                materials: [
                    PointCloudMaterial {
                        uPointSize: viewController.shaderPointSize
                        uViewportHeight: viewController.shaderViewportHeight
                    }
                ]
            }

            // secondary cloud with solid color
            Model {
                visible: root.secondaryColorMode !== 0
                geometry: secondaryGeo
                materials: [
                    PointCloudMaterial {
                        fragmentShader: "qrc:/qt/qml/Pointcaster/PointCloud/Shaders/PointSolid.frag"
                        uPointSize: viewController.shaderPointSize
                        uViewportHeight: viewController.shaderViewportHeight
                        property color uColor: root.secondaryColorMode > 0 ? root.solidColors[root.secondaryColorMode - 1] : "white"
                    }
                ]
            }
        }

        PointCloudGeometry {
            id: secondaryGeo
            pointCloudAdapter: root.secondaryAdapter
        }

        Node {
            id: orbitOrigin

            PerspectiveCamera {
                id: cameraNode
                z: 300
            }
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

    // material picker
    Column {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: Math.round(Scaling.uiScale * 8)
        spacing: Math.round(Scaling.uiScale * 8)

        // colour picker
        Rectangle {
            width: colourCol.implicitWidth + Math.round(Scaling.uiScale * 16)
            height: colourCol.implicitHeight + Math.round(Scaling.uiScale * 16)
            radius: Math.round(Scaling.uiScale * 6)
            color: ThemeColors.withAlpha(ThemeColors.dark, 0.85)

            Column {
                id: colourCol
                anchors.centerIn: parent
                spacing: Math.round(Scaling.uiScale * 8)

                ColorModeSelector {
                    label: "Primary"
                    currentMode: root.primaryColorMode
                    onModeSelected: mode => root.primaryColorMode = mode
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: ThemeColors.middark
                }

                ColorModeSelector {
                    label: "Secondary"
                    currentMode: root.secondaryColorMode
                    onModeSelected: mode => root.secondaryColorMode = mode
                }
            }
        }

        // transform output
        Rectangle {
            id: transformPanel
            visible: root.alignmentController.hasResult
            width: transformCol.implicitWidth + Math.round(Scaling.uiScale * 16)
            height: transformCol.implicitHeight + Math.round(Scaling.uiScale * 16)
            radius: Math.round(Scaling.uiScale * 6)
            color: ThemeColors.withAlpha(ThemeColors.dark, 0.85)

            Column {
                id: transformCol
                anchors.centerIn: parent
                spacing: Math.round(Scaling.uiScale * 8)

                // 4x4 matrix
                Column {
                    spacing: Math.round(Scaling.uiScale * 2)

                    Text {
                        text: "Matrix"
                        font: Scaling.uiSmallFont
                        color: ThemeColors.midlight
                    }

                    TextEdit {
                        readOnly: true
                        selectByMouse: true
                        selectionColor: ThemeColors.highlight
                        selectedTextColor: ThemeColors.text
                        font: Scaling.monoFont
                        color: ThemeColors.text
                        text: root.alignmentController.transformString
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: ThemeColors.middark
                }

                // position
                Column {
                    spacing: Math.round(Scaling.uiScale * 2)

                    Text {
                        text: "Position"
                        font: Scaling.uiSmallFont
                        color: ThemeColors.midlight
                    }

                    TextEdit {
                        readOnly: true
                        selectByMouse: true
                        selectionColor: ThemeColors.highlight
                        selectedTextColor: ThemeColors.text
                        font: Scaling.monoFont
                        color: ThemeColors.text
                        text: {
                            var p = root.alignmentController.resultPosition;
                            return (p.x * 0.1).toFixed(3) + ", " + (p.y * 0.1).toFixed(3) + ", " + (p.z * 0.1).toFixed(3);
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: ThemeColors.middark
                }

                // rotation (quaternion)
                Column {
                    spacing: Math.round(Scaling.uiScale * 2)

                    Text {
                        text: "Quaternion (w, x, y, z)"
                        font: Scaling.uiSmallFont
                        color: ThemeColors.midlight
                    }

                    TextEdit {
                        readOnly: true
                        selectByMouse: true
                        selectionColor: ThemeColors.highlight
                        selectedTextColor: ThemeColors.text
                        font: Scaling.monoFont
                        color: ThemeColors.text
                        text: {
                            var q = root.alignmentController.resultRotation;
                            return q.scalar.toFixed(6) + ", " + q.x.toFixed(6) + ", " + q.y.toFixed(6) + ", " + q.z.toFixed(6);
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: ThemeColors.middark
                }

                // rotation (euler)
                Column {
                    spacing: Math.round(Scaling.uiScale * 2)

                    Text {
                        text: "Euler (pitch, yaw, roll)"
                        font: Scaling.uiSmallFont
                        color: ThemeColors.midlight
                    }

                    TextEdit {
                        readOnly: true
                        selectByMouse: true
                        selectionColor: ThemeColors.highlight
                        selectedTextColor: ThemeColors.text
                        font: Scaling.monoFont
                        color: ThemeColors.text
                        text: {
                            var e = root.alignmentController.resultRotation.toEulerAngles();
                            return e.x.toFixed(4) + ", " + e.y.toFixed(4) + ", " + e.z.toFixed(4);
                        }
                    }
                }

                // fitness
                Text {
                    visible: root.alignmentController.fitnessScore > 0
                    text: "fitness: " + root.alignmentController.fitnessScore.toFixed(6)
                    font: Scaling.uiSmallFont
                    color: ThemeColors.midlight
                }
            }
        }
    }

    // colour mode selector component

    component ColorModeSelector: Column {
        property string label: ""
        property int currentMode: 0
        signal modeSelected(int mode)

        spacing: Math.round(Scaling.uiScale * 4)

        Text {
            text: label
            font: Scaling.uiSmallFont
            color: ThemeColors.midlight
        }

        Row {
            spacing: Math.round(Scaling.uiScale * 4)

            Rectangle {
                width: Math.round(Scaling.uiScale * 36)
                height: Math.round(Scaling.uiScale * 20)
                radius: Math.round(Scaling.uiScale * 3)
                border.width: 1
                border.color: currentMode === 0 ? ThemeColors.highlight : ThemeColors.middark
                color: "transparent"
                opacity: rgbMouseArea.containsMouse ? 1 : currentMode === 0 ? 0.8 : 0.4

                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop {
                        position: 0.0
                        color: ThemeColors.red
                    }
                    GradientStop {
                        position: 0.5
                        color: ThemeColors.green
                    }
                    GradientStop {
                        position: 1.0
                        color: ThemeColors.blue
                    }
                }

                Text {
                    anchors.centerIn: parent
                    text: "RGB"
                    font.pointSize: Scaling.smallPointSize
                    font.weight: Font.Bold
                    color: "white"
                    style: Text.Outline
                    styleColor: Qt.rgba(0, 0, 0, 0.6)
                }

                MouseArea {
                    id: rgbMouseArea
                    anchors.fill: parent
                    onClicked: modeSelected(0)
                    hoverEnabled: true
                }
            }

            Repeater {
                model: root.solidColors

                Rectangle {
                    width: Math.round(Scaling.uiScale * 20)
                    height: Math.round(Scaling.uiScale * 20)
                    radius: Math.round(Scaling.uiScale * 3)
                    color: modelData
                    border.width: 1
                    border.color: currentMode === index + 1 ? ThemeColors.highlight : ThemeColors.middark
                    opacity: mouseArea.containsMouse ? 1 : currentMode === index + 1 ? 0.8 : 0.4

                    MouseArea {
                        id: mouseArea
                        anchors.fill: parent
                        onClicked: modeSelected(index + 1)
                        hoverEnabled: true
                    }
                }
            }
        }
    }

    // refining overlay

    Rectangle {
        anchors.fill: parent
        visible: root.alignmentController.refining
        color: ThemeColors.withAlpha(ThemeColors.shadow, 0.6)

        Column {
            anchors.centerIn: parent
            spacing: Math.round(Scaling.uiScale * 12)

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                running: root.alignmentController.refining
                palette.dark: ThemeColors.highlight
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Refining alignment..."
                font: Scaling.uiFont
                color: ThemeColors.text
            }
        }
    }

    function activate() {
        primaryGeo.setStaticData(root.alignmentController.primaryRenderData, root.alignmentController.primaryRenderStride, root.alignmentController.primaryBoundsMin, root.alignmentController.primaryBoundsMax);
        secondaryGeo.setStaticData(root.alignmentController.secondaryRenderData, root.alignmentController.secondaryRenderStride, root.alignmentController.secondaryBoundsMin, root.alignmentController.secondaryBoundsMax);
        orbitOrigin.position = primaryGeo.boundsCenter;
    }
}
