/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
import QtQuick 2.15

Timer {
    id: root
    property string word
    property bool enabled: false
    property bool selecting: false
    signal lookupRequested(string word)

    interval: 350
    function restartWhenReady() {
        stop()
        if (enabled && !selecting && word.length > 0) {
            restart()
        }
    }
    onWordChanged: restartWhenReady()
    onEnabledChanged: restartWhenReady()
    onSelectingChanged: restartWhenReady()
    onTriggered: {
        if (enabled && !selecting && word.length > 0) {
            lookupRequested(word)
        }
    }
}
