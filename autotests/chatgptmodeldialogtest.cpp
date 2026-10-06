/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>
#include <QtQuickTest/quicktest.h>

// Exercise the real dialog with a local account/model fixture, without OAuth.
class ModelDialogSetup : public QObject
{
    Q_OBJECT
public Q_SLOTS:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        engine->addImportPath(QStringLiteral(QUICK_TEST_SOURCE_DIR "/imports"));
        engine->rootContext()->setContextProperty(QStringLiteral("testDialogUrl"), QUrl::fromLocalFile(QStringLiteral(OKULAR_APP_SOURCE_DIR "/ChatGptModelDialog.qml")));
    }
};

QUICK_TEST_MAIN_WITH_SETUP(chatgptmodeldialog, ModelDialogSetup)
#include "chatgptmodeldialogtest.moc"
