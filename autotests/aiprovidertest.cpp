/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/aiprovider.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

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
                    response.insert(QStringLiteral("output"),
                                    QJsonArray {QJsonObject {{QStringLiteral("content"), QJsonArray {QJsonObject {{QStringLiteral("type"), QStringLiteral("output_text")}, {QStringLiteral("text"), QStringLiteral("response answer")}}}}}});
                } else if (path.endsWith("/chat/completions")) {
                    response.insert(QStringLiteral("choices"), QJsonArray {QJsonObject {{QStringLiteral("message"), QJsonObject {{QStringLiteral("content"), QStringLiteral("chat answer")}}}}});
                } else {
                    response.insert(QStringLiteral("content"), QJsonArray {QJsonObject {{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), QStringLiteral("anthropic answer")}}});
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
        history.instructions = QStringLiteral("Answer in Chinese with a patient tone.");
        history.messages.append(AiMessage {QStringLiteral("user"), QStringLiteral("earlier question"), 0, QStringLiteral("earlier page text"), {}, {}});
        history.messages.append(AiMessage {QStringLiteral("assistant"), QStringLiteral("earlier answer"), -1, {}, {}, {}});
        AiMessage current {QStringLiteral("user"), QStringLiteral("explain equation 1.2"), 1, QStringLiteral("equation 1.2"), {}, QStringLiteral("Zm9v")};
        history.messages.append(current);

        AiProfile profile;
        profile.model = QStringLiteral("test-model");
        profile.apiKey = QStringLiteral("test-key");
        profile.kind = AiProfile::Kind::OpenAiChat;
        profile.extraArguments = QStringLiteral("{\"temperature\":0.2,\"model\":\"ignored\"}");
        profile.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
        provider.send(profile, history, current);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(0).toString(), QStringLiteral("chat answer"));
        QCOMPARE(server.requests.last().path, QByteArray("/v1/chat/completions"));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("temperature")).toDouble(), 0.2);
        QCOMPARE(server.requests.last().body.value(QStringLiteral("model")).toString(), QStringLiteral("test-model"));
        const QJsonArray chatMessages = server.requests.last().body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(chatMessages.size(), 4);
        QVERIFY(chatMessages.first().toObject().value(QStringLiteral("content")).toString().contains(history.instructions));
        QVERIFY(chatMessages.at(1).toObject().value(QStringLiteral("content")).toArray().at(0).toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("earlier page text")));
        QCOMPARE(chatMessages.at(3).toObject().value(QStringLiteral("content")).toArray().size(), 2);

        profile.kind = AiProfile::Kind::Anthropic;
        profile.extraArguments = QStringLiteral("{\"max_tokens\":512}");
        profile.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
        provider.send(profile, history, current);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(0).toString(), QStringLiteral("anthropic answer"));
        QCOMPARE(server.requests.last().path, QByteArray("/v1/messages"));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("max_tokens")).toInt(), 512);
        QVERIFY(server.requests.last().body.value(QStringLiteral("system")).toString().contains(history.instructions));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("messages")).toArray().size(), 3);

        profile.kind = AiProfile::Kind::OpenAiResponses;
        profile.extraArguments = QStringLiteral("{\"reasoning\":{\"effort\":\"low\"}}");
        profile.endpoint = QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort());
        provider.send(profile, history, current);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        const QList<QVariant> response = completed.takeFirst();
        QCOMPARE(response.at(0).toString(), QStringLiteral("response answer"));
        QCOMPARE(response.at(1).toString(), QStringLiteral("conv-123"));
        QCOMPARE(server.requests.at(server.requests.size() - 2).path, QByteArray("/v1/conversations"));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("conversation")).toString(), QStringLiteral("conv-123"));
        QVERIFY(server.requests.last().body.value(QStringLiteral("instructions")).toString().contains(history.instructions));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("reasoning")).toObject().value(QStringLiteral("effort")).toString(), QStringLiteral("low"));
        QCOMPARE(server.requests.last().body.value(QStringLiteral("input")).toArray().at(0).toObject().value(QStringLiteral("content")).toArray().size(), 2);

        const qsizetype requestCount = server.requests.size();
        profile.kind = AiProfile::Kind::OpenAiChat;
        profile.extraArguments = QStringLiteral("not JSON");
        provider.send(profile, history, current);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(server.requests.size(), requestCount);
    }

    void codexSessionResume()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
#ifdef Q_OS_WIN
        const QString executableName = QStringLiteral("codex.exe");
