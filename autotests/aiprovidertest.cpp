/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/aiprovider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

class FakeAiServer : public QTcpServer
{
    Q_OBJECT
public:
    struct Request {
        QByteArray path;
        QJsonObject body;
    };
    QList<Request> requests;

    FakeAiServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, data = QByteArray()]() mutable {
                data += socket->readAll();
                const qsizetype headerEnd = data.indexOf("\r\n\r\n");
                if (headerEnd < 0) {
                    return;
                }
                const QByteArray header = data.left(headerEnd);
                qsizetype contentLength = 0;
                for (const QByteArray &line : header.split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        contentLength = line.mid(15).trimmed().toLongLong();
                    }
                }
                if (data.size() < headerEnd + 4 + contentLength) {
                    return;
                }
                const QByteArray path = header.split(' ').value(1);
                requests.append({path, QJsonDocument::fromJson(data.mid(headerEnd + 4, contentLength)).object()});
                QJsonObject response;
                if (path.endsWith("/conversations")) {
                    response.insert(QStringLiteral("id"), QStringLiteral("conv-123"));
                } else if (path.endsWith("/responses")) {
                    response.insert(QStringLiteral("output"), QJsonArray{QJsonObject{{QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("output_text")},
                                                                                                                                         {QStringLiteral("text"), QStringLiteral("response answer")}}}}}});
                } else if (path.endsWith("/chat/completions")) {
                    response.insert(QStringLiteral("choices"), QJsonArray{QJsonObject{{QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), QStringLiteral("chat answer")}}}}});
                } else {
                    response.insert(QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("anthropic answer")}}});
                }
                const QByteArray body = QJsonDocument(response).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }
};

class AiProviderTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void protocols()
    {
        FakeAiServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        AiProvider provider;
        QSignalSpy completed(&provider, &AiProvider::completed);
        QSignalSpy failed(&provider, &AiProvider::failed);

        AiConversation history;
        history.messages.append(AiMessage{QStringLiteral("user"), QStringLiteral("earlier question"), 0, QStringLiteral("earlier page text"), {}, {}});
        history.messages.append(AiMessage{QStringLiteral("assistant"), QStringLiteral("earlier answer"), -1, {}, {}, {}});
        AiMessage current{QStringLiteral("user"), QStringLiteral("explain equation 1.2"), 1, QStringLiteral("equation 1.2"), {}, QStringLiteral("Zm9v")};
        history.messages.append(current);

        AiProfile profile;
        profile.model = QStringLiteral("test-model");
        profile.apiKey = QStringLiteral("test-key");
        profile.kind = AiProfile::Kind::OpenAiChat;
        profile.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
        provider.send(profile, history, current);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(0).toString(), QStringLiteral("chat answer"));
        QCOMPARE(server.requests.last().path, QByteArray("/v1/chat/completions"));
        const QJsonArray chatMessages = server.requests.last().body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(chatMessages.size(), 4);
        QVERIFY(chatMessages.at(1).toObject().value(QStringLiteral("content")).toArray().at(0).toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("earlier page text")));
        QCOMPARE(chatMessages.at(3).toObject().value(QStringLiteral("content")).toArray().size(), 2);

        profile.kind = AiProfile::Kind::Anthropic;
        profile.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
        provider.send(profile, history, current);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(0).toString(), QStringLiteral("anthropic answer"));
        QCOMPARE(server.requests.last().path, QByteArray("/v1/messages"));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("messages")).toArray().size(), 3);

        profile.kind = AiProfile::Kind::OpenAiResponses;
        profile.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
        provider.send(profile, history, current);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        const QList<QVariant> response = completed.takeFirst();
        QCOMPARE(response.at(0).toString(), QStringLiteral("response answer"));
        QCOMPARE(response.at(1).toString(), QStringLiteral("conv-123"));
        QCOMPARE(server.requests.at(server.requests.size() - 2).path, QByteArray("/v1/conversations"));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("conversation")).toString(), QStringLiteral("conv-123"));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("input")).toArray().at(0).toObject().value(QStringLiteral("content")).toArray().size(), 2);
    }

    void codexSessionResume()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile script(directory.filePath(QStringLiteral("codex")));
        QVERIFY(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$OKULAR_AI_TEST_ARGS\"\ncat >/dev/null\nprintf '%s\\n' '{\"type\":\"thread.started\",\"thread_id\":\"test-thread\"}' '{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"codex answer\"}}'\n");
        script.close();
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray previousPath = qgetenv("PATH");
        QByteArray testPath = QFile::encodeName(directory.path());
        testPath += ':';
        testPath += previousPath;
        qputenv("PATH", testPath);
        qputenv("OKULAR_AI_TEST_ARGS", QFile::encodeName(directory.filePath(QStringLiteral("args.txt"))));

        AiProvider provider;
        QSignalSpy completed(&provider, &AiProvider::completed);
        QSignalSpy failed(&provider, &AiProvider::failed);
        AiProfile profile;
        profile.kind = AiProfile::Kind::Codex;
        AiMessage question{QStringLiteral("user"), QStringLiteral("Why?"), 0, QStringLiteral("page text"), {}, {}};
        AiConversation conversation;
        provider.send(profile, conversation, question);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(1).toString(), QStringLiteral("test-thread"));
        QFile args(directory.filePath(QStringLiteral("args.txt")));
        QVERIFY(args.open(QIODevice::ReadOnly));
        QVERIFY(!args.readAll().contains("resume"));
        args.close();

        conversation.sessionId = QStringLiteral("test-thread");
        provider.send(profile, conversation, question);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(0).toString(), QStringLiteral("codex answer"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QByteArray resumedArgs = args.readAll();
        QVERIFY(resumedArgs.contains("resume"));
        QVERIFY(resumedArgs.contains("test-thread"));
        qputenv("PATH", previousPath);
        qunsetenv("OKULAR_AI_TEST_ARGS");
    }
};

QTEST_GUILESS_MAIN(AiProviderTest)
#include "aiprovidertest.moc"
