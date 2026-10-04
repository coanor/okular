/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../core/annotationsidecar_p.h"
#include "../mobile/components/cloudlibrary.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <KLocalizedString>

class CloudTestServer : public QTcpServer
{
    Q_OBJECT
public:
    QMap<QString, QByteArray> objects;

    CloudTestServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer = QByteArray(), answered = false]() mutable {
                if (answered) {
                    return;
                }
                buffer += socket->readAll();
                const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                const QByteArray headers = buffer.left(headerEnd);
                qsizetype length = 0;
                for (const QByteArray &line : headers.split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        length = line.mid(15).trimmed().toLongLong();
                    }
                }
                if (buffer.size() < headerEnd + 4 + length) {
                    return;
                }
                answered = true;
                const auto request = headers.split('\n').first().split(' ');
                const QUrl url(QStringLiteral("http://localhost") + QString::fromUtf8(request.value(1)));
                const QString key = url.path().mid(QStringLiteral("/test-bucket/").size());
                const QUrlQuery query(url);
                QByteArray body;
                int status = 200;
                if (query.hasQueryItem(QStringLiteral("list-type"))) {
                    body = "<ListBucketResult><IsTruncated>false</IsTruncated>";
                    for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
                        if (it.key().startsWith(query.queryItemValue(QStringLiteral("prefix")))) {
                            body += "<Contents><Key>" + it.key().toHtmlEscaped().toUtf8() + "</Key></Contents>";
                        }
                    }
                    body += "</ListBucketResult>";
                } else if (request.first() == "PUT") {
                    if (objects.contains(key) && headers.toLower().contains("if-none-match: *")) {
                        status = 412;
                    } else {
                        objects.insert(key, buffer.mid(headerEnd + 4, length));
                    }
                } else if (objects.contains(key)) {
                    body = objects.value(key);
                } else {
                    status = 404;
                }
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
    }
};

class MobileCloudLibraryTest : public QObject
{
    Q_OBJECT
private:
    static QVariantMap settings(quint16 port = 80)
    {
        return {{QStringLiteral("bucket"), QStringLiteral("test-bucket")},
                {QStringLiteral("prefix"), QStringLiteral("mobile-test")},
                {QStringLiteral("region"), QStringLiteral("us-east-1")},
                {QStringLiteral("endpoint"), QStringLiteral("http://127.0.0.1:%1").arg(port)},
                {QStringLiteral("accessKeyId"), QStringLiteral("test-access-key")}};
    }

