/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtTest 1.2
import "../../mobile/components/private"

Item {
    width: 400
    height: 600
    QQC2.ScrollView {
        id: viewport
        anchors.fill: parent
        clip: true
        Flickable {
            id: flick
            width: viewport.availableWidth
            height: viewport.availableHeight
            contentWidth: width
            contentHeight: height
            interactive: false
            MouseArea {
                width: flick.contentWidth
                height: flick.contentHeight
                Rectangle {
                    id: page
                    anchors.fill: parent
                    color: "#203040"
                }
                SelectionMagnifier {
                    id: lens
                    parent: viewport
                    visible: false
                    sourceItem: page
                    focusPoint: Qt.point(200, 300)
                }
            }
        }
    }
    TestCase {
        name: "SelectionViewport"
        when: windowShown
        function test_pageSurvivesMagnifier() {
            const originalWidth = page.width
            const originalHeight = page.height
            const before = grabImage(viewport)
            const dpr = before.width / viewport.width
            const sampleX = Math.round(200 * dpr)
            const sampleY = Math.round(400 * dpr)
            compare(before.pixel(sampleX, sampleY), Qt.rgba(32 / 255, 48 / 255, 64 / 255, 1))
            lens.visible = true
            verify(waitForRendering(lens))
            compare(page.width, originalWidth)
            compare(page.height, originalHeight)
            let during = grabImage(viewport)
            compare(during.pixel(sampleX, sampleY), before.pixel(sampleX, sampleY))
            lens.visible = false
            const after = grabImage(viewport)
            compare(after.pixel(sampleX, sampleY), before.pixel(sampleX, sampleY))
        }
    }
}
