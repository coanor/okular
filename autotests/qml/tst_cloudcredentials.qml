/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtTest
import org.kde.okular as Okular

TestCase {
    id: testCase
    name: "CloudCredentials"
    when: windowShown

    Component { id: libraryComponent; Okular.CloudLibrary {} }
    property var library

    function cleanup() {
        if (library) {
            tryCompare(library, "busy", false);
            library.forgetCredentials();
            tryCompare(library, "busy", false);
            library = null;
        }
    }

    function test_androidCredentialsPersistAndClear() {
        const first = createTemporaryObject(libraryComponent, testCase);
        tryCompare(first, "busy", false);
        if (!first.credentialStorageAvailable) {
            skip("Android Keystore is required");
        }
        library = first;
        first.configure({bucket: "test-bucket", region: "us-east-1", accessKeyId: "test-access-key"}, "test-secret", "test-session-token", true);
        tryCompare(first, "busy", false);
        compare(first.error, "");
        verify(first.configured);
        const restored = createTemporaryObject(libraryComponent, testCase);
        tryCompare(restored, "busy", false);
        compare(restored.error, "");
        verify(restored.configured);
        verify(restored.settings.remember);
        compare(restored.settings.accessKeyId, "test-access-key");
        verify(restored.settings.secretAccessKey === undefined);
        verify(restored.settings.sessionToken === undefined);
        restored.forgetCredentials();
        tryCompare(restored, "busy", false);
        compare(restored.error, "");
        verify(!restored.configured);
        const forgotten = createTemporaryObject(libraryComponent, testCase);
        tryCompare(forgotten, "busy", false);
        verify(!forgotten.configured);
        verify(!forgotten.settings.remember);
    }
}
