/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick 2.15
import QtTest 1.2
import "../../mobile/components/private"

Item {
    width: 400
    height: 600

    Rectangle {
        id: page
        x: 40
        y: 40
        width: 320
        height: 500
        color: "white"
        Rectangle {
            id: highlight
            x: 120
            y: 180
            width: 50
            height: 20
            color: "#4a90e2"
        }
    }
    SelectionMagnifier {
        id: lens
        sourceItem: page
        focusPoint: Qt.point(145, 190)
    }
    TestCase {
        name: "SelectionMagnifier"
        when: windowShown

        function init() {
            failOnWarning(/.*/)
            lens.visible = true
            lens.focusPoint = Qt.point(145, 190)
            highlight.color = "#4a90e2"
        }
        function test_aboveFinger() {
            verify(lens.y + lens.height < lens.viewportPoint.y)
            verify(lens.x >= 0)
            verify(lens.x + lens.width <= 400)
        }
        function test_topEdge() {
            lens.focusPoint = Qt.point(5, 5)
            verify(lens.y > lens.viewportPoint.y)
            verify(lens.x >= 0)
            verify(lens.y + lens.height <= 600)
        }
        function test_bottomRightEdge() {
            lens.focusPoint = Qt.point(320, 500)
            verify(lens.x + lens.width <= 400)
            verify(lens.y + lens.height <= 600)
        }
        function test_liveSelection() {
            verify(waitForRendering(lens))
            let image = grabImage(lens)
            let cx = Math.floor(image.width / 2)
            let cy = Math.floor(image.height / 2)
            let dpr = image.width / lens.width
            compare(image.pixel(cx, cy), Qt.rgba(74 / 255, 144 / 255, 226 / 255, 1))
            // The 50-pixel selection is shown at twice its original width.
            compare(image.pixel(cx + Math.round(40 * dpr), cy), Qt.rgba(74 / 255, 144 / 255, 226 / 255, 1))
            compare(image.pixel(cx + Math.round(60 * dpr), cy), Qt.rgba(1, 1, 1, 1))
            highlight.color = "#ff0000"
            verify(waitForRendering(lens))
            image = grabImage(lens)
            compare(image.pixel(cx, cy), Qt.rgba(1, 0, 0, 1))
        }
    }
}
