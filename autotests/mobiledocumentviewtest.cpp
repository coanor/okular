/*
    SPDX-FileCopyrightText: 2026 coanor <coanor@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <KLocalizedContext>
#include <QApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QtQuickTest/quicktest.h>

class DocumentViewTestSetup : public QObject
{
    Q_OBJECT

public Q_SLOTS:
    void applicationAvailable()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void qmlEngineAvailable(QQmlEngine *engine)
    {
        engine->addImportPath(QStringLiteral(OKULAR_QML_IMPORT_PATH));
        engine->rootContext()->setContextObject(new KLocalizedContext(engine));
    }
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    DocumentViewTestSetup setup;
    return quick_test_main_with_setup(argc, argv, "mobiledocumentviewtest", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "mobiledocumentviewtest.moc"
