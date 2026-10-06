/*
    SPDX-FileCopyrightText: 2026 coanor <coanor@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <KConfigGroup>
#include <KLocalizedContext>
#include <KSharedConfig>
#include <QApplication>
#include <QDir>
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
#include <QtConcurrentRun>
#include <QtQuickTest/quicktest.h>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

// The mobile assistant talks to a local HTTP fixture, never a real provider.
class MobileAiServer : public QTcpServer
{
    Q_OBJECT
    Q_PROPERTY(QString endpoint READ endpoint CONSTANT)
    Q_PROPERTY(int requestCount READ requestCount NOTIFY requestReceived)
    Q_PROPERTY(QVariantMap lastRequest READ lastRequest NOTIFY requestReceived)
    Q_PROPERTY(bool autoRespond MEMBER m_autoRespond)
    Q_PROPERTY(bool failRequests MEMBER m_failRequests)
    Q_PROPERTY(QString responseText MEMBER m_responseText)

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
                const QJsonObject choice {{QStringLiteral("message"), QJsonObject {{QStringLiteral("content"), m_responseText}}}};
                const QByteArray body = m_failRequests ? QByteArrayLiteral("{\"error\":{\"message\":\"test failure\"}}") : QJsonDocument(QJsonObject {{QStringLiteral("choices"), QJsonArray {choice}}}).toJson(QJsonDocument::Compact);
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
        m_responseText = QStringLiteral("test answer");
        Q_EMIT requestReceived();
    }

Q_SIGNALS:
    void requestReceived();

private:
    int m_requestCount = 0;
    QVariantMap m_lastRequest;
    bool m_autoRespond = true;
    bool m_failRequests = false;
    QString m_responseText = QStringLiteral("test answer");
};

class DocumentViewTestSetup : public QObject
{
    Q_OBJECT

public:
    Q_INVOKABLE QVariantList storedProfiles() const
    {
        const KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("AI Reading Assistant"));
        return QJsonDocument::fromJson(group.readEntry("Profiles", QByteArray())).array().toVariantList();
    }

    Q_INVOKABLE void setStoredProfiles(const QVariantList &profiles)
    {
        const auto config = KSharedConfig::openConfig();
        KConfigGroup group(config, QStringLiteral("AI Reading Assistant"));
        group.writeEntry("Profiles", QJsonDocument::fromVariant(profiles).toJson(QJsonDocument::Compact));
        config->sync();
    }

    Q_INVOKABLE bool supportsSourceUrls() const
    {
#ifdef Q_OS_ANDROID
        return true;
#else
        return false;
#endif
    }

    Q_INVOKABLE QUrl pipeDescriptorUrl(const QUrl &source = {})
    {
#ifdef Q_OS_UNIX
        QFile file(m_fixturePath);
        int descriptors[2];
        if (!file.open(QIODevice::ReadOnly) || pipe(descriptors) < 0) {
            qFatal("Could not prepare the streaming document fixture");
        }
        const QByteArray data = file.readAll();
        (void)QtConcurrent::run([data, writer = descriptors[1]] {
            QFile stream;
            if (!stream.open(writer, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle) || stream.write(data) != data.size()) {
                qFatal("Could not write the streaming document fixture");
            }
        });
#ifdef Q_OS_ANDROID
        if (!source.isEmpty()) {
            const QJniObject parcel = QJniObject::callStaticObjectMethod("android/os/ParcelFileDescriptor", "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;", descriptors[0]);
            const QJniObject uri = QJniObject::callStaticObjectMethod("android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", QJniObject::fromString(source.toString()).object<jstring>());
            const QJniObject activity(QNativeInterface::QAndroidApplication::context());
            const QString url = activity.callObjectMethod("descriptorUrl", "(Landroid/os/ParcelFileDescriptor;Landroid/net/Uri;)Ljava/lang/String;", parcel.object(), uri.object()).toString();
            if (url.isEmpty()) {
                qFatal("Could not associate the streaming fixture's source URI");
            }
            return QUrl(url);
        }
#else
        Q_UNUSED(source)
#endif
        return QUrl(QStringLiteral("fd:///%1").arg(descriptors[0]));
#else
        Q_UNUSED(source)
        return {};
#endif
    }

    Q_INVOKABLE QUrl restrictedDescriptorUrl()
    {
#ifdef Q_OS_UNIX
        const QString path = m_restrictedFixtureDir.filePath(QStringLiteral("document.pdf"));
        if (!QFile::exists(path) && !QFile::copy(m_fixturePath, path)) {
            qFatal("Could not copy the descriptor fixture");
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qFatal("Could not open the descriptor fixture");
        }
        const int descriptor = dup(file.handle());
        if (descriptor < 0 || !file.setPermissions({})) {
            qFatal("Could not restrict the descriptor fixture");
        }
        // The descriptor stays readable while reopening its pathname fails,
        // as with an Android provider granting access only through a descriptor.
        return QUrl(QStringLiteral("fd:///%1").arg(descriptor));
#else
        return {};
#endif
    }

    ~DocumentViewTestSetup() override
    {
        qunsetenv("OKULAR_READING_DATA_PATH");
        const QFileInfo fixture(m_fixturePath);
        const QString metadataPath =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/okular/docdata/") + QString::number(fixture.size()) + QLatin1Char('.') + fixture.fileName() + QStringLiteral(".xml");
        QFile::remove(metadataPath);
    }

public Q_SLOTS:
    void applicationAvailable()
    {
        QStandardPaths::setTestModeEnabled(true);
        qputenv("OKULAR_READING_DATA_PATH", m_fixtureDir.filePath(QStringLiteral("reading-data.sqlite")).toUtf8());
        // Tests use fake in-memory keys and must never open the user's wallet.
        const auto walletConfig = KSharedConfig::openConfig(QStringLiteral("kwalletrc"));
        KConfigGroup(walletConfig, QStringLiteral("Wallet")).writeEntry("Enabled", false);
        walletConfig->sync();
        m_fixturePath = m_fixtureDir.filePath(QFileInfo(m_fixtureDir.path()).fileName() + QStringLiteral(".pdf"));
        if (!m_fixtureDir.isValid() || !QFile::copy(QStringLiteral(QUICK_TEST_SOURCE_DIR "/../data/simple-multipage.pdf"), m_fixturePath)) {
            qFatal("Could not prepare the mobile document test fixture");
        }
    }

    void qmlEngineAvailable(QQmlEngine *engine)
    {
        const QDir appDirectory(QStringLiteral(OKULAR_APP_SOURCE_DIR));
        for (const QString &file : appDirectory.entryList({QStringLiteral("*.qml")}, QDir::Files)) {
            qmlRegisterType(QUrl::fromLocalFile(appDirectory.filePath(file)), "org.kde.okular.app", 1, 0, QFileInfo(file).baseName().toUtf8().constData());
        }
        engine->addImportPath(QStringLiteral(OKULAR_QML_IMPORT_PATH));
        engine->rootContext()->setContextProperty(QStringLiteral("uri"), QUrl());
        engine->rootContext()->setContextProperty(QStringLiteral("testMainUrl"), QUrl::fromLocalFile(appDirectory.filePath(QStringLiteral("Main.qml"))));
        engine->rootContext()->setContextObject(new KLocalizedContext(engine));
        engine->rootContext()->setContextProperty(QStringLiteral("aiTestServer"), &m_aiServer);
        engine->rootContext()->setContextProperty(QStringLiteral("aiTestFiles"), this);
        engine->rootContext()->setContextProperty(QStringLiteral("testDocumentUrl"), QUrl::fromLocalFile(m_fixturePath));
    }

private:
    static QString fixtureTemplate()
    {
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDir().mkpath(cache);
        return cache + QStringLiteral("/mobile-document-test-XXXXXX");
    }

    MobileAiServer m_aiServer;
    QTemporaryDir m_fixtureDir {fixtureTemplate()};
    QTemporaryDir m_restrictedFixtureDir {fixtureTemplate()};
    QString m_fixturePath;
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    DocumentViewTestSetup setup;
    return quick_test_main_with_setup(argc, argv, "mobiledocumentviewtest", QUICK_TEST_SOURCE_DIR, &setup);
}

#include "mobiledocumentviewtest.moc"
