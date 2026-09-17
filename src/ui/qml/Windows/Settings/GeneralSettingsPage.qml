import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Pointcaster 1.0

SettingsPage {
    id: root

    title: "General"

    SettingsSection {
        title: "Startup"

        SettingsRow {
            label: "Restore previous workspace on startup"

            CheckBox {
                checked: AppSettings.restoreLastWorkspace
                onToggled: AppSettings.restoreLastWorkspace = checked
            }
        }

        SettingsRow {
            label: "Last workspace"

            Label {
                text: AppSettings.lastWorkspacePath.length > 0 ? AppSettings.lastWorkspacePath : "None"
                font: Scaling.monoFont
                color: ThemeColors.readOnlyText
                elide: Text.ElideLeft
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
                Layout.maximumWidth: Math.round(260 * Scaling.uiScale)
            }
        }

        SettingsRow {
            label: "Attempt to load malformed workspace files"

            CheckBox {
                checked: AppSettings.loadMalformedWorkspaces
                onToggled: AppSettings.loadMalformedWorkspaces = checked
            }
        }
    }


}
