/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/s3transport.h"

#include <QCryptographicHash>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include <future>

class FakeS3Server : public QTcpServer
{
    Q_OBJECT
public:
    struct Request {
        QByteArray method;
        QByteArray target;
        QByteArray headers;
        QByteArray body;
    };
    QList<Request> requests;
    int listPages = 0;

    FakeS3Server()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer = QByteArray()]() mutable {
                buffer += socket->readAll();
                const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                const QByteArray headers = buffer.left(headerEnd);
                qsizetype contentLength = 0;
                for (const QByteArray &line : headers.split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        contentLength = line.mid(15).trimmed().toLongLong();
                    }
                }
                if (buffer.size() < headerEnd + 4 + contentLength) {
                    return;
                }
                const QList<QByteArray> requestLine = headers.split('\n').first().trimmed().split(' ');
                requests.append({requestLine.value(0), requestLine.value(1), headers, buffer.mid(headerEnd + 4, contentLength)});
                QByteArray body;
                if (requestLine.value(1).contains("list-type=2")) {
                    ++listPages;
                    body = listPages == 1
                        ? "<ListBucketResult><IsTruncated>true</IsTruncated><NextContinuationToken>next token</NextContinuationToken><Contents><Key>cloud/books/a/manifest.json</Key></Contents></ListBucketResult>"
                        : "<ListBucketResult><IsTruncated>false</IsTruncated><Contents><Key>cloud/books/b/manifest.json</Key></Contents></ListBucketResult>";
                } else if (requestLine.value(0) == "GET") {
                    body = requestLine.value(1).contains("source.pdf") ? QByteArray("source file contents") : QByteArray("{\"schemaVersion\":1}");
                }
                socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }
};

class S3TransportTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void signsRequestsAndPaginates()
    {
        FakeS3Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        S3Configuration config;
        config.bucket = QStringLiteral("test-bucket");
        config.prefix = QStringLiteral("cloud");
        config.region = QStringLiteral("us-east-1");
        config.endpoint = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        config.accessKeyId = QStringLiteral("access-id");
        config.secretAccessKey = QStringLiteral("secret-key");
        config.sessionToken = QStringLiteral("temporary-token");
        S3Transport transport(config);

        auto upload = std::async(std::launch::async, [&transport] { return transport.putObjectIfAbsent(QStringLiteral("books/a/manifest.json"), "{\"a\":1}"); });
        QTRY_COMPARE(server.requests.size(), 1);
        QVERIFY(upload.get().successful());
        QCOMPARE(server.requests.first().method, QByteArray("PUT"));
        QCOMPARE(server.requests.first().target, QByteArray("/test-bucket/cloud/books/a/manifest.json"));
        QCOMPARE(server.requests.first().body, QByteArray("{\"a\":1}"));
        QVERIFY(server.requests.first().headers.contains("If-None-Match: *"));
        QVERIFY(server.requests.first().headers.contains("AWS4-HMAC-SHA256"));
        QVERIFY(server.requests.first().headers.toLower().contains("x-amz-security-token: temporary-token"));
        QVERIFY(!server.requests.first().headers.contains("secret-key"));

        auto download = std::async(std::launch::async, [&transport] { return transport.getObject(QStringLiteral("version.json")); });
        QTRY_COMPARE(server.requests.size(), 2);
        const S3Response downloaded = download.get();
        QVERIFY(downloaded.successful());
        QCOMPARE(downloaded.body, QByteArray("{\"schemaVersion\":1}"));
        QCOMPARE(server.requests.at(1).target, QByteArray("/test-bucket/cloud/version.json"));

        auto list = std::async(std::launch::async, [&transport] {
            QStringList keys;
            QString error;
            const bool okay = transport.listObjects(QStringLiteral("books/"), &keys, &error);
            return qMakePair(okay, qMakePair(keys, error));
        });
        QTRY_COMPARE(server.requests.size(), 4);
        const auto listed = list.get();
        QVERIFY2(listed.first, qPrintable(listed.second.second));
        QCOMPARE(listed.second.first, QStringList({QStringLiteral("books/a/manifest.json"), QStringLiteral("books/b/manifest.json")}));
        const QUrl firstList(QStringLiteral("http://localhost") + QString::fromLatin1(server.requests.at(2).target));
        const QUrl secondList(QStringLiteral("http://localhost") + QString::fromLatin1(server.requests.at(3).target));
        QCOMPARE(QUrlQuery(firstList).queryItemValue(QStringLiteral("prefix")), QStringLiteral("cloud/books/"));
        QCOMPARE(QUrlQuery(secondList).queryItemValue(QStringLiteral("continuation-token")), QStringLiteral("next token"));

        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString sourcePath = temp.filePath(QStringLiteral("source.pdf"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("source file contents"), 20);
        source.close();
        auto fileUpload = std::async(std::launch::async, [&transport, &sourcePath] { return transport.putFileIfAbsent(QStringLiteral("books/a/source.pdf"), sourcePath); });
        QTRY_COMPARE(server.requests.size(), 5);
        QVERIFY(fileUpload.get().successful());
        QCOMPARE(server.requests.at(4).body, QByteArray("source file contents"));

        const QString destinationPath = temp.filePath(QStringLiteral("download.pdf"));
        const QString hash = QString::fromLatin1(QCryptographicHash::hash("source file contents", QCryptographicHash::Sha256).toHex());
        auto fileDownload = std::async(std::launch::async, [&transport, &destinationPath, &hash] {
            return transport.downloadFile(QStringLiteral("books/a/source.pdf"), destinationPath, hash);
        });
        QTRY_COMPARE(server.requests.size(), 6);
        QVERIFY(fileDownload.get().successful());
        QFile downloadedFile(destinationPath);
        QVERIFY(downloadedFile.open(QIODevice::ReadOnly));
        QCOMPARE(downloadedFile.readAll(), QByteArray("source file contents"));
        downloadedFile.close();

        auto corruptDownload = std::async(std::launch::async, [&transport, &destinationPath] {
            return transport.downloadFile(QStringLiteral("books/a/source.pdf"), destinationPath, QStringLiteral("wrong-hash"));
        });
        QTRY_COMPARE(server.requests.size(), 7);
        QVERIFY(!corruptDownload.get().successful());
        QVERIFY(downloadedFile.open(QIODevice::ReadOnly));
        QCOMPARE(downloadedFile.readAll(), QByteArray("source file contents"));
    }
};

QTEST_GUILESS_MAIN(S3TransportTest)
#include "s3transporttest.moc"
