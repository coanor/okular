/*
 SPDX-FileCopyrightText: 2025 Sebastian Kügler <sebas@kde.org>

 SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtCore
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts

import Qt.labs.folderlistmodel

import org.kde.okular as Okular
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard

FormCard.FormCard {
    id: welcomeView

    visible: !document.opened

    anchors.centerIn: parent
    width: Math.max(Kirigami.Units.gridUnit * 24, parent.width - Kirigami.Units.gridUnit * 24)
    height: Math.max(Math.min(parent.height * 0.9, Kirigami.Units.gridUnit * 40), parent.height - Kirigami.Units.gridUnit * 8)

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.margins: Kirigami.Units.gridUnit

        spacing: Kirigami.Units.gridUnit

        Kirigami.PlaceholderMessage {
            text: i18n("No document open")
            helpfulAction: openDocumentAction
            Layout.fillWidth: true
        }

        Kirigami.Heading {
            text: i18nc("in welcome screen", "Continue Reading")
            visible: recentList.count
        }

        Component {
            id: fileDelegate
            MouseArea {
                Layout.fillWidth: true
                width: Math.max(recentList.width, documentsList.width)
                height: Kirigami.Units.gridUnit * 1.6
                RowLayout {
                    anchors.fill: parent
                    spacing: Math.round(Kirigami.Units.gridUnit * 0.4)

                    Kirigami.Icon {
                        source: (typeof iconName !== "undefined") ? iconName : "application-pdf"
                    }

                    Controls.Label {
                        text: (typeof modelData !== "undefined" && modelData.pageCount !== undefined) ? (modelData.pageCount > 0 ? i18n("%1 — page %2 of %3", modelData.title, modelData.page + 1, modelData.pageCount) : modelData.title) : fileName
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
                onClicked: {
                    if (typeof modelData !== "undefined" && modelData.url !== undefined) {
                        // Recent Document
                        documentItem.url = modelData.url;
                    } else {
                        // File is in Documents folder
                        documentItem.url = fileUrl;
                    }
                }
            }
        }

        Controls.ScrollView{
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: recentList.count

            background: Rectangle {
                color: Kirigami.Theme.backgroundColor
                border.color: Kirigami.Theme.textColor
                radius: Kirigami.Units.cornerRadius
                opacity: 0.2
            }

            ListView {
                id: recentList
                clip: true
                spacing: Math.round(Kirigami.Units.gridUnit * 0.2)

                model: document.readingHistory.books
                delegate: fileDelegate
            }
        }

        Kirigami.Heading {
            text: i18nc("in welcome screen", "My Documents")
            visible: documentsList.count
        }

        Controls.ScrollView{
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: documentsList.count

            background: Rectangle {
                color: Kirigami.Theme.backgroundColor
                border.color: Kirigami.Theme.textColor
                radius: Kirigami.Units.cornerRadius
                opacity: 0.2
            }

            ListView {
                id: documentsList
                clip: true
                spacing: Math.round(Kirigami.Units.gridUnit * 0.2)

                FolderListModel {
                    id: folderModel
                    // Note: can be changed by editing ~/.config/user-dirs.dirs and changing
                    // the value of XDG_DOCUMENTS_DIR
                    folder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
                    nameFilters: Okular.Okular.nameFilters
                }

                model: folderModel
                delegate: fileDelegate
            }
        }
    }
}