#else
        const QString executableName = QStringLiteral("codex");
#endif
        QFile script(directory.filePath(executableName));
        QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), script.fileName()));
        QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        const QByteArray previousPath = qgetenv("PATH");
        const auto restoreEnvironment = qScopeGuard([previousPath] {
            qputenv("PATH", previousPath);
            qunsetenv("OKULAR_AI_TEST_ARGS");
            qunsetenv("OKULAR_AI_TEST_STDIN");
        });
        QByteArray testPath = QFile::encodeName(directory.path());
        testPath += QDir::listSeparator().toLatin1();
        testPath += previousPath;
        qputenv("PATH", testPath);
        qputenv("OKULAR_AI_TEST_ARGS", QFile::encodeName(directory.filePath(QStringLiteral("args.txt"))));
        qputenv("OKULAR_AI_TEST_STDIN", QFile::encodeName(directory.filePath(QStringLiteral("stdin.txt"))));
        QCOMPARE(QStandardPaths::findExecutable(QStringLiteral("codex")), script.fileName());

        AiProvider provider;
        QSignalSpy completed(&provider, &AiProvider::completed);
        QSignalSpy failed(&provider, &AiProvider::failed);
        AiProfile profile;
        profile.kind = AiProfile::Kind::Codex;
        AiMessage question {QStringLiteral("user"), QStringLiteral("Why?"), 0, QStringLiteral("page text"), {}, {}};
        AiConversation conversation;
        conversation.instructions = QStringLiteral("Answer in Chinese with a patient tone.");
        provider.send(profile, conversation, question);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(1).toString(), QStringLiteral("test-thread"));
        QFile args(directory.filePath(QStringLiteral("args.txt")));
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QByteArray initialArgs = args.readAll();
        QVERIFY(!initialArgs.contains("resume"));
        QVERIFY(initialArgs.contains("model_reasoning_effort=low"));
        args.close();
        QFile stdinFile(directory.filePath(QStringLiteral("stdin.txt")));
        QVERIFY(stdinFile.open(QIODevice::ReadOnly));
        QVERIFY(stdinFile.readAll().contains(conversation.instructions.toUtf8()));
        stdinFile.close();

        conversation.sessionId = QStringLiteral("test-thread");
        provider.send(profile, conversation, question);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        QCOMPARE(completed.takeFirst().at(0).toString(), QStringLiteral("codex answer"));
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QByteArray resumedArgs = args.readAll();
        QVERIFY(resumedArgs.contains("resume"));
        QVERIFY(resumedArgs.contains("test-thread"));
        QVERIFY(resumedArgs.contains("model_reasoning_effort=low"));
        args.close();
        QVERIFY(stdinFile.open(QIODevice::ReadOnly));
        QVERIFY(stdinFile.readAll().contains(conversation.instructions.toUtf8()));
        stdinFile.close();

        profile.extraArguments = QStringLiteral("-c model_reasoning_effort=medium");
        provider.send(profile, conversation, question);
        QVERIFY(completed.wait(5000));
        QVERIFY(failed.isEmpty());
        completed.takeFirst();
        QVERIFY(args.open(QIODevice::ReadOnly));
        const QByteArray overriddenArgs = args.readAll();
        QVERIFY(overriddenArgs.contains("model_reasoning_effort=medium"));
        QVERIFY(!overriddenArgs.contains("model_reasoning_effort=low"));
    }
};

// Run the copied test executable as a fake CLI, without a platform shell.
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (application.applicationName() == QLatin1String("codex")) {
        QFile args(qEnvironmentVariable("OKULAR_AI_TEST_ARGS"));
        QFile input(qEnvironmentVariable("OKULAR_AI_TEST_STDIN"));
        QFile standardInput;
        QFile standardOutput;
        if (!args.open(QIODevice::WriteOnly) || !input.open(QIODevice::WriteOnly) || !standardInput.open(stdin, QIODevice::ReadOnly) || !standardOutput.open(stdout, QIODevice::WriteOnly)) {
            return 1;
        }
        args.write(application.arguments().mid(1).join(QLatin1Char('\n')).toUtf8());
        input.write(standardInput.readAll());
        standardOutput.write(
            "{\"type\":\"thread.started\",\"thread_id\":\"test-thread\"}\n"
            "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":\"codex answer\"}}\n");
        return 0;
    }
    AiProviderTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}
#include "aiprovidertest.moc"
