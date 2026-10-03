/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick 2.15
import QtQuick.Window 2.15

Item {
    id: root
    property Item sourceItem
    property point focusPoint
    readonly property real zoom: 2
    readonly property point viewportPoint: sourceItem ? sourceItem.mapToItem(parent, focusPoint.x, focusPoint.y) : Qt.point(0, 0)

    width: Math.min(180, Math.max(0, parent.width - 16))
    height: 90
    x: Math.max(8, Math.min(parent.width - width - 8, viewportPoint.x - width / 2))
    // Stay clear of the finger; near the top edge, show the lens below it.
    y: Math.max(8, Math.min(parent.height - height - 8,
                           viewportPoint.y - height - 52 >= 8 ? viewportPoint.y - height - 52 : viewportPoint.y + 52))

    Rectangle {
        anchors.fill: parent
        radius: 12
        color: "white"
        border.color: "#4a90e2"
        border.width: 2
    }
    ShaderEffectSource {
        anchors.fill: parent
        anchors.margins: 8
        sourceItem: root.visible ? root.sourceItem : null
        sourceRect: Qt.rect(root.focusPoint.x - width / root.zoom / 2,
                            root.focusPoint.y - height / root.zoom / 2,
                            width / root.zoom, height / root.zoom)
        textureSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        live: true
    }
}
