/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtTest
import org.kde.okular as Okular
import org.kde.okular.app

TestCase {
    id: testCase
    name: "ReadingHistory"
    when: windowShown
    Component {
        id: documentComponent
        Okular.DocumentItem {}
    }
    Component {
        id: bookshelfComponent
        BookshelfPage {}
    }

    function test_newDocumentItemResumesAndUpdatesBookshelf() {
        const first = createTemporaryObject(documentComponent, testCase);
        verify(first !== null);
        first.url = testDocumentUrl;
        tryCompare(first, "opened", true);
        wait(300);
        first.currentPage = 15;
        first.saveReadingProgress();
        first.destroy();
        const second = createTemporaryObject(documentComponent, testCase);
        second.url = testDocumentUrl;
        tryCompare(second, "opened", true);
        tryCompare(second, "currentPage", 15);
        tryVerify(() => second.readingHistory.books.some(book => book.url.toString() === testDocumentUrl.toString() && book.page === 15));
    }

    function test_mainResumesLastPage() {
        const component = Qt.createComponent(testMainUrl);
        compare(component.status, Component.Ready, component.errorString());
        const window = createTemporaryObject(component, null, {width: 360, height: 900});
        verify(window !== null);
        const document = findChild(window, "mobileDocument");
        verify(document !== null);
        document.url = testDocumentUrl;
        tryCompare(document, "opened", true);
        wait(300);
        document.currentPage = 20;
        document.url = "";
        document.url = testDocumentUrl;
        tryCompare(document, "opened", true);
        tryCompare(document, "currentPage", 20);
    }

    function test_bookshelfLoadsFromLocalHistory() {
        const document = createTemporaryObject(documentComponent, testCase);
        document.url = testDocumentUrl;
        tryCompare(document, "opened", true);
        wait(300);
        document.currentPage = 12;
        document.saveReadingProgress();
        const shelf = createTemporaryObject(bookshelfComponent, testCase, {document: document});
        verify(shelf !== null);
        tryVerify(() => shelf.books.some(book => book.url.toString() === testDocumentUrl.toString() && book.page === 12));
        document.url = "";
        document.url = shelf.books.find(book => book.url.toString() === testDocumentUrl.toString()).url;
        tryCompare(document, "opened", true);
        tryCompare(document, "currentPage", 12);
    }

}
