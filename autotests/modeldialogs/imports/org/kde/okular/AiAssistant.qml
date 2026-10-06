/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick

QtObject {
    property string status: ""
    property string profileModel: "sol"
    property string lastSavedModel: ""

    function profile(index) {
        return index < 0 ? {} : {name: "My model", model: profileModel, chatGptAccountId: "account", vision: true};
    }
    function saveChatGptProfile(index, fields) {
        lastSavedModel = fields.model;
        profileModel = fields.model;
        return true;
    }
    function removeProfile(index) {}

    property QtObject chatGpt: QtObject {
        property var accounts: [{id: "account", label: "Test account"}]
        property var models: []
        property var nextModels: [{id: "astra", name: "Astra"}, {id: "sol", name: "Sol"}]
        property string modelAccountId: "account"
        property string status: ""
        property bool busy: false
        signal firstPlanUse()

        function isConnected(id) { return true; }
        function loadModels(id) {
            models = [];
            Qt.callLater(function() { models = nextModels; });
        }
        function signIn(id) {}
        function signOut(id) {}
        function cancel() {}
    }
}
