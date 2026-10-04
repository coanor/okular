/*
    SPDX-FileCopyrightText: 2026 coanor <coanor@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <KLocalizedContext>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QtQuickTest/quicktest.h>

// The mobile assistant talks to a local HTTP fixture, never a real provider.
class MobileAiServer : public QTcpServer
{
    Q_OBJECT
    Q_PROPERTY(QString endpoint READ endpoint CONSTANT)
    Q_PROPERTY(int requestCount READ requestCount NOTIFY requestReceived)
    Q_PROPERTY(QVariantMap lastRequest READ lastRequest NOTIFY requestReceived)
    Q_PROPERTY(bool autoRespond MEMBER m_autoRespond)
    Q_PROPERTY(bool failRequests MEMBER m_failRequests)

public:
    MobileAiServer()
    {
        if (!listen(QHostAddress::LocalHost)) {
            qFatal("Could not start the mobile AI test server");
        }
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, data = QByteArray(), handled = false]() mutable {
                if (handled) {
                    return;
                }
                data += socket->readAll();
                const qsizetype headerEnd = data.indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                qsizetype length = 0;
                for (const QByteArray &line : data.left(headerEnd).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        length = line.mid(15).trimmed().toLongLong();
                    }
                }
                if (data.size() < headerEnd + 4 + length) {
                    return;
                }
                handled = true;
                m_lastRequest = QJsonDocument::fromJson(data.mid(headerEnd + 4, length)).object().toVariantMap();
                ++m_requestCount;
                Q_EMIT requestReceived();
                if (!m_autoRespond) {
                    return;
                }
                const QByteArray body = m_failRequests ? QByteArrayLiteral("{\"error\":{\"message\":\"test failure\"}}") : QByteArrayLiteral("{\"choices\":[{\"message\":{\"content\":\"test answer\"}}]}");
                socket->write((m_failRequests ? QByteArrayLiteral("HTTP/1.1 500 Error\r\n") : QByteArrayLiteral("HTTP/1.1 200 OK\r\n")) +
                              "Content-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }

    QString endpoint() const
    {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(serverPort());
    }
    int requestCount() const
    {
        return m_requestCount;
    }
    QVariantMap lastRequest() const
    {
        return m_lastRequest;
    }

    Q_INVOKABLE void reset()
    {
        m_requestCount = 0;
        m_lastRequest.clear();
        m_autoRespond = true;
        m_failRequests = false;
        Q_EMIT requestReceived();
    }

Q_SIGNALS:
    void requestReceived();

private:
    int m_requestCount = 0;
    QVariantMap m_lastRequest;
    bool m_autoRespond = true;
    bool m_failRequests = false;
};

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
        engine->rootContext()->setContextProperty(QStringLiteral("aiTestServer"), &m_aiServer);
        engine->rootContext()->setContextProperty(QStringLiteral("testDocumentUrl"), QUrl::fromLocalFile(m_fixturePath));
    }

private:
    MobileAiServer m_aiServer;
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
