/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtTest
import org.kde.okular 2.0 as Okular

TestCase {
    id: testCase
    name: "ChatGptModelDialog"
    width: 640
    height: 900
    when: windowShown
    property var dialog
    property var assistant
    function i18n(text) { return text; }
    Component { id: assistantComponent; Okular.AiAssistant {} }

    function init() {
        assistant = createTemporaryObject(assistantComponent, testCase);
        const component = Qt.createComponent(testDialogUrl);
        compare(component.status, Component.Ready, component.errorString());
        dialog = createTemporaryObject(component, testCase, {assistant: assistant});
        verify(dialog !== null);
    }
    function cleanup() {
        dialog.destroy();
        wait(0);
        dialog = null;
        assistant = null;
    }
    function picker() { return findChild(dialog, "chatGptModelPicker"); }
    function saveButton() { return findChild(dialog, "saveChatGptModel"); }

    function test_editAndSaveKeepsModel() {
        dialog.edit(0);
        tryCompare(picker(), "currentValue", "sol");
        compare(assistant.profileModel, "sol");
        verify(saveButton().enabled);
        saveButton().clicked();
        compare(assistant.lastSavedModel, "sol");
        dialog.edit(0);
        tryCompare(picker(), "currentValue", "sol");
    }

    function test_refreshKeepsExplicitSelection() {
        assistant.chatGpt.nextModels = [{id: "astra", name: "Astra"}, {id: "sol", name: "Sol"}, {id: "luna", name: "Luna"}];
        dialog.edit(0);
        tryCompare(picker(), "currentValue", "sol");
        picker().currentIndex = 2;
        picker().activated(2);
        assistant.chatGpt.loadModels("account");
        tryCompare(picker(), "currentValue", "luna");
        saveButton().clicked();
        compare(assistant.lastSavedModel, "luna");
    }

    function test_reorderedListWithSameCountKeepsModel() {
        dialog.edit(0);
        tryCompare(picker(), "currentValue", "sol");
        assistant.chatGpt.models = [{id: "sol", name: "Sol"}, {id: "astra", name: "Astra"}];
        tryCompare(picker(), "currentValue", "sol");
    }

    function test_missingSavedModelRequiresSelection() {
        assistant.chatGpt.nextModels = [{id: "astra", name: "Astra"}];
        dialog.edit(0);
        tryCompare(picker(), "count", 1);
        tryCompare(picker(), "currentIndex", -1);
        verify(!saveButton().enabled);
        compare(assistant.profileModel, "sol");
    }

    function test_newProfileUsesFirstAvailableModel() {
        dialog.edit(-1);
        tryCompare(picker(), "currentValue", "astra");
        verify(saveButton().enabled);
    }
}
