/*
    SPDX-FileCopyrightText: 2026 coanor <coanor@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <KLocalizedContext>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtQuickTest/quicktest.h>

class DocumentViewTestSetup : public QObject
{
    Q_OBJECT

public:
    ~DocumentViewTestSetup() override
    {
        const QFileInfo fixture(m_fixturePath);
        const QString metadataPath =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/okular/docdata/") + QString::number(fixture.size()) + QLatin1Char('.') + fixture.fileName() + QStringLiteral(".xml");
        QFile::remove(metadataPath);
    }

public Q_SLOTS:
    void applicationAvailable()
    {
        QStandardPaths::setTestModeEnabled(true);
        m_fixturePath = m_fixtureDir.filePath(QFileInfo(m_fixtureDir.path()).fileName() + QStringLiteral(".pdf"));
        if (!m_fixtureDir.isValid() || !QFile::copy(QStringLiteral(QUICK_TEST_SOURCE_DIR "/../data/simple-multipage.pdf"), m_fixturePath)) {
            qFatal("Could not prepare the mobile document test fixture");
        }
    }

    void qmlEngineAvailable(QQmlEngine *engine)
    {
        engine->addImportPath(QStringLiteral(OKULAR_QML_IMPORT_PATH));
        engine->rootContext()->setContextObject(new KLocalizedContext(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("testDocumentUrl"), QUrl::fromLocalFile(m_fixturePath));
    }

private:
    QTemporaryDir m_fixtureDir;
    QString m_fixturePath;
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    DocumentViewTestSetup setup;
    return quick_test_main_with_setup(argc, argv, "mobiledocumentviewtest", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "mobiledocumentviewtest.moc"
