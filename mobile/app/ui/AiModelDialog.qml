/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.okular as Okular
import org.kde.kirigami as Kirigami

QQC2.Dialog {
    id: root
    required property Okular.AiAssistant assistant
    property int profileIndex: -1
    property bool hasApiKey: false
    title: profileIndex < 0 ? i18n("Add AI model") : i18n("Edit AI model")
    modal: true
    focus: true
    anchors.centerIn: parent
    width: Math.min(parent.width - 24, 480)
    height: Math.min(parent.height - 24, implicitHeight)
    standardButtons: QQC2.Dialog.Cancel

    function edit(index) {
        profileIndex = index;
        const fields = assistant.profile(index);
        nameField.text = fields.name || "";
        kindField.currentIndex = fields.kind || 0;
        endpointField.text = fields.endpoint || "";
        modelField.text = fields.model || "";
        extraField.text = fields.extraArguments || "";
        visionField.checked = fields.vision === undefined ? true : fields.vision;
        hasApiKey = fields.hasApiKey || false;
        keyField.text = "";
        open();
    }

    onClosed: keyField.text = ""

    contentItem: QQC2.ScrollView {
        clip: true
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            QQC2.Label { text: i18n("Name") }
            QQC2.TextField { id: nameField; Layout.fillWidth: true }
            QQC2.Label { text: i18n("Protocol") }
            QQC2.ComboBox {
                id: kindField
                Layout.fillWidth: true
                model: [i18n("OpenAI-compatible Chat Completions"), i18n("OpenAI Responses"), i18n("Anthropic-compatible Messages")]
            }
            QQC2.Label { text: i18n("Base URL") }
            QQC2.TextField {
                id: endpointField
                Layout.fillWidth: true
                placeholderText: kindField.currentIndex === 2 ? "https://api.anthropic.com" : "https://api.openai.com/v1"
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
            }
            QQC2.Label { text: i18n("Model") }
            QQC2.TextField { id: modelField; Layout.fillWidth: true; inputMethodHints: Qt.ImhNoPredictiveText }
            QQC2.Label { text: i18n("API key") }
            Kirigami.PasswordField {
                id: keyField
                Layout.fillWidth: true
                placeholderText: root.hasApiKey ? i18n("Leave blank to keep the current key") : i18n("API key")
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("API keys use KWallet when available. Otherwise, enter the key again after restarting the app.")
                wrapMode: Text.WordWrap
            }
            QQC2.Label { text: i18n("Extra arguments (JSON)") }
            QQC2.TextArea {
                id: extraField
                Layout.fillWidth: true
                placeholderText: '{"temperature":0.2}'
                wrapMode: TextEdit.Wrap
                textFormat: TextEdit.PlainText
            }
            QQC2.CheckBox { id: visionField; text: i18n("This model accepts page images") }
            QQC2.Label {
                Layout.fillWidth: true
                visible: !!root.assistant.status
                text: root.assistant.status
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }
            RowLayout {
                QQC2.Button {
                    text: i18n("Save")
                    onClicked: {
                        if (root.assistant.saveProfile(root.profileIndex, {
                            name: nameField.text, kind: kindField.currentIndex,
                            endpoint: endpointField.text, model: modelField.text,
                            apiKey: keyField.text, extraArguments: extraField.text,
                            vision: visionField.checked
                        })) {
                            root.close();
                        }
                    }
                }
                QQC2.Button {
                    text: i18n("Remove")
                    visible: root.profileIndex >= 0
                    onClicked: {
                        root.assistant.removeProfile(root.profileIndex);
                        root.close();
                    }
                }
            }
        }
    }
}
