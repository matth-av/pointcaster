import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Pointcaster 1.0

SettingsPage {
    id: root

    title: "Diagnostics"

    SettingsSection {
        title: "Logging"

        SettingsRow {
            label: "Log level"

            ComboBox {
                id: logLevelCombo

                font: Scaling.uiFont
                textRole: "text"
                valueRole: "value"
                Layout.preferredWidth: Math.round(160 * Scaling.uiScale)

                model: [
                    {
                        text: "Trace",
                        value: AppSettings.Trace
                    },
                    {
                        text: "Debug",
                        value: AppSettings.Debug
                    },
                    {
                        text: "Info",
                        value: AppSettings.Info
                    },
                    {
                        text: "Warn",
                        value: AppSettings.Warn
                    },
                    {
                        text: "Error",
                        value: AppSettings.Error
                    },
                    {
                        text: "Critical",
                        value: AppSettings.Critical
                    },
                    {
                        text: "Off",
                        value: AppSettings.Off
                    }
                ]

                Component.onCompleted: currentIndex = indexOfValue(AppSettings.logLevel)

                onActivated: {
                    const v = currentValue;
                    if (v !== undefined && v !== null)
                        AppSettings.logLevel = v;
                }

                Connections {
                    target: AppSettings
                    function onLogLevelChanged() {
                        const idx = logLevelCombo.indexOfValue(AppSettings.logLevel);
                        if (idx >= 0 && idx !== logLevelCombo.currentIndex)
                            logLevelCombo.currentIndex = idx;
                    }
                }
            }
        }

        SettingsRow {
            label: "Write log to file"

            CheckBox {
                checked: AppSettings.logToFile
                onToggled: AppSettings.logToFile = checked
            }
        }
    }

    SettingsSection {
        title: "Prometheus"
        description: "Prometheus can be used to gather time series data from running installations."

        SettingsRow {
            label: "Metrics endpoint"

            CheckBox {
                checked: !!AppSettings.enablePrometheusMetrics
                onToggled: AppSettings.enablePrometheusMetrics = checked
            }
        }

        SettingsRow {
            label: "Address"

            TextField {
                id: addressField

                font: Scaling.monoFont
                placeholderText: "127.0.0.1:8080"
                Layout.preferredWidth: Math.round(180 * Scaling.uiScale)

                property bool isValid: true

                color: isValid ? ThemeColors.text : ThemeColors.red

                Component.onCompleted: {
                    text = String(AppSettings.prometheusAddress ?? "127.0.0.1:8080");
                    validate(text, false);
                }

                function validate(address, commit) {
                    isValid = NetUtils.isValidHostAddress(address);
                    if (!commit)
                        return;
                    if (isValid) {
                        AppSettings.prometheusAddress = address;
                    } else {
                        // revert to last saved value
                        text = String(AppSettings.prometheusAddress ?? "127.0.0.1:8080");
                        isValid = NetUtils.isValidHostAddress(text);
                    }
                }

                onTextEdited: validate(text, false)
                onEditingFinished: validate(text, true)
            }
        }
    }

    SettingsSection {
        title: "Tracy"
        description: "Tracy can profile performance and help locate slowdowns in a Pointcaster workspace."

        SettingsRow {
            label: "Profiling endpoint"
            description: "Requires restart."

            CheckBox {
                checked: !!AppSettings.enableTracyProfiling
                onToggled: AppSettings.enableTracyProfiling = checked
            }
        }

        SettingsRow {
            label: "System tracing"
            description: "Enable call stack sampling and thread scheduling to the profile. This can cause stutters itself. Requires restart."
            enabled: !!AppSettings.enableTracyProfiling

            CheckBox {
                checked: !!AppSettings.enableTracySystemTracing
                onToggled: AppSettings.enableTracySystemTracing = checked
            }
        }
    }
}
