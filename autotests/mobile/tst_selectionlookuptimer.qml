/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick 2.15
import QtTest 1.15
import "../../mobile/components/private"

TestCase {
    name: "SelectionLookupTimer"
    SelectionLookupTimer {
        id: timer
    }
    SignalSpy {
        id: lookupSpy
        target: timer
        signalName: "lookupRequested"
    }
    function init() {
        timer.enabled = false
        timer.selecting = false
        timer.word = ""
        lookupSpy.clear()
    }
    function test_waitForFingerRelease() {
        timer.enabled = true
        timer.selecting = true
        timer.word = "printed"
        wait(timer.interval + 100)
        compare(lookupSpy.count, 0)
        timer.selecting = false
        tryCompare(lookupSpy, "count", 1)
        compare(lookupSpy.signalArguments[0][0], "printed")
    }
    function test_handleDragCancelsPendingLookup() {
        timer.enabled = true
        timer.word = "first"
        timer.selecting = true
        timer.word = "second"
        wait(timer.interval + 100)
        compare(lookupSpy.count, 0)
        timer.selecting = false
        tryCompare(lookupSpy, "count", 1)
        compare(lookupSpy.signalArguments[0][0], "second")
    }
    function test_disabledOrClearedSelection() {
        timer.enabled = true
        timer.word = "printed"
        timer.enabled = false
        wait(timer.interval + 100)
        compare(lookupSpy.count, 0)
        timer.enabled = true
        timer.word = ""
        wait(timer.interval + 100)
        compare(lookupSpy.count, 0)
    }
}