    static bool writeFile(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    }

private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain("okular");
    }

    void credentialsStayOutOfSettingsAndSyncMetadata()
    {
        QTemporaryDir directory;
        CloudLibrary library(directory.path());
        QTRY_VERIFY(!library.busy());
        library.configure(settings(), {}, {}, false);
        QVERIFY(!library.configured());
        QVERIFY(!library.error().isEmpty());
        library.configure(settings(), QStringLiteral("secret-must-stay-private"), QStringLiteral("session-must-stay-private"), false);
        QTRY_VERIFY(!library.busy());
        QVERIFY2(library.error().isEmpty(), qPrintable(library.error()));
        QVERIFY(library.configured());
        QVERIFY(!library.settings().contains(QStringLiteral("secretAccessKey")));
        QVERIFY(!library.settings().contains(QStringLiteral("sessionToken")));
        QFile settingsFile(directory.filePath(QStringLiteral("settings.ini")));
        QVERIFY(settingsFile.open(QIODevice::ReadOnly));
        const QByteArray stored = settingsFile.readAll();
        QVERIFY(!stored.contains("secret-must-stay-private"));
        QVERIFY(!stored.contains("session-must-stay-private"));
        QVERIFY(!stored.contains("test-access-key"));
        CloudLibrary restarted(directory.path());
        QTRY_VERIFY(!restarted.busy());
        QVERIFY(!restarted.configured());
        QCOMPARE(restarted.settings().value(QStringLiteral("bucket")).toString(), QStringLiteral("test-bucket"));
        library.forgetCredentials();
        QTRY_VERIFY(!library.busy());
        QVERIFY(!library.configured());
    }

    void importKeepsOriginalAndDeduplicates()
    {
        QTemporaryDir directory;
        const QString source = directory.filePath(QStringLiteral("book.pdf"));
        const QByteArray contents("%PDF-1.7\nMobile cloud test book\n");
        QVERIFY(writeFile(source, contents));
        CloudLibrary library(directory.filePath(QStringLiteral("library")));
        QTRY_VERIFY(!library.busy());
        QSignalSpy imported(&library, &CloudLibrary::bookImported);
        library.importBook(QUrl::fromLocalFile(source));
        QTRY_VERIFY(!library.busy());
        QVERIFY2(library.error().isEmpty(), qPrintable(library.error()));
        QCOMPARE(imported.size(), 1);
        QCOMPARE(library.books().size(), 1);
        QVERIFY(QFile::exists(source));
        const QString managed = library.books().first().toMap().value(QStringLiteral("url")).toUrl().toLocalFile();
        QVERIFY(managed != source);
        QFile copy(managed);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), contents);
        library.importBook(QUrl::fromLocalFile(source));
        QTRY_VERIFY(!library.busy());
        QCOMPARE(library.books().size(), 1);
        QVERIFY(QFile::exists(source));
        library.importBook(QUrl(QStringLiteral("https://example.invalid/book.pdf")));
        QVERIFY(!library.busy());
        QVERIFY(!library.error().isEmpty());
        library.importBook(QUrl::fromLocalFile(directory.filePath(QStringLiteral("missing.pdf"))));
        QTRY_VERIFY(!library.busy());
        QVERIFY(!library.error().isEmpty());
        QCOMPARE(library.books().size(), 1);
    }

    void exchangesBooksAnnotationsAndResolvesConflicts()
    {
        QTemporaryDir directory;
        const QByteArray previousDataHome = qgetenv("XDG_DATA_HOME");
        const auto restoreEnvironment = qScopeGuard([previousDataHome] {
            if (previousDataHome.isNull()) {
                qunsetenv("XDG_DATA_HOME");
            } else {
                qputenv("XDG_DATA_HOME", previousDataHome);
            }
        });
        CloudTestServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        CloudLibrary first(directory.filePath(QStringLiteral("first")));
        CloudLibrary second(directory.filePath(QStringLiteral("second")));
        QTRY_VERIFY(!first.busy() && !second.busy());
        first.configure(settings(server.serverPort()), QStringLiteral("private-secret"), {}, false);
        second.configure(settings(server.serverPort()), QStringLiteral("private-secret"), {}, false);
        QTRY_VERIFY(!first.busy() && !second.busy());
        QVERIFY(first.configured() && second.configured());
        const QString source = directory.filePath(QStringLiteral("book.pdf"));
        QVERIFY(writeFile(source, "%PDF-1.7\nTest cloud document\n"));
        first.importBook(QUrl::fromLocalFile(source));
        QTRY_VERIFY(!first.busy());
        QCOMPARE(first.books().size(), 1);
        const QString hash = first.books().first().toMap().value(QStringLiteral("id")).toString();
        QString error;
        qputenv("XDG_DATA_HOME", directory.filePath(QStringLiteral("data-first")).toUtf8());
        QVERIFY(Okular::AnnotationSidecar::pathForHash(hash).startsWith(directory.path()));
        Okular::SidecarAnnotation annotation {QStringLiteral("highlight"), 0, 1, QStringLiteral("<highlight>initial</highlight>"), QStringLiteral("initial")};
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {annotation}, &error), qPrintable(error));
        first.synchronize();
        QTRY_VERIFY_WITH_TIMEOUT(!first.busy(), 15000);
        QVERIFY2(first.error().isEmpty(), qPrintable(first.error()));
        qputenv("XDG_DATA_HOME", directory.filePath(QStringLiteral("data-second")).toUtf8());
        second.synchronize();
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 15000);
        QVERIFY2(second.error().isEmpty(), qPrintable(second.error()));
        QCOMPARE(second.books().size(), 1);
        QCOMPARE(second.books().first().toMap().value(QStringLiteral("id")).toString(), hash);
        QList<Okular::SidecarAnnotation> downloaded;
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &downloaded, &error), qPrintable(error));
        QCOMPARE(downloaded.size(), 1);
        QCOMPARE(downloaded.first().contents, QStringLiteral("initial"));
        annotation.xml = QStringLiteral("<highlight>second</highlight>");
        annotation.contents = QStringLiteral("second");
        QVERIFY(Okular::AnnotationSidecar::save(hash, {annotation}, &error));
        second.synchronize();
        QTRY_VERIFY_WITH_TIMEOUT(!second.busy(), 15000);
        QVERIFY2(second.error().isEmpty(), qPrintable(second.error()));
        qputenv("XDG_DATA_HOME", directory.filePath(QStringLiteral("data-first")).toUtf8());
        annotation.xml = QStringLiteral("<highlight>first</highlight>");
        annotation.contents = QStringLiteral("first");
        QVERIFY(Okular::AnnotationSidecar::save(hash, {annotation}, &error));
        first.synchronize();
        QTRY_VERIFY_WITH_TIMEOUT(!first.busy(), 15000);
        QVERIFY2(first.error().isEmpty(), qPrintable(first.error()));
        QCOMPARE(first.conflicts().size(), 1);
        const QVariantList variants = first.conflicts().first().toMap().value(QStringLiteral("variants")).toList();
        int secondVersion = -1;
        for (int index = 0; index < variants.size(); ++index) {
            if (variants.at(index).toMap().value(QStringLiteral("value")).toMap().value(QStringLiteral("contents")).toString() == QLatin1String("second")) {
                secondVersion = index;
            }
        }
        QVERIFY(secondVersion >= 0);
        first.resolveConflict(0, secondVersion);
        QTRY_VERIFY_WITH_TIMEOUT(!first.busy(), 15000);
        QVERIFY2(first.error().isEmpty(), qPrintable(first.error()));
        QVERIFY(first.conflicts().isEmpty());
        QVERIFY(Okular::AnnotationSidecar::load(hash, &downloaded, &error));
        QCOMPARE(downloaded.first().contents, QStringLiteral("second"));
        for (const QByteArray &object : std::as_const(server.objects)) {
            QVERIFY(!object.contains("private-secret"));
        }
    }
};

QTEST_GUILESS_MAIN(MobileCloudLibraryTest)
#include "mobilecloudlibrarytest.moc"
