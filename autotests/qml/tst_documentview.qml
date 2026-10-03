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
                url: Qt.resolvedUrl("../data/simple-multipage.pdf")
            }
        }
    }

    property var view

    function init() {
        view = createTemporaryObject(viewComponent, testCase);
        verify(view !== null);
        tryCompare(view.document, "opened", true);
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
}
