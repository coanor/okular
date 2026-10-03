/*
    SPDX-FileCopyrightText: 2026 coanor <coanor@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtTest
import org.kde.okular as Okular

TestCase {
    id: testCase
    name: "DocumentView"
    width: 360
    height: 900
    visible: true
    when: windowShown

    Component {
        id: viewComponent
        Okular.DocumentView {
            width: testCase.width
            height: testCase.height
            document: Okular.DocumentItem {
                url: testDocumentUrl
            }
        }
    }

    Component {
        id: pageComponent
        Okular.PageItem {}
    }

    property var view

    SignalSpy {
        id: clickedSpy
        target: view
        signalName: "clicked"
    }

    function init() {
        view = createTemporaryObject(viewComponent, testCase);
        verify(view !== null);
        tryCompare(view.document, "opened", true);
        view.document.currentPage = 0;
        tryVerify(() => view.page !== null && view.page.height > 0);
        wait(300);
    }

    function pageDelegates(item) {
        let pages = [];
        if (item.pageItem !== undefined) {
            pages.push(item);
        }
        for (let child of item.children) {
            pages = pages.concat(pageDelegates(child));
        }
        return pages;
    }

    function visiblePages() {
        return pageDelegates(view).filter(item => {
            const pos = item.mapToItem(view, 0, 0);
            return item.visible && pos.y < view.height && pos.y + item.height > 0 && pos.x < view.width && pos.x + item.width > 0;
        });
    }

    function test_multiplePagesOnOneScreen() {
        const pages = visiblePages();
        const first = pages.find(item => item.pageNumber === 0);
        const second = pages.find(item => item.pageNumber === 1);
        verify(first !== undefined && second !== undefined, "The viewport should display adjacent pages together");
        const firstPos = first.mapToItem(view, 0, 0);
        const secondPos = second.mapToItem(view, 0, 0);
        verify(secondPos.y >= firstPos.y + first.height - 1, "Adjacent pages must not overlap");
        verify(secondPos.y <= firstPos.y + first.height + 20, "Pages should have only a small gap");
        compare(view.page.width, view.width);
        verify(pageDelegates(view).length < view.document.pageCount, "Pages outside the viewport should be loaded on demand");
    }

    function test_scrollAcrossPageBoundaryWithoutSnapping() {
        const touch = touchEvent(view);
        const firstPage = visiblePages()[0];
        touch.press(0, view, 180, 800).commit();
        for (let y = 750; y >= 200; y -= 50) {
            const before = firstPage.mapToItem(view, 0, 0).y;
            touch.move(0, view, 180, y).commit();
            wait(20);
            if (y <= 650) {
                const after = firstPage.mapToItem(view, 0, 0).y;
                fuzzyCompare(before - after, 50, 2, "Page boundaries must not interrupt scrolling");
            }
        }
        verify(view.document.currentPage > 0, "Scrolling should update the reading position");
        compare(view.page.pageNumber, view.document.currentPage);
        const trackedPage = visiblePages()[0];
        const before = trackedPage.mapToItem(view, 0, 0).y;
        touch.move(0, view, 180, 180).commit();
        wait(20);
        const after = trackedPage.mapToItem(view, 0, 0).y;
        fuzzyCompare(before - after, 20, 2, "Crossing pages must not snap the scroll position");
        touch.release(0, view, 180, 180).commit();
    }

    function test_externalPageNavigation() {
        view.document.currentPage = 20;
        tryVerify(() => view.page !== null && view.page.pageNumber === 20);
        tryVerify(() => visiblePages().some(item => item.pageNumber === 20));
    }

    function test_resizeKeepsPageWidth() {
        view.width = 600;
        tryCompare(view.page, "width", 600);
    }

    function test_pinchZoomAndReset() {
        const touch = touchEvent(view);
        touch.press(0, view, 140, 300).press(1, view, 220, 300).commit();
        for (let offset = 10; offset <= 60; offset += 10) {
            touch.move(0, view, 140 - offset, 300).move(1, view, 220 + offset, 300).commit();
            wait(20);
        }
        touch.release(0, view, 80, 300).release(1, view, 280, 300).commit();
        tryVerify(() => view.page.width > view.width, 5000, "Pinching should zoom the pages");
        mouseDoubleClickSequence(view, 180, 300);
        tryCompare(view.page, "width", view.width);
    }

    function test_touchTapTogglesControls() {
        const count = clickedSpy.count;
        const touch = touchEvent(view);
        touch.press(0, view, 180, 300).commit();
        touch.release(0, view, 180, 300).commit();
        tryCompare(clickedSpy, "count", count + 1);
    }

    function test_touchDoubleTapResetsZoom() {
        view.zoomAt(2, Qt.point(180, 300));
        const touch = touchEvent(view);
        touch.press(0, view, 180, 300).commit();
        touch.release(0, view, 180, 300).commit();
        wait(50);
        touch.press(0, view, 180, 300).commit();
        touch.release(0, view, 180, 300).commit();
        tryCompare(view, "zoomFactor", 1);
    }

    function test_touchLongPressAndDismissSelection() {
        const page = view.page;
        const word = page.mapToItem(view, page.width * 0.22, page.height * 0.162);
        const touch = touchEvent(view);
        touch.press(0, view, word.x, word.y).commit();
        wait(1000);
        touch.release(0, view, word.x, word.y).commit();
        verify(view.hasSelection, "Long pressing the fixture's first word should select it");
        const count = clickedSpy.count;
        touch.press(0, view, 180, 800).commit();
        for (let y = 750; y >= 400; y -= 50) {
            touch.move(0, view, 180, y).commit();
            wait(20);
        }
        verify(!view.hasSelection, "Dragging elsewhere should dismiss the selection");
        verify(view.contentItem.contentY > 100, "The same drag should scroll the document");
        touch.release(0, view, 180, 400).commit();
        compare(clickedSpy.count, count, "Dismissing a selection should not toggle controls");
    }

    function test_resetZoomUpdatesCurrentPage() {
        view.document.currentPage = 10;
        view.zoomAt(3, Qt.point(180, 450));
        mouseDoubleClickSequence(view, 180, 200);
        tryCompare(view, "zoomFactor", 1);
        const flick = view.contentItem;
        const index = flick.indexAt(flick.contentX + flick.width / 2, flick.contentY + flick.height / 2);
        compare(view.document.currentPage, index);
        compare(view.page.pageNumber, index);
    }

    function test_twoFingerPan() {
        view.document.currentPage = 10;
        view.zoomAt(2, Qt.point(180, 450));
        const touch = touchEvent(view);
        touch.press(0, view, 100, 500).press(1, view, 260, 500).commit();
        touch.move(0, view, 80, 500).move(1, view, 280, 500).commit();
        wait(20);
        const before = view.contentItem.contentY;
        const zoom = view.zoomFactor;
        for (let delta = 10; delta <= 140; delta += 10) {
            touch.move(0, view, 80, 500 - delta).move(1, view, 280, 500 - delta).commit();
            wait(20);
        }
        fuzzyCompare(view.contentItem.contentY - before, 140, 2, "Keeping the fingers apart should pan without changing zoom");
        fuzzyCompare(view.zoomFactor, zoom, 0.01);
        touch.release(0, view, 80, 360).release(1, view, 280, 360).commit();
    }

    function test_pinchAcrossPageBoundary() {
        const first = visiblePages().find(item => item.pageNumber === 0);
        const second = visiblePages().find(item => item.pageNumber === 1);
        const boundary = first.mapToItem(view, 0, first.height).y;
        const secondY = second.mapToItem(view, 0, 0).y;
        const center = boundary + 2;
        const touch = touchEvent(view);
        touch.press(0, view, 140, center - 30).press(1, view, 220, center + 30).commit();
        for (let delta = 10; delta <= 60; delta += 10) {
            touch.move(0, view, 140 - delta, center - 30 - delta).move(1, view, 220 + delta, center + 30 + delta).commit();
            wait(20);
        }
        verify(view.zoomFactor > 1.3, "A pinch should work when its fingers start on different pages");
        fuzzyCompare(second.mapToItem(view, 0, 0).y, secondY, 2, "Pinching around the gap should keep the page boundary in place");
        touch.release(0, view, 80, center - 90).release(1, view, 280, center + 90).commit();
    }

    function test_lastPageHasNoTrailingSpace() {
        view.document.currentPage = view.document.pageCount - 1;
        tryVerify(() => view.page !== null && view.page.pageNumber === view.document.pageCount - 1);
        const lastPage = visiblePages().find(item => item.pageNumber === view.document.pageCount - 1);
        verify(lastPage !== undefined);
        const pos = lastPage.mapToItem(view, 0, 0);
        fuzzyCompare(pos.y + lastPage.height, view.height, 2);
    }

    function test_closeDocument() {
        view.document.url = "";
        tryCompare(view.document, "opened", false);
        tryCompare(view, "page", null);
    }

    function test_detachedPageIgnoresPixmapUpdates() {
        const page = createTemporaryObject(pageComponent, view, {
            document: view.document,
            width: view.page.width,
            height: view.page.height
        });
        verify(page !== null);
        page.parent = null;
        view.zoomAt(2, Qt.point(180, 300));
        // Detached ListView delegates can still receive the shared observer's pixmap updates.
        wait(1000);
        compare(view.page.width, view.width * 2);
    }
}
