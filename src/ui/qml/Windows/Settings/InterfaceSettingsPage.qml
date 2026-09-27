import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import Pointcaster 1.0

SettingsPage {
    id: root

    title: "Interface"

    SettingsSection {
        title: "Application"

        SettingsRow {
            label: "UI scale"

            DragFloat {
                font: Scaling.uiFont
                Layout.preferredWidth: Math.round(110 * Scaling.uiScale)
                decimals: 2
                minValue: 0.5
                maxValue: 2.5
                defaultValue: 1.0
                boundValue: root.pendingUiScale
                onCommitValue: function (v) {
                    root.pendingUiScale = v;
                }
            }

            IconButton {
                text: "Apply"
                enabled: Math.abs(root.pendingUiScale - AppSettings.uiScale) > 1e-9
                onClicked: AppSettings.uiScale = root.pendingUiScale
            }
        }
    }

    SettingsSection {
        title: "Viewport"

        SettingsRow {
            label: "Background colour"

            TextField {
                id: backgroundColorField

                font: Scaling.monoFont
                text: AppSettings.backgroundColor
                Layout.preferredWidth: Math.round(110 * Scaling.uiScale)

                onEditingFinished: {
                    if (root.isValidColor(text))
                        AppSettings.backgroundColor = text.trim();
                    else
                        text = AppSettings.backgroundColor; // revert invalid input
                }

                Connections {
                    target: AppSettings
                    function onBackgroundColorChanged() {
                        backgroundColorField.text = AppSettings.backgroundColor;
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: Math.round(20 * Scaling.uiScale)
                Layout.preferredHeight: Math.round(20 * Scaling.uiScale)
                Layout.alignment: Qt.AlignVCenter
                radius: Math.round(3 * Scaling.uiScale)
                color: AppSettings.backgroundColor
                border.color: ThemeColors.mid
                border.width: 1
            }

            IconButton {
                text: "Select…"
                onClicked: {
                    backgroundColorDialog.selectedColor = AppSettings.backgroundColor;
                    backgroundColorDialog.open();
                }
            }
        }

        SettingsRow {
            label: "Antialiasing"

            ComboBox {
                id: antialiasingCombo

                font: Scaling.uiFont
                textRole: "text"
                valueRole: "value"
                Layout.preferredWidth: Math.round(160 * Scaling.uiScale)

                model: [
                    {
                        text: "None",
                        value: AppSettings.NoAA
                    },
                    {
                        text: "SSAA",
                        value: AppSettings.SSAA
                    },
                    {
                        text: "MSAA",
                        value: AppSettings.MSAA
                    },
                    {
                        text: "Progressive",
                        value: AppSettings.ProgressiveAA
                    }
                ]

                Component.onCompleted: currentIndex = indexOfValue(AppSettings.antialiasing)

                onActivated: {
                    const v = currentValue;
                    if (v !== undefined && v !== null)
                        AppSettings.antialiasing = v;
                }

                Connections {
                    target: AppSettings
                    function onAntialiasingChanged() {
                        const idx = antialiasingCombo.indexOfValue(AppSettings.antialiasing);
                        if (idx >= 0 && idx !== antialiasingCombo.currentIndex)
                            antialiasingCombo.currentIndex = idx;
                    }
                }
            }
        }

        SettingsRow {
            label: "Grid size (metres)"

            DragInt {
                font: Scaling.uiFont
                Layout.preferredWidth: Math.round(110 * Scaling.uiScale)
                minValue: 1
                maxValue: 30
                defaultValue: 10
                boundValue: AppSettings.gridSizeMetres
                onCommitValue: function (v) {
                    AppSettings.gridSizeMetres = v;
                }
            }
        }

        SettingsRow {
            label: "Point size (mm)"
            description: "Default size of points in the session view. Clouds with a scale attribute ignore this setting."

            DragInt {
                font: Scaling.uiFont
                Layout.preferredWidth: Math.round(110 * Scaling.uiScale)
                minValue: 1
                maxValue: 250
                defaultValue: 5
                boundValue: AppSettings.pointSize
                onCommitValue: function (v) {
                    AppSettings.pointSize = v;
                }
            }
        }
    }

    ColorDialog {
        id: backgroundColorDialog
        onAccepted: AppSettings.backgroundColor = selectedColor.toString()
    }

    // --- local pending state / helpers ---

    property real pendingUiScale: AppSettings.uiScale

    // Qt.color() yields an invalid colour for anything it cannot parse, which
    // covers both "#rrggbb" input and the named colours
    function isValidColor(text) {
        const trimmed = String(text).trim();
        if (trimmed.length === 0)
            return false;
        return Qt.color(trimmed).valid;
    }

    Connections {
        target: AppSettings
        function onUiScaleChanged() {
            // If something else changes the setting, keep the pending in sync.
            root.pendingUiScale = AppSettings.uiScale;
        }
    }
}
