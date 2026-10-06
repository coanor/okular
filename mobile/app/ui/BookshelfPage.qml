/* SPDX-License-Identifier: GPL-2.0-or-later */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Dialogs as QQD
import org.kde.okular as Okular
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: root
    required property Okular.DocumentItem document
    signal openBook(url url)
    signal openDocument()

    title: i18n("Bookshelf")
    readonly property var books: document.readingHistory.books

    Component.onCompleted: document.readingHistory.refresh()
    actions: [Kirigami.Action {
        text: i18n("Open…")
        icon.name: "document-open"
        onTriggered: root.openDocument()
    }, Kirigami.Action {
        text: i18n("Export reading data…")
        icon.name: "document-export"
        onTriggered: exportDialog.open()
    }]

    QQD.FileDialog {
        id: exportDialog
        fileMode: QQD.FileDialog.SaveFile
        nameFilters: [i18n("SQLite database (*.sqlite)")]
        defaultSuffix: "sqlite"
        onAccepted: root.document.readingHistory.exportDatabase(selectedFile)
    }
    Connections {
        target: root.document.readingHistory
        function onExported(destination) {
            applicationWindow().showPassiveNotification(i18n("Reading data exported"))
        }
    }

    ListView {
        model: root.books
        spacing: Kirigami.Units.smallSpacing
        header: Kirigami.InlineMessage {
            width: ListView.view.width
            text: root.document.readingHistory.error
            type: Kirigami.MessageType.Error
            visible: text.length > 0
        }
        delegate: QQC2.ItemDelegate {
            required property var modelData
            width: ListView.view.width
            onClicked: {
                if (modelData.url.toString().length > 0)
                    root.openBook(modelData.url)
                else
                    root.openDocument()
            }
            contentItem: ColumnLayout {
                QQC2.Label {
                    Layout.fillWidth: true
                    text: modelData.title
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: modelData.pageCount > 0 ? i18n("Page %1 of %2", modelData.page + 1, modelData.pageCount) : i18n("Ready to read")
                    opacity: 0.7
                }
                QQC2.Label {
                    visible: modelData.lastRead !== undefined
                    text: modelData.lastRead !== undefined ? Qt.formatDateTime(modelData.lastRead, "yyyy-MM-dd hh:mm") : ""
                    opacity: 0.7
                }
                QQC2.ProgressBar {
                    Layout.fillWidth: true
                    visible: modelData.pageCount > 0
                    value: modelData.pageCount > 0 ? (modelData.page + 1) / modelData.pageCount : 0
                }
            }
        }
        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.largeSpacing * 2
            visible: root.books.length === 0
            text: i18n("Your bookshelf is empty")
            explanation: i18n("Open a document to add it to your bookshelf.")
        }
    }
}
