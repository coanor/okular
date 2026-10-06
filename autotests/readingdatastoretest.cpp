/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../core/annotationsidecar_p.h"
#include "../core/readingdatastore_p.h"
#include "../part/aiconversationstore.h"
#include "../part/readinghistory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class ReadingDataStoreTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_directory;
    QSqlDatabase m_database;
    QString m_path;
    QString m_hash = QString(64, QLatin1Char('a'));

    bool sql(const QString &statement)
    {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            qWarning() << query.lastError();
            return false;
        }
        return true;
    }
    QString scalar(const QString &statement)
    {
        QSqlQuery query(m_database);
        if (!query.exec(statement) || !query.next()) {
            return {};
        }
        return query.value(0).toString();
    }
    QString createLegacy(const QString &name, const QStringList &statements)
    {
        const QString path = m_directory.filePath(name);
        const QString connection = QUuid::createUuid().toString();
        bool ok;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            database.setDatabaseName(path);
            ok = database.open();
            QSqlQuery query(database);
            for (const QString &statement : statements) {
                ok = ok && query.exec(statement);
            }
        }
        QSqlDatabase::removeDatabase(connection);
        return ok ? path : QString();
    }
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }
    void init()
    {
        m_path = m_directory.filePath(QUuid::createUuid().toString() + QStringLiteral(".sqlite"));
        qputenv("OKULAR_READING_DATA_PATH", m_path.toUtf8());
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("shared-store-test"));
        m_database.setDatabaseName(m_path);
        QVERIFY(m_database.open());
        QString error;
        QVERIFY2(Okular::ReadingDataStore::initialize(m_database, &error), qPrintable(error));
    }
    void cleanup()
    {
        m_database = {};
        QSqlDatabase::removeDatabase(QStringLiteral("shared-store-test"));
        qunsetenv("OKULAR_READING_DATA_PATH");
    }
    void oneDatabaseAndBinaryImages()
    {
        QCOMPARE(ReadingHistory::defaultPath(), m_path);
        QCOMPARE(AiConversationStore::defaultPath(), m_path);
        QCOMPARE(Okular::AnnotationSidecar::pathForHash(m_hash), m_path);
        const QByteArray image = QByteArray::fromHex("ffd8000100ff00ffd9");
        AiConversation conversation;
        conversation.messages.append({QStringLiteral("user"), QStringLiteral("Question"), 1, {}, {}, QString::fromLatin1(image.toBase64())});
        QString error;
        AiConversationStore store(m_path, {});
        QVERIFY2(store.save(m_hash, QStringLiteral("profile"), conversation, &error), qPrintable(error));
        QCOMPARE(scalar(QStringLiteral("SELECT typeof(page_image) FROM ai_messages")), QStringLiteral("blob"));
        QCOMPARE(scalar(QStringLiteral("SELECT hex(page_image) FROM ai_messages")), QString::fromLatin1(image.toHex().toUpper()));
        AiConversation loaded;
        QVERIFY(store.load(m_hash, QStringLiteral("profile"), &loaded, &error));
        QCOMPARE(QByteArray::fromBase64(loaded.messages.first().pageImage.toLatin1()), image);
    }
    void progressFollowsBytesAcrossPathsAndDevices()
    {
        const QString first = m_directory.filePath(QStringLiteral("original.pdf"));
        const QString second = m_directory.filePath(QStringLiteral("renamed.pdf"));
        QFile file(first);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("same book bytes"), 15);
        file.close();
        QFile::remove(second);
        QVERIFY(QFile::copy(first, second));
        QString error;
        ReadingHistory history(m_path);
        const auto record = ReadingHistory::record(QUrl::fromLocalFile(first), {}, 8, 20);
        QVERIFY2(history.save(record, &error), qPrintable(error));
        // Simulate a copied database: source-device locations cannot be used.
        QVERIFY(sql(QStringLiteral("UPDATE book_locations SET device_id = 'other-device'")));
        QList<ReadingRecord> books;
        QVERIFY(history.entries(&books, &error));
        QCOMPARE(books.size(), 1);
        QVERIFY(books.first().url.isEmpty());
        ReadingRecord loaded;
        QVERIFY2(history.read(QUrl::fromLocalFile(second), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.page, 8);
        QCOMPARE(loaded.bookId, books.first().bookId);
        auto updated = ReadingHistory::record(QUrl::fromLocalFile(second), {}, 9, 20);
        updated.updatedAt = record.updatedAt + 1;
        QVERIFY(history.save(updated, &error));
        QVERIFY(history.entries(&books, &error));
        QCOMPARE(books.size(), 1);
        QCOMPARE(books.first().url, QUrl::fromLocalFile(second));
        QVERIFY(file.open(QIODevice::Append));
        QVERIFY(file.write("changed") > 0);
        file.close();
        QVERIFY(history.read(QUrl::fromLocalFile(first), &loaded, &error));
        QVERIFY(loaded.bookId.isEmpty());
    }
    void annotationsAndRevisionsAreBookScoped()
    {
        const QString secondHash(64, QLatin1Char('b'));
        const Okular::SidecarAnnotation note {QStringLiteral("same-id"), 0, 1, QStringLiteral("<annotation/>")};
        QString error;
        qint64 firstRevision = 0;
        qint64 secondRevision = 0;
        QVERIFY2(Okular::AnnotationSidecar::save(m_hash, {note}, &error, 0, &firstRevision), qPrintable(error));
        QVERIFY(Okular::AnnotationSidecar::save(secondHash, {note}, &error, 0, &secondRevision));
        QCOMPARE(firstRevision, 1);
        QCOMPARE(secondRevision, 1);
        QVERIFY(Okular::AnnotationSidecar::save(m_hash, {}, &error, firstRevision, &firstRevision));
        QCOMPARE(firstRevision, 2);
        QVERIFY(!Okular::AnnotationSidecar::save(m_hash, {note}, &error, 1));
        QList<Okular::SidecarAnnotation> notes;
        QVERIFY(Okular::AnnotationSidecar::load(m_hash, &notes, &error));
        QVERIFY(notes.isEmpty());
        QVERIFY(Okular::AnnotationSidecar::load(secondHash, &notes, &error, &secondRevision));
        QCOMPARE(notes.size(), 1);
        QCOMPARE(secondRevision, 1);
    }
    void migrateAiTextImageWithoutRevivingClearedHistory()
    {
        const QString legacy = createLegacy(QStringLiteral("old-ai.sqlite"),
                                            {QStringLiteral("CREATE TABLE ai_conversations(document_key, profile_id, session_id, instructions)"),
                                             QStringLiteral("CREATE TABLE ai_messages(document_key, profile_id, ordinal, role, content, page, page_text, selected_text, page_image TEXT)"),
                                             QStringLiteral("INSERT INTO ai_conversations VALUES ('%1', 'profile', 'session', 'prompt')").arg(m_hash),
                                             QStringLiteral("INSERT INTO ai_messages VALUES ('%1', 'profile', 0, 'user', 'old question', 2, '', '', 'anBlZw==')").arg(m_hash)});
        QVERIFY(!legacy.isEmpty());
        QString error;
        QVERIFY2(Okular::ReadingDataStore::importAiHistory(m_database, legacy, &error), qPrintable(error));
        QCOMPARE(scalar(QStringLiteral("SELECT hex(page_image) FROM ai_messages")), QStringLiteral("6A706567"));
        QCOMPARE(scalar(QStringLiteral("SELECT typeof(page_image) FROM ai_messages")), QStringLiteral("blob"));
        AiConversationStore store(m_path, {});
        QVERIFY(store.clear(m_hash, QStringLiteral("profile"), &error));
        QVERIFY2(Okular::ReadingDataStore::importAiHistory(m_database, legacy, &error), qPrintable(error));
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM ai_messages")), QStringLiteral("0"));
        QVERIFY(QFile::exists(legacy));
    }
    void migrateAnnotationsAndKeepDeletion()
    {
        const QString root = m_directory.filePath(QStringLiteral("old-annotations"));
        QVERIFY(QDir().mkpath(root));
        const QString legacy = createLegacy(QStringLiteral("old-annotations/") + m_hash + QStringLiteral(".sqlite"),
                                            {QStringLiteral("CREATE TABLE metadata(key, value)"),
                                             QStringLiteral("INSERT INTO metadata VALUES ('schema_version', '2'), ('pdf_sha256', '%1')").arg(m_hash),
                                             QStringLiteral("CREATE TABLE events(sequence, annotation_id, page, subtype, xml, operation, recorded_utc)"),
                                             QStringLiteral("CREATE TABLE annotations(page, annotation_id, subtype, contents, author, color, hidden_native, xml)"),
                                             QStringLiteral("INSERT INTO events VALUES (1, 'note', 0, 1, '<annotation/>', 'upsert', '2026-01-01')"),
                                             QStringLiteral("INSERT INTO annotations VALUES (0, 'note', 1, 'note text', '', '', 0, '<annotation/>')")});
        QVERIFY(!legacy.isEmpty());
        QString error;
        QVERIFY2(Okular::ReadingDataStore::importAnnotations(m_database, root, &error), qPrintable(error));
        QList<Okular::SidecarAnnotation> notes;
        qint64 revision = 0;
        QVERIFY(Okular::AnnotationSidecar::load(m_hash, &notes, &error, &revision));
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().contents, QStringLiteral("note text"));
        QVERIFY(Okular::AnnotationSidecar::save(m_hash, {}, &error, revision));
        QVERIFY(Okular::ReadingDataStore::importAnnotations(m_database, root, &error));
        QVERIFY(Okular::AnnotationSidecar::load(m_hash, &notes, &error));
        QVERIFY(notes.isEmpty());
        QVERIFY(QFile::exists(legacy));
    }
    void migrationFailureRollsBack()
    {
        const QString legacy =
            createLegacy(QStringLiteral("broken-ai.sqlite"),
                         {QStringLiteral("CREATE TABLE ai_conversations(document_key, profile_id, session_id, instructions)"), QStringLiteral("INSERT INTO ai_conversations VALUES ('%1', 'profile', 'session', '')").arg(m_hash)});
        QVERIFY(!legacy.isEmpty());
        QString error;
        QVERIFY(!Okular::ReadingDataStore::importAiHistory(m_database, legacy, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM ai_conversations")), QStringLiteral("0"));
        QVERIFY(QFile::exists(legacy));
    }
    void legacyProgressWaitsForFileAccess()
    {
        const QString filePath = m_directory.filePath(QStringLiteral("later.pdf"));
        const QString url = QUrl::fromLocalFile(filePath).toString(QUrl::FullyEncoded);
        const QString revision = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString legacy = createLegacy(
            QStringLiteral("old-progress.sqlite"),
            {QStringLiteral("CREATE TABLE reading_history(url, title, book_id, page, page_count, updated_at, revision)"), QStringLiteral("INSERT INTO reading_history VALUES ('%1', 'later.pdf', '', 7, 30, 10, '%2')").arg(url, revision)});
        QString error;
        QVERIFY(Okular::ReadingDataStore::importReadingHistory(m_database, legacy, &error));
        ReadingHistory history(m_path);
        QList<ReadingRecord> books;
        QVERIFY(history.entries(&books, &error));
        QCOMPARE(books.size(), 1);
        QCOMPARE(books.first().page, 7);
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("book now available") > 0);
        file.close();
        ReadingRecord loaded;
        QVERIFY(history.read(QUrl(url), &loaded, &error));
        QCOMPARE(loaded.page, 7);
        QVERIFY(Okular::ReadingDataStore::isBookHash(loaded.bookId));
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM legacy_reading_history")), QStringLiteral("0"));
        QVERIFY(QFile::exists(legacy));
    }
    void snapshotIncludesWalAndAllTables()
    {
        QVERIFY(sql(QStringLiteral("PRAGMA journal_mode=WAL")));
        QString error;
        QVERIFY(Okular::ReadingDataStore::ensureBook(m_database, m_hash, QStringLiteral("book.pdf"), &error));
        QVERIFY(sql(QStringLiteral("INSERT INTO reading_progress VALUES ('%1', 4, 10, 1, 'revision')").arg(m_hash)));
        QVERIFY(sql(QStringLiteral("INSERT INTO ai_conversations VALUES ('%1', 'profile', '', '')").arg(m_hash)));
        const QString destination = m_directory.filePath(QStringLiteral("export.sqlite"));
        QVERIFY2(Okular::ReadingDataStore::snapshot(m_path, destination, &error), qPrintable(error));
        QVERIFY(!Okular::ReadingDataStore::snapshot(m_path, destination, &error));
        m_database.close();
        m_database.setDatabaseName(destination);
        QVERIFY(m_database.open());
        QCOMPARE(scalar(QStringLiteral("SELECT file_name FROM books")), QStringLiteral("book.pdf"));
        QCOMPARE(scalar(QStringLiteral("SELECT page FROM reading_progress")), QStringLiteral("4"));
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM ai_conversations")), QStringLiteral("1"));
        QCOMPARE(scalar(QStringLiteral("PRAGMA integrity_check")), QStringLiteral("ok"));
    }
};
QTEST_GUILESS_MAIN(ReadingDataStoreTest)
#include "readingdatastoretest.moc"
