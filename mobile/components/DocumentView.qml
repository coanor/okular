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

    signal clicked
    signal urlOpened

    clip: true
    padding: 0

    function clearSelection() {
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
        if (positioning || !document || !document.opened || pinchArea.pinch.active) {
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

    function zoomAt(factor, center) {
        const newZoom = Math.max(1, Math.min(3, factor));
        if (newZoom === zoomFactor) {
            return;
        }
        clearSelection();
        const item = flick.itemAt(flick.contentX + center.x, flick.contentY + center.y);
        const relativeY = item ? (flick.contentY + center.y - item.y) / item.height : 0;
        const relativeX = (flick.contentX + center.x) / zoomFactor;
        positioning = true;
        zoomFactor = newZoom;
        flick.forceLayout();
        flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, relativeX * zoomFactor - center.x));
        if (item) {
            flick.contentY = item.y + relativeY * item.height - center.y;
        }
        flick.returnToBounds();
        positioning = false;
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

    contentItem: ListView {
        id: flick
        model: root.document ? root.document.pageCount : 0
        contentWidth: width * root.zoomFactor
        flickableDirection: Flickable.AutoFlickDirection
        boundsBehavior: Flickable.StopAtBounds
        spacing: 4
        cacheBuffer: height
        highlightFollowsCurrentItem: false
        interactive: !pinchArea.pinch.active && !root.hasSelection

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
                preventStealing: longPressSelecting

                onPressed: {
                    longPressSelecting = false;
                    suppressClick = root.hasSelection;
                    root.clearSelection();
                }
                onPressAndHold: mouse => {
                    root.clearSelection();
                    const pos = mapToItem(pageDelegate.pageItem, mouse.x, mouse.y);
                    longPressSelecting = pageDelegate.pageItem.selectWordAt(pos.x, pos.y);
                    if (longPressSelecting) {
                        root.selectionPage = pageDelegate.pageItem;
                        suppressClick = true;
                    }
                }
                onPositionChanged: mouse => {
                    if (longPressSelecting) {
                        const pos = mapToItem(pageDelegate.pageItem, mouse.x, mouse.y);
                        pageDelegate.pageItem.moveSelectionHandle(false, pos.x, pos.y);
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

        PinchArea {
            id: pinchArea
            parent: flick
            anchors.fill: parent
            property real initialZoom

            onPinchStarted: {
                root.clearSelection();
                initialZoom = root.zoomFactor;
            }
            onPinchUpdated: pinch => root.zoomAt(initialZoom * pinch.scale, pinch.center)
            onPinchFinished: {
                flick.returnToBounds();
                root.updateCurrentPage();
            }
        }

        Loader {
            parent: flick
            anchors.fill: parent
            z: 5
            active: root.hasSelection
            sourceComponent: Item {
                id: selectionOverlay
                anchors.fill: parent
                QQC2.ToolBar {
                    id: selectionMenu
                    z: 5
                    visible: root.selectionPage.hasSelection
                    x: Math.max(0, Math.min(selectionOverlay.width - width, root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionStart.x, root.selectionPage.selectionStart.y).x - width / 2))
                    y: Math.max(0, root.selectionPage.mapToItem(selectionOverlay, root.selectionPage.selectionStart.x, root.selectionPage.selectionStart.y).y - height - 12)

                    contentItem: Row {
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
                        anchors.fill: parent
                        onPositionChanged: mouse => {
                            var pos = mapToItem(root.selectionPage, mouse.x, mouse.y);
                            root.selectionPage.moveSelectionHandle(true, pos.x, pos.y);
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
                        anchors.fill: parent
                        onPositionChanged: mouse => {
                            var pos = mapToItem(root.selectionPage, mouse.x, mouse.y);
                            root.selectionPage.moveSelectionHandle(false, pos.x, pos.y);
                        }
                    }
                }
            }
        }
    }
}
