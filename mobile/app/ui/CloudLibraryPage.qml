/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as QQD
import org.kde.kirigami as Kirigami
import org.kde.okular as Okular

Kirigami.ScrollablePage {
    id: root
    required property Okular.CloudLibrary library
    signal openBook(url url)
    signal synchronizeRequested()
    signal resolveRequested(int conflictIndex, int variantIndex)
    title: i18n("Cloud Library")

    QQD.FileDialog {
        id: importDialog
        nameFilters: Okular.Okular.nameFilters
        onAccepted: root.library.importBook(selectedFile)
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            visible: !root.library.available || !!root.library.error
            type: Kirigami.MessageType.Error
            text: root.library.available ? root.library.error : i18n("This build does not include S3 support.")
        }
        RowLayout {
            QQC2.BusyIndicator { running: root.library.busy; visible: running }
            QQC2.Label {
                Layout.fillWidth: true
                text: root.library.status
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                Accessible.role: Accessible.StaticText
            }
        }
        Kirigami.Heading { text: i18n("S3 Connection"); level: 2 }
        Kirigami.FormLayout {
            Layout.fillWidth: true
            enabled: root.library.available && !root.library.busy
            QQC2.TextField {
                id: bucket
                Kirigami.FormData.label: i18n("Bucket:")
                text: root.library.settings.bucket || ""
            }
            QQC2.TextField {
                id: prefix
                Kirigami.FormData.label: i18n("Prefix:")
                text: root.library.settings.prefix || ""
                placeholderText: i18n("Optional")
            }
            QQC2.TextField {
                id: region
                Kirigami.FormData.label: i18n("Region:")
                text: root.library.settings.region || ""
                placeholderText: "us-east-1"
            }
            QQC2.TextField {
                id: endpoint
                Kirigami.FormData.label: i18n("Endpoint:")
                text: root.library.settings.endpoint || ""
                placeholderText: i18n("Leave empty for AWS S3")
                inputMethodHints: Qt.ImhUrlCharactersOnly
            }
            QQC2.TextField {
                id: accessKey
                Kirigami.FormData.label: i18n("Access key ID:")
                text: root.library.settings.accessKeyId || ""
                inputMethodHints: Qt.ImhNoPredictiveText
            }
            Kirigami.PasswordField {
                id: secretKey
                Kirigami.FormData.label: i18n("Secret access key:")
                placeholderText: root.library.configured ? i18n("Leave empty to keep the current key") : ""
            }
            Kirigami.PasswordField {
                id: sessionToken
                Kirigami.FormData.label: i18n("Session token:")
                placeholderText: i18n("Optional; enter again when changing settings")
            }
            QQC2.CheckBox {
                id: remember
                text: i18n("Remember credentials on this device")
                visible: root.library.credentialStorageAvailable
                checked: !!root.library.settings.remember
            }
            QQC2.Label {
                visible: !root.library.credentialStorageAvailable
                text: i18n("Credentials are kept only for this session.")
                wrapMode: Text.WordWrap
            }
            RowLayout {
                QQC2.Button {
                    text: i18n("Save connection")
                    onClicked: {
                        root.library.configure({bucket: bucket.text, prefix: prefix.text, region: region.text,
                            endpoint: endpoint.text, accessKeyId: accessKey.text}, secretKey.text, sessionToken.text, remember.checked)
                        secretKey.clear()
                        sessionToken.clear()
                    }
                }
                QQC2.Button {
                    text: i18n("Forget credentials")
                    enabled: root.library.configured || !!root.library.settings.remember
                    onClicked: root.library.forgetCredentials()
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.Button {
                text: i18n("Add book…")
                icon.name: "document-open"
                enabled: root.library.available && !root.library.busy
                onClicked: importDialog.open()
            }
            QQC2.Button {
                text: i18n("Sync now")
                icon.name: "view-refresh"
                enabled: root.library.available && root.library.configured && !root.library.busy
                onClicked: root.synchronizeRequested()
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: i18n("Books are copied to this app's private library. Originals are kept. Sync also exchanges PDF highlights and annotations.")
            wrapMode: Text.WordWrap
        }
        Kirigami.Heading { text: i18n("Books"); level: 2 }
        QQC2.Label { visible: root.library.books.length === 0; text: i18n("Add a book or sync to download your cloud library."); wrapMode: Text.WordWrap }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 14)
            clip: true
            model: root.library.books
            delegate: QQC2.ItemDelegate {
                required property var modelData
                width: ListView.view.width
                text: modelData.title
                enabled: !root.library.busy
                onClicked: root.openBook(modelData.url)
            }
            QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
        }
        Kirigami.Heading { text: i18n("Annotation conflicts"); level: 2; visible: root.library.conflicts.length > 0 }
        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 10)
            clip: true
            model: root.library.conflicts
            delegate: QQC2.ItemDelegate {
                required property int index
                required property var modelData
                width: ListView.view.width
                text: i18n("%1 — page %2", modelData.title, modelData.page + 1)
                enabled: !root.library.busy && root.library.configured
                onClicked: {
                    conflictDialog.conflictIndex = index
                    conflictDialog.variants = modelData.variants
                    conflictDialog.open()
                }
            }
            QQC2.ScrollBar.vertical: QQC2.ScrollBar {}
        }
    }

    QQC2.Dialog {
        id: conflictDialog
        property int conflictIndex: -1
        property var variants: []
        title: i18n("Choose Annotation Version")
        parent: QQC2.Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - Kirigami.Units.largeSpacing * 2, Kirigami.Units.gridUnit * 30)
        modal: true
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        contentItem: ColumnLayout {
            QQC2.ComboBox {
                id: variantChoice
                Layout.fillWidth: true
                model: conflictDialog.variants.map((variant, index) => variant.value ? i18n("Version %1 — %2", index + 1, variant.value.author || i18n("Unknown author")) : i18n("Deleted annotation"))
            }
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 12
                QQC2.TextArea {
                    readonly property var value: conflictDialog.variants[variantChoice.currentIndex]?.value
                    text: value ? (value.contents || i18n("Annotation without text")) : i18n("This version deletes the annotation.")
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                }
            }
        }
        onAccepted: root.resolveRequested(conflictIndex, variantChoice.currentIndex)
    }
}
