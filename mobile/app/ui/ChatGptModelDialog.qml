/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.okular as Okular

QQC2.Dialog {
    id: root
    required property Okular.AiAssistant assistant
    readonly property var connection: assistant.chatGpt
    property int profileIndex: -1
    property string savedModel: ""
    readonly property string accountId: accountPicker.currentIndex >= 0 && accountPicker.currentIndex < connection.accounts.length ?
                                        connection.accounts[accountPicker.currentIndex].id : ""
    title: i18n("ChatGPT")
    modal: true
    focus: true
    anchors.centerIn: parent
    width: Math.min(parent.width - 24, 480)
    height: Math.min(parent.height - 24, implicitHeight)
    standardButtons: QQC2.Dialog.Close

    function edit(index) {
        profileIndex = index;
        const fields = assistant.profile(index);
        nameField.text = fields.name || "";
        savedModel = fields.model || "";
        visionField.checked = fields.vision || false;
        let selected = connection.accounts.length > 0 ? 0 : -1;
        for (let i = 0; i < connection.accounts.length; ++i) {
            if (connection.accounts[i].id === fields.chatGptAccountId) {
                selected = i;
            }
        }
        accountPicker.currentIndex = selected;
        open();
        if (accountId && connection.isConnected(accountId)) {
            connection.loadModels(accountId);
        }
    }

    contentItem: QQC2.ScrollView {
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            width: parent.width
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Connect your ChatGPT plan to use its available models. Requests share your existing plan limits.")
                wrapMode: Text.WordWrap
            }
            QQC2.Label { text: i18n("ChatGPT account"); visible: accountPicker.count > 0 }
            QQC2.ComboBox {
                id: accountPicker
                Layout.fillWidth: true
                visible: count > 0
                model: root.connection.accounts
                textRole: "label"
                enabled: !root.connection.busy
                onActivated: root.connection.loadModels(root.accountId)
            }
            QQC2.Button {
                Layout.fillWidth: true
                objectName: "continueWithChatGpt"
                text: i18n("Continue with ChatGPT")
                enabled: !root.connection.busy
                onClicked: root.connection.signIn(root.accountId)
            }
            RowLayout {
                visible: accountPicker.count > 0
                QQC2.Button {
                    text: i18n("Add account…")
                    enabled: !root.connection.busy
                    onClicked: root.connection.signIn("")
                }
                QQC2.Button {
                    text: i18n("Sign out")
                    enabled: !root.connection.busy && root.connection.isConnected(root.accountId)
                    onClicked: root.connection.signOut(root.accountId)
                }
            }
            QQC2.Label { text: i18n("Model") }
            QQC2.ComboBox {
                id: modelPicker
                Layout.fillWidth: true
                model: root.connection.modelAccountId === root.accountId ? root.connection.models : []
                textRole: "name"
                enabled: !root.connection.busy && count > 0
                onCountChanged: {
                    for (let i = 0; i < count; ++i) {
                        if (root.connection.models[i].id === root.savedModel) {
                            currentIndex = i;
                            break;
                        }
                    }
                }
            }
            QQC2.Button {
                text: i18n("Refresh models")
                enabled: !!root.accountId && !root.connection.busy && root.connection.isConnected(root.accountId)
                onClicked: root.connection.loadModels(root.accountId)
            }
            QQC2.Label { text: i18n("Name") }
            QQC2.TextField { id: nameField; Layout.fillWidth: true; placeholderText: i18n("ChatGPT") }
            QQC2.CheckBox { id: visionField; text: i18n("This model accepts page images") }
            QQC2.Label {
                Layout.fillWidth: true
                text: root.connection.status
                visible: !!text
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: root.assistant.status
                visible: !!text
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
            }
            QQC2.Button {
                text: i18n("Cancel")
                visible: root.connection.busy
                onClicked: root.connection.cancel()
            }
            QQC2.Button {
                text: i18n("Save")
                enabled: !root.connection.busy && modelPicker.currentIndex >= 0 && modelPicker.count > 0
                onClicked: {
                    const model = root.connection.models[modelPicker.currentIndex];
                    if (root.assistant.saveChatGptProfile(root.profileIndex, {
                        name: nameField.text, accountId: root.accountId,
                        model: model.id, vision: visionField.checked
                    })) {
                        root.close();
                    }
                }
            }
            QQC2.Button {
                text: i18n("Remove model")
                visible: root.profileIndex >= 0
                enabled: !root.connection.busy
                onClicked: {
                    root.assistant.removeProfile(root.profileIndex);
                    root.close();
                }
            }
            QQC2.Button { text: i18n("Manage ChatGPT usage"); onClicked: Qt.openUrlExternally("https://chatgpt.com/settings/usage") }
            QQC2.Button { text: i18n("Learn more"); onClicked: Qt.openUrlExternally("https://learn.chatgpt.com/docs/sign-in-with-chatgpt") }
        }
    }

    Connections {
        target: root.connection
        function onFirstPlanUse() { welcomeDialog.open(); }
        function onModelsChanged() {
            // A newly registered account may be added while the dialog is open.
            for (let i = 0; i < root.connection.accounts.length; ++i) {
                if (root.connection.accounts[i].id === root.connection.modelAccountId) {
                    accountPicker.currentIndex = i;
                    break;
                }
            }
        }
    }

    QQC2.Dialog {
        id: welcomeDialog
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 440)
        modal: true
        title: i18n("You're using your ChatGPT plan")
        contentItem: ColumnLayout {
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Eligible requests in Okular use your existing ChatGPT plan limits. You can manage this app's usage in ChatGPT Settings.")
                wrapMode: Text.WordWrap
            }
        }
        footer: QQC2.DialogButtonBox {
            QQC2.Button {
                text: i18n("Got it")
                QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.AcceptRole
            }
            onAccepted: welcomeDialog.close()
        }
    }
}
