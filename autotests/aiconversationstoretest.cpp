/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/aiconversationstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

class AiConversationStoreTest : public QObject
{
    Q_OBJECT

    static AiConversation sample()
    {
        return {
            QStringLiteral("remote-session"),
            QStringLiteral("Answer in Chinese"),
            {{QStringLiteral("user"), QStringLiteral("Explain this"), 3, QStringLiteral("Page text"), QStringLiteral("Selection"), QStringLiteral("anBlZw==")}, {QStringLiteral("assistant"), QStringLiteral("Answer\n中文"), 3, {}, {}, {}}}};
    }

    static QString writeLegacy(const QString &directory, const QByteArray &bytes)
    {
        QDir().mkpath(directory);
        const QString profileHash = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("model"), QCryptographicHash::Sha256).toHex());
        const QString path = QDir(directory).filePath(QString(64, QLatin1Char('a')) + QLatin1Char('-') + profileHash + QStringLiteral(".json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            return {};
        }
        return path;
    }

private Q_SLOTS:
    void persistsAndIsolatesBooksAndProfiles()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("history.sqlite"));
        QString error;
        const AiConversation original = sample();
        {
            AiConversationStore store(path, {});
            QVERIFY2(store.save(QString(64, QLatin1Char('a')), QStringLiteral("model"), original, &error), qPrintable(error));
            QVERIFY(store.save(QString(64, QLatin1Char('b')), QStringLiteral("model"), {}, &error));
            QVERIFY(store.save(QString(64, QLatin1Char('a')), QStringLiteral("other-model"), {}, &error));
        }
        AiConversationStore reopened(path, {});
        AiConversation loaded;
        QVERIFY2(reopened.load(QString(64, QLatin1Char('a')), QStringLiteral("model"), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.sessionId, original.sessionId);
        QCOMPARE(loaded.instructions, original.instructions);
        QCOMPARE(loaded.messages.size(), 2);
        QCOMPARE(loaded.messages[0].role, QStringLiteral("user"));
        QCOMPARE(loaded.messages[0].page, 3);
        QCOMPARE(loaded.messages[0].pageText, original.messages[0].pageText);
        QCOMPARE(loaded.messages[0].selectedText, original.messages[0].selectedText);
        QCOMPARE(loaded.messages[0].pageImage, original.messages[0].pageImage);
        QCOMPARE(loaded.messages[1].content, original.messages[1].content);
        QVERIFY(reopened.load(QString(64, QLatin1Char('a')), QStringLiteral("other-model"), &loaded, &error));
        QVERIFY(loaded.messages.isEmpty());
        QVERIFY(reopened.load(QString(64, QLatin1Char('b')), QStringLiteral("model"), &loaded, &error));
        QVERIFY(loaded.messages.isEmpty());
    }

    void replacesAndClearsOnlySelectedConversation()
    {
        QTemporaryDir directory;
        AiConversationStore store(directory.filePath(QStringLiteral("history.sqlite")), directory.filePath(QStringLiteral("legacy")));
        QString error;
        auto conversation = sample();
        QVERIFY(store.save(QString(64, QLatin1Char('a')), QStringLiteral("model"), conversation, &error));
        QVERIFY(store.save(QString(64, QLatin1Char('a')), QStringLiteral("other-model"), conversation, &error));
        conversation.messages.removeLast();
        conversation.messages[0].pageImage.clear();
        QVERIFY(store.save(QString(64, QLatin1Char('a')), QStringLiteral("model"), conversation, &error));
        AiConversation loaded;
        QVERIFY(store.load(QString(64, QLatin1Char('a')), QStringLiteral("model"), &loaded, &error));
        QCOMPARE(loaded.messages.size(), 1);
        QVERIFY(loaded.messages[0].pageImage.isEmpty());
        QVERIFY(store.clear(QString(64, QLatin1Char('a')), QStringLiteral("model"), &error));
        // Even an old file restored from backup must not revive a cleared record.
        QVERIFY(!writeLegacy(directory.filePath(QStringLiteral("legacy")), QByteArrayLiteral("{\"version\":1,\"sessionId\":\"old\",\"messages\":[]}")).isEmpty());
        QVERIFY(store.load(QString(64, QLatin1Char('a')), QStringLiteral("model"), &loaded, &error));
        QVERIFY(loaded.messages.isEmpty());
        QVERIFY(loaded.sessionId.isEmpty());
        QVERIFY(loaded.instructions.isEmpty());
        QVERIFY(store.load(QString(64, QLatin1Char('a')), QStringLiteral("other-model"), &loaded, &error));
        QCOMPARE(loaded.messages.size(), 2);
    }

    void migratesLegacyJson()
    {
        QTemporaryDir directory;
        const QString legacyDirectory = directory.filePath(QStringLiteral("legacy"));
        const QString legacy = writeLegacy(legacyDirectory,
                                           QByteArrayLiteral("{\"version\":1,\"sessionId\":\"session\",\"instructions\":\"Prompt\",\"messages\":["
                                                             "{\"role\":\"user\",\"content\":\"Question\",\"page\":2,\"pageText\":\"Text\",\"selectedText\":\"Selected\",\"pageImage\":\"anBlZw==\"},"
                                                             "{\"role\":\"assistant\",\"content\":\"Answer\"},{\"role\":\"invalid\",\"content\":\"Ignored\"}]}"));
        QVERIFY(!legacy.isEmpty());
        const QString path = directory.filePath(QStringLiteral("history.sqlite"));
        QString error;
        AiConversation loaded;
        {
            AiConversationStore store(path, legacyDirectory);
            QVERIFY2(store.load(QString(64, QLatin1Char('a')), QStringLiteral("model"), &loaded, &error), qPrintable(error));
        }
        QVERIFY(QFile::exists(legacy));
        AiConversationStore reopened(path, legacyDirectory);
        QVERIFY(reopened.load(QString(64, QLatin1Char('a')), QStringLiteral("model"), &loaded, &error));
        QCOMPARE(loaded.sessionId, QStringLiteral("session"));
        QCOMPARE(loaded.instructions, QStringLiteral("Prompt"));
        QCOMPARE(loaded.messages.size(), 2);
        QCOMPARE(loaded.messages[0].page, 2);
        QCOMPARE(loaded.messages[0].pageText, QStringLiteral("Text"));
        QCOMPARE(loaded.messages[0].selectedText, QStringLiteral("Selected"));
        QCOMPARE(loaded.messages[0].pageImage, QStringLiteral("anBlZw=="));
        QCOMPARE(loaded.messages[1].content, QStringLiteral("Answer"));
    }

    void failedMigrationKeepsLegacyFile()
    {
        QTemporaryDir directory;
        const QString legacyDirectory = directory.filePath(QStringLiteral("legacy"));
        const QString legacy = writeLegacy(legacyDirectory, QByteArrayLiteral("invalid JSON"));
        QVERIFY(!legacy.isEmpty());
        AiConversationStore store(directory.filePath(QStringLiteral("history.sqlite")), legacyDirectory);
        AiConversation conversation;
        QString error;
        QVERIFY(!store.load(QString(64, QLatin1Char('a')), QStringLiteral("model"), &conversation, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::exists(legacy));
        AiConversationStore unavailable(directory.path(), legacyDirectory);
        QVERIFY(!unavailable.save(QString(64, QLatin1Char('a')), QStringLiteral("model"), sample(), &error));
        QVERIFY(QFile::exists(legacy));
    }

    void failedWriteRollsBackMetadataAndMessages()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("history.sqlite"));
        AiConversationStore store(path, {});
        QString error;
        QVERIFY(store.save(QString(64, QLatin1Char('a')), QStringLiteral("model"), sample(), &error));
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("failure-trigger"));
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QStringLiteral("CREATE TRIGGER reject_message BEFORE INSERT ON ai_messages WHEN NEW.content = 'reject' BEGIN SELECT RAISE(ABORT, 'write failed'); END")));
        }
        QSqlDatabase::removeDatabase(QStringLiteral("failure-trigger"));
        auto changed = sample();
        changed.sessionId = QStringLiteral("new-session");
        changed.instructions = QStringLiteral("New instructions");
        changed.messages[1].content = QStringLiteral("reject");
        QVERIFY(!store.save(QString(64, QLatin1Char('a')), QStringLiteral("model"), changed, &error));
        QVERIFY(!error.isEmpty());
        AiConversation loaded;
        QVERIFY(store.load(QString(64, QLatin1Char('a')), QStringLiteral("model"), &loaded, &error));
        QCOMPARE(loaded.sessionId, sample().sessionId);
        QCOMPARE(loaded.instructions, sample().instructions);
        QCOMPARE(loaded.messages.size(), 2);
        QCOMPARE(loaded.messages[1].content, sample().messages[1].content);
    }

    void missingHistoryRetainsDefaultPromptAndRejectsMissingIdentity()
    {
        QTemporaryDir directory;
        AiConversationStore store(directory.filePath(QStringLiteral("history.sqlite")), {});
        AiConversation conversation;
        conversation.instructions = QStringLiteral("Default prompt");
        QString error;
        QVERIFY(store.load(QString(64, QLatin1Char('c')), QStringLiteral("model"), &conversation, &error));
        QCOMPARE(conversation.instructions, QStringLiteral("Default prompt"));
        QVERIFY(!store.save({}, QStringLiteral("model"), conversation, &error));
        QVERIFY(!store.clear(QString(64, QLatin1Char('a')), {}, &error));
        QVERIFY(!store.load({}, QStringLiteral("model"), &conversation, &error));
    }
};

QTEST_GUILESS_MAIN(AiConversationStoreTest)
#include "aiconversationstoretest.moc"
