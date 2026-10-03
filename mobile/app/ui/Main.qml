/*
    SPDX-FileCopyrightText: 2012 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtCore
import QtQuick 2.15
import QtQuick.Controls 2.15 as QQC2
import QtQuick.Dialogs as QQD
import QtQuick.Layouts
import org.kde.okular 2.0 as Okular
import org.kde.kirigami 2.17 as Kirigami
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.okular.app

Kirigami.ApplicationWindow {
    id: fileBrowserRoot

    readonly property int columnWidth: Kirigami.Units.gridUnit * 13

    wideScreen: width > columnWidth * 5
    visible: true

    globalDrawer: Kirigami.GlobalDrawer {
        title: i18n("Okular")
        titleIcon: "okular"
        drawerOpen: false
        isMenu: true

        QQD.FileDialog {
            id: fileDialog
            nameFilters: Okular.Okular.nameFilters
            currentFolder: StandardPaths.standardLocations(StandardPaths.DocumentsLocation)[0]
            onAccepted: {
                documentItem.url = fileDialog.selectedFile
            }
        }

        actions: [
            Kirigami.Action {
                id: openDocumentAction
                text: i18n("Open…")
                icon.name: "document-open"
                onTriggered: {
                    fileDialog.open()
                }
            },
            Kirigami.Action {
                text: i18n("Dictionary…")
                icon.name: "applications-education-language-symbolic"
                onTriggered: dictionaryDialog.open()
            },
            Kirigami.Action {
                text: i18n("About")
                icon.name: "help-about-symbolic"
                onTriggered: fileBrowserRoot.pageStack.layers.push(Qt.createComponent("org.kde.kirigamiaddons.formcard", "AboutPage"));
                enabled: fileBrowserRoot.pageStack.layers.depth === 1
            }
        ]
    }

    QQD.FileDialog {
        id: dictionaryFileDialog
        fileMode: QQD.FileDialog.OpenFile
        nameFilters: [i18n("MDX dictionaries (*.mdx)")]
        onAccepted: Okular.DictionaryLookup.importFile(selectedFile)
    }

    QQC2.Dialog {
        id: dictionaryDialog
        title: i18n("Dictionary")
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 440)
        standardButtons: QQC2.Dialog.Close

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing
            QQC2.CheckBox {
                text: i18n("Automatically look up selected words")
                checked: Okular.DictionaryLookup.autoLookupEnabled
                onToggled: Okular.DictionaryLookup.autoLookupEnabled = checked
            }
            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("MDX dictionary")
            }
            QQC2.TextField {
                id: dictionaryPath
                property bool edited: false
                Layout.fillWidth: true
                enabled: Okular.DictionaryLookup.mdxAvailable
                text: Okular.DictionaryLookup.dictionaryFile
                placeholderText: i18n("Leave empty to use Eudic")
                onTextEdited: edited = true
                onEditingFinished: {
                    if (edited) {
                        edited = false
                        Okular.DictionaryLookup.dictionaryFile = text.trim()
                    }
                }
            }
            RowLayout {
                QQC2.Button {
                    text: i18n("Browse…")
                    enabled: Okular.DictionaryLookup.mdxAvailable && !Okular.DictionaryLookup.importing
                    onClicked: dictionaryFileDialog.open()
                }
                QQC2.Button {
                    text: i18n("Use Eudic")
                    onClicked: {
                        Okular.DictionaryLookup.dictionaryFile = ""
                        dictionaryPath.text = ""
                    }
                }
            }
            QQC2.Label {
                Layout.fillWidth: true
                visible: !Okular.DictionaryLookup.mdxAvailable || Okular.DictionaryLookup.importing || !!Okular.DictionaryLookup.importError
                text: !Okular.DictionaryLookup.mdxAvailable ? i18n("This build does not include MDX support.") :
                      Okular.DictionaryLookup.importing ? i18n("Importing dictionary…") : Okular.DictionaryLookup.importError
                wrapMode: Text.WordWrap
            }
            Connections {
                target: Okular.DictionaryLookup
                function onDictionaryFileChanged() {
                    if (!dictionaryPath.activeFocus) {
                        dictionaryPath.text = Okular.DictionaryLookup.dictionaryFile
                    }
                }
            }
        }
    }
    contextDrawer: OkularDrawer {
        width: columnWidth
        contentItem.implicitWidth: columnWidth
        modal: !fileBrowserRoot.wideScreen
        onModalChanged: drawerOpen = !modal
        onEnabledChanged: drawerOpen = enabled && !modal
        enabled: documentItem.opened && pageStack.layers.depth < 2
        handleVisible: enabled && pageStack.layers.depth < 2
    }

    title: documentItem.windowTitleForDocument ? documentItem.windowTitleForDocument : i18n("Okular")
    Okular.DocumentItem {
        id: documentItem
        onUrlChanged: { currentPage = 0 }

        onNeedsPasswordChanged: {
            if (needsPassword) {
                passwordDialog.open();
            }
        }
    }

    pageStack.initialPage: MainView {
        id: mainView
        document: documentItem
        Kirigami.ColumnView.preventStealing: true
    }

    //FIXME: this is due to global vars being bound after the parse is done, do the 2 steps parsing
    Timer {
        interval: 100
        running: true
        onTriggered: {
            if (uri) {
                documentItem.url = uri
            }
        }
    }

    QQC2.Dialog {
        id: passwordDialog
        focus: true
        anchors.centerIn: parent
        title: i18n("Password Needed")
        contentItem: Kirigami.PasswordField {
            id: pwdField
            onAccepted: passwordDialog.accept();
            focus: true
        }
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel

        onAccepted: documentItem.setPassword(pwdField.text);
        onRejected: documentItem.url = "";
    }
}
