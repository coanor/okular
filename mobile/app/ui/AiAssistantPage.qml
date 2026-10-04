/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.okular as Okular
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: root
    required property Okular.AiAssistant assistant
    required property Okular.DocumentItem document
    title: i18n("AI Reading Assistant")

    ColumnLayout {
        anchors.fill: parent
        RowLayout {
            Layout.fillWidth: true
            QQC2.ComboBox {
                Layout.fillWidth: true
                model: root.assistant.profiles
                textRole: "name"
                currentIndex: root.assistant.currentProfile
                enabled: !root.assistant.busy
                onActivated: index => root.assistant.currentProfile = index
            }
            QQC2.Button {
                text: i18n("Models…")
                icon.name: "configure"
                enabled: !root.assistant.busy
                onClicked: modelsMenu.popup()
                QQC2.Menu {
                    id: modelsMenu
                    QQC2.MenuItem { text: i18n("Add model…"); onTriggered: modelDialog.edit(-1) }
                    QQC2.MenuItem {
                        text: i18n("Continue with ChatGPT…")
                        enabled: root.assistant.chatGpt.available
                        onTriggered: chatGptDialog.edit(-1)
                    }
                    QQC2.MenuItem {
                        text: i18n("Edit model…")
                        enabled: root.assistant.currentProfile >= 0
                        onTriggered: {
                            const index = root.assistant.currentProfile;
                            if (root.assistant.profile(index).chatGptAccountId) {
                                chatGptDialog.edit(index);
                            } else {
                                modelDialog.edit(index);
                            }
                        }
                    }
                    QQC2.MenuItem {
                        text: i18n("Start new conversation")
                        enabled: root.assistant.ready && root.assistant.currentProfile >= 0 && root.assistant.messages.length > 0
                        onTriggered: clearDialog.open()
                    }
                }
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            text: root.document.opened ? i18n("Page content is sent to the selected provider when you press Ask.") :
                  i18n("Configure an AI model, then open a document to ask questions.")
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !!root.assistant.profile(root.assistant.currentProfile).chatGptAccountId
            QQC2.Label { Layout.fillWidth: true; text: i18n("Using ChatGPT plan") }
            QQC2.Button { text: i18n("Manage usage"); onClicked: Qt.openUrlExternally("https://chatgpt.com/settings/usage") }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !!root.assistant.selection
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Selected text: %1", root.assistant.selection)
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            QQC2.Button {
                text: i18n("Clear")
                enabled: !root.assistant.busy
                onClicked: root.assistant.setSelection("", -1)
            }
        }
        QQC2.ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ListView {
                id: history
                model: root.assistant.messages
                spacing: Kirigami.Units.largeSpacing
                delegate: ColumnLayout {
                    required property var modelData
                    width: history.width
                    QQC2.Label {
                        text: modelData.role === "user" ? i18n("You · Page %1", modelData.page) : i18n("Assistant")
                        font.bold: true
                    }
                    QQC2.TextArea {
                        objectName: "aiMessage-" + modelData.role
                        Layout.fillWidth: true
                        text: modelData.role === "assistant" ? root.assistant.renderMarkdown(modelData.content, font) : modelData.content
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        textFormat: modelData.role === "assistant" ? TextEdit.RichText : TextEdit.PlainText
                        background: null
                        onLinkActivated: link => {
                            if (/^https?:\/\//i.test(link)) {
                                Qt.openUrlExternally(link);
                            }
                        }
                    }
                }
                onCountChanged: Qt.callLater(() => history.positionViewAtEnd())
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            visible: root.assistant.busy || !!root.assistant.status
            text: root.assistant.status || i18n("Waiting for the model…")
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                QQC2.TextArea {
                    id: prompt
                    text: root.assistant.question
                    onTextChanged: root.assistant.question = text
                    placeholderText: root.document.opened ? i18n("Ask about this page…") : i18n("Open a document to ask questions.")
                    readOnly: root.assistant.busy || !root.document.opened
                    wrapMode: TextEdit.Wrap
                    textFormat: TextEdit.PlainText
                }
            }
            QQC2.Button {
                text: root.assistant.busy ? i18n("Cancel") : i18n("Ask")
                enabled: root.assistant.busy || (root.assistant.ready && root.assistant.currentProfile >= 0 && !!prompt.text.trim())
                onClicked: root.assistant.busy ? root.assistant.cancel() : root.assistant.ask()
            }
        }
    }

    Connections {
        target: root.document
        function onOpenedChanged() {
            if (root.document.opened) {
                root.assistant.activate();
            }
        }
    }

    AiModelDialog { id: modelDialog; assistant: root.assistant }
    ChatGptModelDialog { id: chatGptDialog; assistant: root.assistant }
    QQC2.Dialog {
        id: clearDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 440)
        title: i18n("Start new conversation")
        modal: true
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
        contentItem: QQC2.Label {
            text: i18n("Remove this conversation from the app?")
            wrapMode: Text.WordWrap
        }
        onAccepted: root.assistant.clearConversation()
    }
}
