/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls as QQC2
import QtTest
import org.kde.okular as Okular
import "../../mobile/app/ui" as MobileUi

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
    Component {
        id: assistantPageComponent
        MobileUi.AiAssistantPage { anchors.fill: parent }
    }
    Component {
        id: clipboardComponent
        QQC2.TextArea { textFormat: TextEdit.PlainText }
    }
    property var document
    property var assistant
    property var assistantPage: null

    function fields(vision) {
        return {name: "Mobile test", kind: 0, endpoint: aiTestServer.endpoint,
                model: "test-model", apiKey: "test-key", vision: vision, extraArguments: ""};
    }

    function messageText(item) {
        // TextEdit.getText() retains paragraph and table frame separators.
        return item.getText(0, item.length).replace(/[\u2029\uFDD0\uFDD1]/g, "\n");
    }

    function init() {
        aiTestServer.reset();
        document = createTemporaryObject(documentComponent, testCase);
        verify(document.opened);
        assistant = document.aiAssistant;
        assistant.activate();
        tryCompare(assistant, "ready", true);
        verify(assistant.saveProfile(-1, fields(false)));
        tryCompare(assistant, "busy", false);
        assistant.clearConversation();
        tryCompare(assistant, "busy", false);
    }

    function cleanup() {
        if (assistantPage !== null) {
            assistantPage.destroy();
            assistantPage = null;
            wait(0);
        }
        assistant.cancel();
        tryCompare(assistant, "busy", false);
        assistant.clearConversation();
        tryCompare(assistant, "busy", false);
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

    function test_clearedHistoryStaysEmptyAfterReopening() {
        assistant.question = "Remember this question";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        compare(assistant.messages.length, 2);
        assistant.clearConversation();
        tryCompare(assistant, "busy", false);
        compare(assistant.messages.length, 0);
        document.url = "";
        document.url = testDocumentUrl;
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages.length, 0);
        compare(aiTestServer.requestCount, 1);
    }

    function test_closeWhileRestoringHistoryIgnoresPreviousDocument() {
        assistant.question = "Saved question";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        document.url = "";
        document.url = testDocumentUrl;
        assistant.activate();
        document.url = "";
        wait(100);
        compare(assistant.ready, false);
        compare(assistant.busy, false);
        compare(assistant.messages.length, 0);
        document.url = testDocumentUrl;
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages.length, 2);
        compare(aiTestServer.requestCount, 1);
    }

    function test_streamingProviderHistoryIsSeparateAndPersistent() {
        if (!aiTestFiles.supportsSourceUrls()) {
            skip("Source URI association requires Android");
        }
        const source = "content://org.kde.okular.test/books/first";
        document.url = aiTestFiles.pipeDescriptorUrl(source);
        verify(document.opened);
        assistant.activate();
        tryCompare(assistant, "ready", true);
        assistant.question = "Remember the first streaming book";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        const previousQuestion = assistant.messages[0].content;

        document.url = aiTestFiles.pipeDescriptorUrl("content://org.kde.okular.test/books/second");
        verify(document.opened);
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages.length, 0);

        document.url = aiTestFiles.pipeDescriptorUrl(source);
        verify(document.opened);
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages.length, 2);
        compare(assistant.messages[0].content, previousQuestion);
        compare(aiTestServer.requestCount, 1);
    }

    function test_replyRendersMarkdownAndHistory() {
        failOnWarning(/Binding loop/);
        assistantPage = assistantPageComponent.createObject(testCase, {
            assistant: assistant, document: document
        });
        const page = assistantPage;
        verify(page !== null);
        const question = "# Question\n**Keep my input literal**";
        const answer = "# Summary\n\n**Bold** and `inline code`.\n\n- First\n- Second\n\n"
                     + "```cpp\nreturn 42;\n```\n\n[Reference](https://example.com/)\n\n"
                     + "| Name | Value |\n| --- | --- |\n| Answer | 42 |";
        aiTestServer.responseText = answer;
        assistant.question = question;
        assistant.ask();
        tryCompare(assistant, "busy", false);
        compare(assistant.messages[1].content, answer);
        tryVerify(() => findChild(page, "aiMessage-assistant") !== null);
        let reply = findChild(page, "aiMessage-assistant");
        const rendered = messageText(reply);
        verify(rendered.indexOf("Summary") === 0, "The reply must show the heading without Markdown markers");
        verify(rendered.indexOf("Bold and inline code.") >= 0);
        verify(rendered.indexOf("return 42;") >= 0);
        verify(rendered.indexOf("```") < 0);
        verify(rendered.indexOf("[Reference]") < 0);
        verify(rendered.indexOf("| --- |") < 0);
        const userMessage = findChild(page, "aiMessage-user");
        verify(userMessage !== null);
        compare(messageText(userMessage), question);

        reply.selectAll();
        const clipboard = createTemporaryObject(clipboardComponent, testCase);
        reply.copy();
        clipboard.paste();
        compare(clipboard.text, rendered);

        document.url = "";
        document.url = testDocumentUrl;
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages[1].content, answer);
        tryVerify(() => findChild(page, "aiMessage-assistant") !== null);
        reply = findChild(page, "aiMessage-assistant");
        compare(messageText(reply), rendered);
        compare(aiTestServer.requestCount, 1);
    }

    function test_descriptorWithoutPathAccessCanAsk() {
        const url = aiTestFiles.restrictedDescriptorUrl();
        if (!url.toString()) {
            skip("File descriptors are unavailable on this platform");
        }
        const descriptorDocument = createTemporaryObject(documentComponent, testCase, {url: url});
        verify(descriptorDocument.opened);
        const descriptorAssistant = descriptorDocument.aiAssistant;
        verify(descriptorAssistant.saveProfile(-1, fields(false)));
        tryCompare(descriptorAssistant, "busy", false);
        const index = descriptorAssistant.currentProfile;
        try {
            descriptorAssistant.question = "Explain the descriptor document";
            descriptorAssistant.activate();
            tryCompare(descriptorAssistant, "busy", false);
            verify(descriptorAssistant.ready, "An opened descriptor document must enable Ask");
            descriptorAssistant.ask();
            tryCompare(descriptorAssistant, "busy", false);
            compare(aiTestServer.requestCount, 1);
            compare(descriptorAssistant.messages.length, 2);
            const reopenedDocument = createTemporaryObject(documentComponent, testCase, {
                url: aiTestFiles.restrictedDescriptorUrl()
            });
            verify(reopenedDocument.opened);
            const reopenedAssistant = reopenedDocument.aiAssistant;
            reopenedAssistant.currentProfile = index;
            reopenedAssistant.activate();
            tryCompare(reopenedAssistant, "ready", true);
            compare(reopenedAssistant.messages.length, 2);
            descriptorAssistant.clearConversation();
            tryCompare(descriptorAssistant, "busy", false);
        } finally {
            descriptorAssistant.cancel();
            descriptorAssistant.removeProfile(index);
        }
    }

    function test_pipeDescriptorCanAsk() {
        const url = aiTestFiles.pipeDescriptorUrl();
        if (!url.toString()) {
            skip("File descriptors are unavailable on this platform");
        }
        document.url = url;
        verify(document.opened);
        assistant.activate();
        tryCompare(assistant, "busy", false);
        verify(assistant.ready, "A readable streaming document must enable Ask");
        assistant.question = "Explain the streaming document";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        compare(assistant.messages.length, 2);
        compare(aiTestServer.requestCount, 1);
        assistant.clearConversation();
        tryCompare(assistant, "busy", false);
        assistant.question = "History follows the same book bytes";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        document.url = aiTestFiles.pipeDescriptorUrl();
        verify(document.opened);
        assistant.activate();
        tryCompare(assistant, "ready", true);
        compare(assistant.messages.length, 2, "Identical streamed bytes must restore the same book history");
    }

    function test_preservesDesktopProfiles() {
        const original = aiTestFiles.storedProfiles();
        const desktop = {id: "desktop-codex-test", name: "Desktop Codex", kind: 3,
                         model: "desktop-model", endpoint: "", vision: false,
                         extraArguments: "--config model_reasoning_effort=high"};
        aiTestFiles.setStoredProfiles([desktop].concat(original));
        try {
            const otherDocument = createTemporaryObject(documentComponent, testCase);
            const otherAssistant = otherDocument.aiAssistant;
            compare(otherAssistant.profiles.length, assistant.profiles.length);
            verify(otherAssistant.saveProfile(-1, fields(false)));
            tryCompare(otherAssistant, "busy", false);
            let saved = aiTestFiles.storedProfiles().find(profile => profile.id === desktop.id);
            verify(saved !== undefined, "Saving a mobile model must retain desktop-only profiles");
            compare(saved.extraArguments, desktop.extraArguments);
            compare(aiTestFiles.storedProfiles()[0].id, desktop.id, "Saving must preserve the desktop's default model");
            const edited = fields(false);
            edited.name = "Edited mobile profile";
            verify(otherAssistant.saveProfile(otherAssistant.currentProfile, edited));
            tryCompare(otherAssistant, "busy", false);
            saved = aiTestFiles.storedProfiles().find(profile => profile.id === desktop.id);
            verify(saved !== undefined, "Editing a mobile model must retain desktop-only profiles");
            compare(saved.name, desktop.name);
            compare(aiTestFiles.storedProfiles()[0].id, desktop.id);
            otherAssistant.removeProfile(otherAssistant.currentProfile);
            saved = aiTestFiles.storedProfiles().find(profile => profile.id === desktop.id);
            verify(saved !== undefined, "Removing a mobile model must retain desktop-only profiles");
            compare(saved.model, desktop.model);
            compare(aiTestFiles.storedProfiles()[0].id, desktop.id);
        } finally {
            aiTestFiles.setStoredProfiles(original);
        }
    }

    function test_replyDoesNotLoadImages_data() {
        const remote = aiTestServer.endpoint + "/image";
        return [
            {tag: "inline", answer: "![Preview](" + remote + ")"},
            {tag: "reference", answer: "![Preview][image]\n\n[image]: " + remote},
            {tag: "html", answer: '<img src="' + remote + '">'},
            {tag: "local", answer: "![Preview](<" + testDocumentUrl.toString() + ">)"},
            {tag: "data", answer: "![Preview](data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7)"}
        ];
    }

    function test_replyDoesNotLoadImages(data) {
        assistantPage = assistantPageComponent.createObject(testCase, {
            assistant: assistant, document: document
        });
        verify(assistantPage !== null);
        aiTestServer.responseText = data.answer;
        assistant.question = "Explain without fetching other resources";
        assistant.ask();
        tryCompare(assistant, "busy", false);
        tryVerify(() => findChild(assistantPage, "aiMessage-assistant") !== null);
        const reply = findChild(assistantPage, "aiMessage-assistant");
        wait(300);
        compare(aiTestServer.requestCount, 1, "Rendering a reply must not fetch remote images");
        verify(messageText(reply).indexOf("\uFFFC") < 0, "Reply images must become text instead of loading resources");
        compare(assistant.messages[1].content, data.answer);
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
        tryCompare(emptyAssistant, "busy", false);
        const index = emptyAssistant.currentProfile;
        try {
            compare(emptyAssistant.profiles.length, count + 1);
            const edited = fields(false);
            edited.name = "Configured before opening";
            verify(emptyAssistant.saveProfile(index, edited));
            tryCompare(emptyAssistant, "busy", false);
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
            tryCompare(emptyAssistant, "busy", false);

            emptyDocument.url = "";
            compare(emptyAssistant.ready, false);
            verify(emptyAssistant.saveProfile(index, fields(false)));
            tryCompare(emptyAssistant, "busy", false);
        } finally {
            emptyAssistant.cancel();
            tryCompare(emptyAssistant, "busy", false);
            emptyAssistant.clearConversation();
            tryCompare(emptyAssistant, "busy", false);
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
        tryCompare(assistant, "busy", false);
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
        tryCompare(assistant, "busy", false);
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
        tryCompare(assistant, "busy", false);
        const second = assistant.currentProfile;
        compare(assistant.messages.length, 0);
        assistant.currentProfile = index;
        tryCompare(assistant, "busy", false);
        compare(assistant.messages.length, 2);
        assistant.clearConversation();
        tryCompare(assistant, "busy", false);
        assistant.removeProfile(second);
        assistant.currentProfile = index;
        tryCompare(assistant, "busy", false);
    }
}
