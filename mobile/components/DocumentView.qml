/*
    SPDX-FileCopyrightText: 2015 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.okular 2.0
import "private"

/**
 * A touchscreen optimized, continuous view for a document.
 * Pages fit the available width and support flicking, pinch zoom and text selection.
 */
QQC2.ScrollView {
    id: root
    property DocumentItem document
    readonly property PageItem page: flick.currentItem ? flick.currentItem.pageItem : null
    property real zoomFactor: 1
    property PageItem selectionPage: null
    readonly property bool hasSelection: selectionPage !== null && selectionPage.hasSelection
    property bool positioning: false
    property Item selectionInput: null
    property point selectionFocus
    readonly property bool selectingText: (selectionInput !== null && selectionInput.longPressSelecting) ||
                                          (selectionOverlayLoader.item !== null && selectionOverlayLoader.item.handlePressed)
    onSelectionPageChanged: DictionaryLookup.clear()

    signal clicked
    signal urlOpened

    clip: true
    padding: 0

    SelectionLookupTimer {
        id: dictionaryTimer
        word: root.selectionPage ? root.selectionPage.selectedWord : ""
        enabled: DictionaryLookup.autoLookupEnabled
        selecting: root.selectingText || (root.selectionInput !== null && root.selectionInput.pressed)
        onLookupRequested: word => DictionaryLookup.lookup(word)
    }
    Connections {
        target: root.selectionPage
        function onSelectionChanged() {
            DictionaryLookup.clear()
        }
    }
    Connections {
        target: DictionaryLookup
        function onDictionaryFileChanged() {
            dictionaryTimer.restartWhenReady()
        }
    }

    function clearSelection() {
        dictionaryTimer.stop();
        DictionaryLookup.clear();
        selectionInput = null;
        if (selectionPage) {
            selectionPage.clearSelection();
            selectionPage = null;
        }
    }

    function positionCurrentPage() {
        if (!document || !document.opened || flick.count === 0) {
            return;
        }
        positioning = true;
        clearSelection();
        flick.currentIndex = document.currentPage;
        flick.positionViewAtIndex(document.currentPage, ListView.Beginning);
        positioning = false;
    }

    function updateCurrentPage() {
        if (positioning || !document || !document.opened || pinchHandler.active) {
            return;
        }
        const index = flick.indexAt(flick.contentX + flick.width / 2, flick.contentY + flick.height / 2);
        if (index >= 0) {
            flick.currentIndex = index;
            if (document.currentPage !== index) {
                positioning = true;
                document.currentPage = index;
                positioning = false;
            }
        }
    }

    function zoomAt(factor, center, previousCenter = center) {
        const newZoom = Math.max(1, Math.min(3, factor));
        if (newZoom === zoomFactor && center.x === previousCenter.x && center.y === previousCenter.y) {
            return;
        }
        clearSelection();
        const anchorY = flick.contentY + previousCenter.y;
        const anchorX = flick.contentX + previousCenter.x;
        const item = flick.itemAt(anchorX, anchorY) || flick.itemAt(anchorX, anchorY - flick.spacing);
        const relativeY = item ? Math.min(1, (anchorY - item.y) / item.height) : 0;
        // The space between pages stays fixed when the pinch center is in a gap.
        const gapOffset = item ? Math.max(0, anchorY - item.y - item.height) : 0;
        const relativeX = (flick.contentX + previousCenter.x) / zoomFactor;
        positioning = true;
        zoomFactor = newZoom;
        flick.forceLayout();
        flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, relativeX * zoomFactor - center.x));
        if (item) {
            flick.contentY = item.y + relativeY * item.height + gapOffset - center.y;
        }
        flick.returnToBounds();
        positioning = false;
        updateCurrentPage();
    }

    Component.onCompleted: Qt.callLater(positionCurrentPage)

    Connections {
        target: root.document

        function onUrlChanged() {
            root.clearSelection();
            root.zoomFactor = 1;
            flick.contentX = 0;
            Qt.callLater(root.positionCurrentPage);
            root.urlOpened();
        }

        function onPageCountChanged() {
            Qt.callLater(root.positionCurrentPage);
        }

        function onCurrentPageChanged() {
            if (!root.positioning) {
                root.positionCurrentPage();
            }
        }
    }

    SelectionMagnifier {
        parent: root
        z: 10
        sourceItem: root.selectionPage
        focusPoint: root.selectionFocus
        visible: root.hasSelection && root.selectingText
    }

    contentItem: ListView {
        id: flick
        model: root.document ? root.document.pageCount : 0
        contentWidth: width * root.zoomFactor
        flickableDirection: Flickable.AutoFlickDirection
        boundsBehavior: Flickable.StopAtBounds
        spacing: 4
        cacheBuffer: height
        highlightFollowsCurrentItem: false
        interactive: !pinchHandler.active && !root.hasSelection

        onContentYChanged: root.updateCurrentPage()
        onMovementStarted: root.clearSelection()
        onMovementEnded: root.updateCurrentPage()
        onWidthChanged: Qt.callLater(root.positionCurrentPage)

        delegate: PageView {
            id: pageDelegate
            required property int index
            width: flick.contentWidth
            height: width / pageRatio
            document: root.document
            pageNumber: index

            MouseArea {
                id: mouseArea
                anchors.fill: parent
                property bool longPressSelecting: false
                property bool suppressClick: false
                property bool longPressExtended: false
                property bool dragStartHandle: false
                property point longPressOrigin
                pressAndHoldInterval: 450
                preventStealing: longPressSelecting

                onPressed: {
                    longPressSelecting = false;
                    longPressExtended = false;
                    suppressClick = root.hasSelection;
                    root.clearSelection();
                    root.selectionInput = mouseArea;
                }
                onPressAndHold: mouse => {
                    root.clearSelection();
                    const pos = mapToItem(pageDelegate.pageItem, mouse.x, mouse.y);
                    longPressSelecting = pageDelegate.pageItem.selectWordAt(pos.x, pos.y);
                    longPressOrigin = pos;
                    root.selectionFocus = pos;
                    root.selectionInput = mouseArea;
                    if (longPressSelecting) {
                        root.selectionPage = pageDelegate.pageItem;
                        suppressClick = true;
                    }
                }
                onPositionChanged: mouse => {
                    if (longPressSelecting) {
                        const pos = mapToItem(pageDelegate.pageItem, mouse.x, mouse.y);
                        const dx = pos.x - longPressOrigin.x;
                        const dy = pos.y - longPressOrigin.y;
                        // Finger jitter must not shrink the initially selected word.
                        if (!longPressExtended) {
                            if (dx * dx + dy * dy < 12 * 12) {
                                return;
                            }
                            dragStartHandle = dy < -8 || (Math.abs(dy) <= 8 && dx < 0);
                            longPressExtended = true;
                        }
                        pageDelegate.pageItem.moveSelectionHandle(dragStartHandle, pos.x, pos.y);
                        root.selectionFocus = dragStartHandle ? pageDelegate.pageItem.selectionStart : pageDelegate.pageItem.selectionEnd;
                    }
                }
                onReleased: longPressSelecting = false
                onCanceled: longPressSelecting = false
                onClicked: {
                    if (suppressClick) {
                        suppressClick = false;
                    } else if (root.hasSelection) {
                        root.clearSelection();
                    } else {
                        root.clicked();
                    }
                }
                onDoubleClicked: mouse => {
                    const pos = mapToItem(flick, mouse.x, mouse.y);
                    root.zoomAt(1, pos);
                }
                onWheel: wheel => {
                    if (wheel.modifiers & Qt.ControlModifier) {
                        const pos = mapToItem(flick, wheel.x, wheel.y);
                        root.zoomAt(root.zoomFactor * Math.pow(1.2, wheel.angleDelta.y / 120), pos);
                    } else {
                        wheel.accepted = false;
                    }
                }
            }
        }

        PinchHandler {
            id: pinchHandler
            parent: flick
            target: null
            // Take both points before Flickable locks a cross-page drag to one of them.
            dragThreshold: 0
            property real initialZoom
            property point previousCenter

            onActiveChanged: {
                if (active) {
                    root.clearSelection();
                    flick.cancelFlick();
                    initialZoom = root.zoomFactor;
                    previousCenter = parent.mapToItem(flick, centroid.position);
                } else {
                    root.updateCurrentPage();
                }
            }
            onUpdated: {
                if (active) {
                    const center = parent.mapToItem(flick, centroid.position);
                    root.zoomAt(initialZoom * activeScale, center, previousCenter);
                    previousCenter = center;
                }
            }
        }

        Loader {
            id: selectionOverlayLoader
            parent: flick
            anchors.fill: parent
            z: 5
            active: root.hasSelection
            sourceComponent: Item {
                id: selectionOverlay
                anchors.fill: parent
                readonly property bool handlePressed: startHandleMouse.pressed || endHandleMouse.pressed
                QQC2.ToolBar {
                    id: selectionMenu
                    z: 5
                    visible: root.hasSelection && !root.selectingText
                    width: Math.min(selectionOverlay.width - 16, Math.max(280, selectionActions.implicitWidth))
                    x: Math.max(0, Math.min(selectionOverlay.width - width, root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionStart.x, root.selectionPage.selectionStart.y).x - width / 2))
                    y: Math.max(0, root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionStart.x, root.selectionPage.selectionStart.y).y - height - 12)

                    contentItem: Column {
                        spacing: 4
                        Row {
                            id: selectionActions
                            QQC2.ToolButton {
                                text: i18n("Copy")
                                enabled: root.selectionPage.canCopySelection
                                onClicked: root.selectionPage.copySelection()
                            }
                            QQC2.ToolButton {
                                text: i18n("Highlight")
                                enabled: root.selectionPage.canHighlightSelection
                                onClicked: root.selectionPage.highlightSelection()
                            }
                            QQC2.ToolButton {
                                text: i18n("Look up")
                                enabled: !!root.selectionPage.selectedWord
                                onClicked: DictionaryLookup.retry(root.selectionPage.selectedWord)
                            }
                        }
                        QQC2.Label {
                            width: parent.width
                            visible: !!DictionaryLookup.word && DictionaryLookup.word === root.selectionPage.selectedWord &&
                                     (DictionaryLookup.loading || !!DictionaryLookup.definition || !!DictionaryLookup.error)
                            text: DictionaryLookup.loading ? i18n("Looking up %1…", DictionaryLookup.word) :
                                  DictionaryLookup.word + "\n" + (DictionaryLookup.definition || DictionaryLookup.error)
                            wrapMode: Text.WordWrap
                            maximumLineCount: 8
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                    }
                }

                Item {
                    id: startHandle
                    z: 5
                    visible: root.selectionPage.hasSelection
                    width: 40
                    height: 40
                    x: root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionStart.x, root.selectionPage.selectionStart.y).x - width / 2
                    y: root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionStart.x, root.selectionPage.selectionStart.y).y - height / 2

                    Rectangle {
                        width: 18
                        height: 18
                        radius: 9
                        color: "#4a90e2"
                        border.color: "white"
                        border.width: 2
                        anchors.centerIn: parent
                    }
                    MouseArea {
                        id: startHandleMouse
                        anchors.fill: parent
                        preventStealing: true
                        property point grabOffset
                        onPressed: mouse => {
                            const pos = mapToItem(root.selectionPage, mouse.x, mouse.y);
                            grabOffset = Qt.point(pos.x - root.selectionPage.selectionStart.x, pos.y - root.selectionPage.selectionStart.y);
                            root.selectionFocus = root.selectionPage.selectionStart;
                        }
                        onPositionChanged: mouse => {
                            if (!pressed) {
                                return;
                            }
                            const pos = mapToItem(root.selectionPage, mouse.x, mouse.y);
                            root.selectionPage.moveSelectionHandle(true, pos.x - grabOffset.x, pos.y - grabOffset.y);
                            root.selectionFocus = root.selectionPage.selectionStart;
                        }
                    }
                }

                Item {
                    id: endHandle
                    z: 5
                    visible: root.selectionPage.hasSelection
                    width: 40
                    height: 40
                    x: root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionEnd.x, root.selectionPage.selectionEnd.y).x - width / 2
                    y: root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionEnd.x, root.selectionPage.selectionEnd.y).y - height / 2

                    Rectangle {
                        width: 18
                        height: 18
                        radius: 9
                        color: "#4a90e2"
                        border.color: "white"
                        border.width: 2
                        anchors.centerIn: parent
                    }
                    MouseArea {
                        id: endHandleMouse
                        anchors.fill: parent
                        preventStealing: true
                        property point grabOffset
                        onPressed: mouse => {
                            const pos = mapToItem(root.selectionPage, mouse.x, mouse.y);
                            grabOffset = Qt.point(pos.x - root.selectionPage.selectionEnd.x, pos.y - root.selectionPage.selectionEnd.y);
                            root.selectionFocus = root.selectionPage.selectionEnd;
                        }
                        onPositionChanged: mouse => {
                            if (!pressed) {
                                return;
                            }
                            const pos = mapToItem(root.selectionPage, mouse.x, mouse.y);
                            root.selectionPage.moveSelectionHandle(false, pos.x - grabOffset.x, pos.y - grabOffset.y);
                            root.selectionFocus = root.selectionPage.selectionEnd;
                        }
                    }
                }
            }
        }
    }
}
