/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtTest
import org.kde.okular as Okular

TestCase {
    id: testCase
    name: "MobileAiAssistant"
    when: windowShown
    visible: true
    width: 360
    height: 900

    Component {
        id: documentComponent
        Okular.DocumentItem { url: testDocumentUrl }
    }
    property var document
    property var assistant

    function fields(vision) {
        return {name: "Mobile test", kind: 0, endpoint: aiTestServer.endpoint,
                model: "test-model", apiKey: "test-key", vision: vision, extraArguments: ""};
    }

    function init() {
        aiTestServer.reset();
        document = createTemporaryObject(documentComponent, testCase);
        verify(document.opened);
        assistant = document.aiAssistant;
        assistant.activate();
        tryCompare(assistant, "ready", true);
        verify(assistant.saveProfile(-1, fields(false)));
        assistant.clearConversation();
    }

    function cleanup() {
        assistant.cancel();
        tryCompare(assistant, "busy", false);
        assistant.clearConversation();
        assistant.removeProfile(assistant.currentProfile);
    }

    function test_noRequestUntilAskAndHistoryReload() {
        compare(aiTestServer.requestCount, 0);
        assistant.question = "Explain this page";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        compare(aiTestServer.requestCount, 1);
        compare(assistant.messages.length, 2);
        compare(assistant.messages[1].content, "test answer");
        compare(assistant.question, "");
        document.url = "";
        compare(assistant.ready, false);
        compare(assistant.messages.length, 0);
        document.url = testDocumentUrl;
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages.length, 2);
        compare(aiTestServer.requestCount, 1);
    }

    function test_configureBeforeOpeningDocument() {
        const emptyDocument = createTemporaryObject(documentComponent, testCase, {url: ""});
        verify(emptyDocument !== null && !emptyDocument.opened);
        const emptyAssistant = emptyDocument.aiAssistant;
        emptyAssistant.activate();
        compare(emptyAssistant.ready, false);
        compare(emptyAssistant.busy, false);
        const count = emptyAssistant.profiles.length;
        verify(emptyAssistant.saveProfile(-1, fields(false)));
        const index = emptyAssistant.currentProfile;
        try {
            compare(emptyAssistant.profiles.length, count + 1);
            const edited = fields(false);
            edited.name = "Configured before opening";
            verify(emptyAssistant.saveProfile(index, edited));
            compare(emptyAssistant.profile(index).name, edited.name);
            emptyAssistant.question = "Wait for a document";
            emptyAssistant.ask();
            compare(aiTestServer.requestCount, 0);
            compare(emptyAssistant.messages.length, 0);

            emptyDocument.url = testDocumentUrl;
            verify(emptyDocument.opened);
            emptyAssistant.activate();
            tryCompare(emptyAssistant, "ready", true);
            compare(emptyAssistant.currentProfile, index);
            emptyAssistant.question = "Use the configured model";
            emptyAssistant.ask();
            tryCompare(emptyAssistant, "busy", false);
            compare(aiTestServer.requestCount, 1);
            compare(emptyAssistant.messages.length, 2);
            emptyAssistant.clearConversation();

            emptyDocument.url = "";
            compare(emptyAssistant.ready, false);
            verify(emptyAssistant.saveProfile(index, fields(false)));
        } finally {
            emptyAssistant.cancel();
            tryCompare(emptyAssistant, "busy", false);
            emptyAssistant.clearConversation();
            emptyAssistant.removeProfile(index);
        }
        compare(emptyAssistant.profiles.length, count);
    }

    function test_chatGptConfigurationWithoutDocument() {
        const emptyDocument = createTemporaryObject(documentComponent, testCase, {url: ""});
        const emptyAssistant = emptyDocument.aiAssistant;
        const connection = emptyAssistant.chatGpt;
        verify(connection !== null);
        compare(connection.busy, false);
        verify(!connection.isConnected("oaiapp_unknown"));
        const count = emptyAssistant.profiles.length;
        verify(!emptyAssistant.saveChatGptProfile(-1, {
            name: "Unconnected ChatGPT", accountId: "oaiapp_unknown", model: "unknown-model"
        }));
        compare(emptyAssistant.profiles.length, count);
        compare(aiTestServer.requestCount, 0);
        for (let i = 0; i < connection.accounts.length; ++i) {
            verify(connection.accounts[i].access_token === undefined);
            verify(connection.accounts[i].refresh_token === undefined);
        }
    }

    function test_selectedPassageUsesItsPage() {
        const currentPage = document.currentPage;
        const selectedPage = currentPage === 1 ? 2 : 1;
        assistant.setSelection("A selected passage", selectedPage);
        assistant.question = "Explain the selection";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        const text = aiTestServer.lastRequest.messages[1].content[0].text;
        verify(text.indexOf("Current page: " + (selectedPage + 1)) >= 0);
        verify(text.indexOf("A selected passage") >= 0);
        compare(document.currentPage, currentPage);
        compare(assistant.messages[0].page, selectedPage + 1);
    }

    function test_cancelRestoresQuestionAndHistory() {
        aiTestServer.autoRespond = false;
        assistant.question = "Question to retry";
        assistant.ask();
        tryCompare(aiTestServer, "requestCount", 1);
        verify(assistant.busy);
        assistant.cancel();
        tryCompare(assistant, "busy", false);
        compare(assistant.question, "Question to retry");
        compare(assistant.messages.length, 0);
        aiTestServer.autoRespond = true;
        assistant.ask();
        tryCompare(assistant, "busy", false);
        compare(assistant.messages.length, 2);
    }

    function test_failureRestoresQuestion() {
        aiTestServer.failRequests = true;
        assistant.question = "Failed question";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        compare(assistant.question, "Failed question");
        compare(assistant.messages.length, 0);
        compare(assistant.status, "test failure");
    }

    function test_closeDuringRequestDoesNotReuseContext() {
        aiTestServer.autoRespond = false;
        assistant.setSelection("Old passage", 0);
        assistant.question = "Old question";
        assistant.ask();
        tryCompare(aiTestServer, "requestCount", 1);
        document.url = "";
        tryCompare(assistant, "busy", false);
        compare(assistant.ready, false);
        compare(assistant.selection, "");
        compare(assistant.question, "");
        compare(assistant.messages.length, 0);
        document.url = testDocumentUrl;
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages.length, 0);
    }

    function test_visionSendsImageOnlyOnAsk() {
        verify(assistant.saveProfile(assistant.currentProfile, fields(true)));
        compare(aiTestServer.requestCount, 0);
        assistant.question = "Explain the page image";
        assistant.ask();
        tryCompare(assistant, "busy", false, 30000);
        compare(aiTestServer.requestCount, 1);
        const content = aiTestServer.lastRequest.messages[1].content;
        compare(content.length, 2);
        compare(content[1].type, "image_url");
        verify(content[1].image_url.url.indexOf("data:image/jpeg;base64,") === 0);
    }

    function test_cancelVisionThenRetry() {
        verify(assistant.saveProfile(assistant.currentProfile, fields(true)));
        assistant.question = "Retry page image";
        assistant.ask();
        verify(assistant.busy);
        assistant.cancel();
        tryCompare(assistant, "busy", false);
        compare(assistant.question, "Retry page image");
        compare(assistant.messages.length, 0);
        assistant.ask();
        tryCompare(assistant, "busy", false, 30000);
        compare(aiTestServer.requestCount, 1);
        compare(assistant.messages.length, 2);
    }

    function test_invalidProfilesAndPerModelHistory() {
        const index = assistant.currentProfile;
        const count = assistant.profiles.length;
        const invalid = fields(false);
        invalid.extraArguments = "[1]";
        verify(!assistant.saveProfile(-1, invalid));
        compare(assistant.profiles.length, count);
        assistant.question = "First model";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        verify(assistant.saveProfile(-1, fields(false)));
        const second = assistant.currentProfile;
        compare(assistant.messages.length, 0);
        assistant.currentProfile = index;
        compare(assistant.messages.length, 2);
        assistant.clearConversation();
        assistant.removeProfile(second);
        assistant.currentProfile = index;
    }
}
