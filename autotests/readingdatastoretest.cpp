/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../core/annotationsidecar_p.h"
#include "../core/readingdatastore_p.h"
#include "../part/aiconversationstore.h"
#include "../part/readinghistory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUuid>
#include <atomic>

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
    void stagedUrlsNeverBindAnotherDeviceBook()
    {
        ReadingHistory history(m_path);
        const QString path = m_directory.filePath(QStringLiteral("unavailable.pdf"));
        QFile::remove(path);
        auto original = ReadingHistory::record(QUrl::fromLocalFile(path), {}, 9, 30);
        QString error;
        QVERIFY(history.save(original, &error));
        QVERIFY(sql(QStringLiteral("UPDATE legacy_reading_history SET device_id = 'original-device'")));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("a different device's book") > 0);
        file.close();
        ReadingRecord loaded;
        QVERIFY(history.read(original.url, &loaded, &error));
        QVERIFY(loaded.url.isEmpty());
        QList<ReadingRecord> books;
        QVERIFY(history.entries(&books, &error));
        QCOMPARE(books.size(), 1);
        QVERIFY(books.first().url.isEmpty());
        QCOMPARE(books.first().page, 9);
        QVERIFY(history.save(ReadingHistory::record(original.url, {}, 0, 10), &error));
        QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM legacy_reading_history")), QStringLiteral("1"));
    }
    void upgradesSchemaOneWithoutChangingProgress()
    {
        QVERIFY(sql(QStringLiteral("DROP TABLE legacy_reading_history")));
        QVERIFY(sql(
            QStringLiteral("CREATE TABLE legacy_reading_history(url TEXT PRIMARY KEY, title TEXT NOT NULL, book_id TEXT NOT NULL, page INTEGER NOT NULL, page_count INTEGER NOT NULL, updated_at INTEGER NOT NULL, revision TEXT NOT NULL)")));
        QVERIFY(sql(QStringLiteral("INSERT INTO legacy_reading_history VALUES ('file:///old.pdf', 'old.pdf', '', 5, 20, 1, 'revision')")));
        QVERIFY(sql(QStringLiteral("INSERT INTO book_locations VALUES ('source-device', 'file:///known.pdf', '%1')").arg(m_hash)));
        QVERIFY(sql(QStringLiteral("UPDATE reading_data_metadata SET value = '1' WHERE key = 'schema_version'")));
        QString error;
        QVERIFY2(Okular::ReadingDataStore::initialize(m_database, &error), qPrintable(error));
        QCOMPARE(scalar(QStringLiteral("SELECT value FROM reading_data_metadata WHERE key = 'schema_version'")), QStringLiteral("2"));
        QCOMPARE(scalar(QStringLiteral("SELECT device_id FROM legacy_reading_history")), QStringLiteral("source-device"));
        QCOMPARE(scalar(QStringLiteral("SELECT page FROM legacy_reading_history")), QStringLiteral("5"));
    }
    void annotationOnlyRecordsDoNotCreateBlankBookshelfRows()
    {
        QString error;
        QVERIFY(Okular::AnnotationSidecar::save(m_hash, {{QStringLiteral("note"), 0, 1, QStringLiteral("<annotation/>")}}, &error));
        QList<ReadingRecord> books;
        QVERIFY(ReadingHistory(m_path).entries(&books, &error));
        QVERIFY(books.isEmpty());
    }
    void corruptUnrelatedAnnotationsDoNotBlockMigration()
    {
        const QString root = m_directory.filePath(QStringLiteral("corrupt-annotations"));
        QVERIFY(QDir().mkpath(root));
        QFile file(QDir(root).filePath(m_hash + QStringLiteral(".sqlite")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("corrupt database") > 0);
        file.close();
        QString error;
        QVERIFY(Okular::ReadingDataStore::importAnnotations(m_database, root, &error));
        QVERIFY(!Okular::ReadingDataStore::importAnnotations(m_database, root, &error, m_hash));
        QVERIFY(!error.isEmpty());
    }
    void migrationDomainsFailIndependently()
    {
        const QString previousName = QCoreApplication::applicationName();
        QCoreApplication::setApplicationName(QUuid::createUuid().toString());
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(QDir().mkpath(root));
        const QString source = QDir(root).filePath(QStringLiteral("ai-history.sqlite"));
        const auto cleanup = qScopeGuard([previousName, source, root] {
            QFile::remove(source);
            QDir().rmdir(root);
            QCoreApplication::setApplicationName(previousName);
        });
        QFile file(source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("corrupt AI database") > 0);
        file.close();
        QString error;
        QVERIFY2(Okular::ReadingDataStore::importLegacyData(m_database, Okular::ReadingDataStore::LegacyData::ReadingHistory, &error), qPrintable(error));
        QVERIFY(!Okular::ReadingDataStore::importLegacyData(m_database, Okular::ReadingDataStore::LegacyData::AiHistory, &error));
        QVERIFY(!error.isEmpty());
    }
    void aiMigrationReadsOneSourceSnapshot()
    {
        const QString legacy =
            createLegacy(QStringLiteral("concurrent-ai.sqlite"),
                         {QStringLiteral("PRAGMA journal_mode=WAL"),
                          QStringLiteral("CREATE TABLE ai_conversations(document_key, profile_id, session_id, instructions)"),
                          QStringLiteral("CREATE TABLE ai_messages(document_key, profile_id, ordinal, role, content, page, page_text, selected_text, page_image)"),
                          QStringLiteral("INSERT INTO ai_conversations VALUES ('%1', 'profile', '0', '')").arg(m_hash),
                          QStringLiteral("WITH RECURSIVE rows(n) AS (VALUES(0) UNION ALL SELECT n + 1 FROM rows WHERE n < 500) INSERT INTO ai_messages SELECT '%1', 'profile', n, 'user', '0', 0, '', '', '' FROM rows").arg(m_hash)});
        QVERIFY(!legacy.isEmpty());
        std::atomic<bool> stop = false;
        std::atomic<int> completedWrites = 0;
        auto *writer = QThread::create([&stop, &completedWrites, legacy] {
            const QString name = QUuid::createUuid().toString();
            {
                auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
                database.setDatabaseName(legacy);
                database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
                if (database.open()) {
                    int version = 0;
                    while (!stop) {
                        QSqlQuery query(database);
                        if (!database.transaction()) {
                            break;
                        }
                        const QString value = QString::number(++version);
                        if (!query.exec(QStringLiteral("UPDATE ai_conversations SET session_id = '%1'").arg(value)) || !query.exec(QStringLiteral("UPDATE ai_messages SET content = '%1'").arg(value)) || !database.commit()) {
                            database.rollback();
                            break;
                        }
                        ++completedWrites;
                    }
                }
            }
            QSqlDatabase::removeDatabase(name);
        });
        const auto cleanup = qScopeGuard([&stop, writer] {
            stop = true;
            writer->wait();
            delete writer;
        });
        writer->start();
        QTRY_VERIFY_WITH_TIMEOUT(completedWrites > 0, 5000);
        for (int i = 0; i < 5; ++i) {
            QVERIFY(sql(QStringLiteral("DELETE FROM ai_messages")));
            QVERIFY(sql(QStringLiteral("DELETE FROM ai_conversations")));
            QString error;
            QVERIFY2(Okular::ReadingDataStore::importAiHistory(m_database, legacy, &error), qPrintable(error));
            const QString version = scalar(QStringLiteral("SELECT session_id FROM ai_conversations"));
            QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM ai_messages WHERE content <> '%1'").arg(version)), QStringLiteral("0"));
            QCOMPARE(scalar(QStringLiteral("SELECT COUNT(*) FROM ai_messages")), QStringLiteral("501"));
        }
    }
    void exportRejectsSymlinkToActiveDatabase()
    {
#ifdef Q_OS_UNIX
        const QString alias = m_directory.filePath(QStringLiteral("database-link.sqlite"));
        QVERIFY(QFile::link(m_path, alias));
        qputenv("OKULAR_READING_DATA_PATH", alias.toUtf8());
        QString error;
        QVERIFY(!Okular::ReadingDataStore::exportDatabase(QUrl::fromLocalFile(alias), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(scalar(QStringLiteral("PRAGMA integrity_check")), QStringLiteral("ok"));
#else
        QSKIP("This case requires Unix symlinks");
#endif
    }
    void exportAtomicallyReplacesDestination()
    {
        const QString destination = m_directory.filePath(QStringLiteral("replace.sqlite"));
        QFile old(destination);
        QVERIFY(old.open(QIODevice::WriteOnly));
        QVERIFY(old.write("old backup") > 0);
        old.close();
        QString error;
        QVERIFY(Okular::ReadingDataStore::ensureBook(m_database, m_hash, QStringLiteral("book.pdf"), &error));
        QVERIFY2(Okular::ReadingDataStore::exportDatabase(QUrl::fromLocalFile(destination), &error), qPrintable(error));
        m_database.close();
        m_database.setDatabaseName(destination);
        QVERIFY(m_database.open());
        QCOMPARE(scalar(QStringLiteral("SELECT file_name FROM books")), QStringLiteral("book.pdf"));
        QCOMPARE(scalar(QStringLiteral("PRAGMA integrity_check")), QStringLiteral("ok"));
    }
    void exportRejectsActiveDatabaseAndSidecars()
    {
        QStringList paths {m_path, m_path + QStringLiteral("-wal"), m_path + QStringLiteral("-shm"), m_path + QStringLiteral("-journal")};
#ifdef Q_OS_WIN
        const QStringList originals = paths;
        for (const QString &path : originals) {
            paths.append(path.toUpper());
        }
#endif
        QString error;
        for (const QString &path : paths) {
            QVERIFY(!Okular::ReadingDataStore::exportDatabase(QUrl::fromLocalFile(path), &error));
            QVERIFY(!error.isEmpty());
        }
        QCOMPARE(scalar(QStringLiteral("PRAGMA integrity_check")), QStringLiteral("ok"));
    }
    void exportHandlesUnicodePaths()
    {
        m_database.close();
        const QString unicodePath = m_directory.filePath(QStringLiteral("阅读记录.sqlite"));
        QVERIFY(QFile::rename(m_path, unicodePath));
        m_path = unicodePath;
        m_database.setDatabaseName(m_path);
        QVERIFY(m_database.open());
        qputenv("OKULAR_READING_DATA_PATH", m_path.toUtf8());
        QString error;
        QVERIFY(Okular::ReadingDataStore::ensureBook(m_database, m_hash, QStringLiteral("book.pdf"), &error));
        const QString destination = m_directory.filePath(QStringLiteral("导出/书架.sqlite"));
        QVERIFY(QDir().mkpath(QFileInfo(destination).absolutePath()));
        QVERIFY2(Okular::ReadingDataStore::exportDatabase(QUrl::fromLocalFile(destination), &error), qPrintable(error));
        m_database.close();
        m_database.setDatabaseName(destination);
        QVERIFY(m_database.open());
        QCOMPARE(scalar(QStringLiteral("SELECT file_name FROM books")), QStringLiteral("book.pdf"));
        QCOMPARE(scalar(QStringLiteral("PRAGMA integrity_check")), QStringLiteral("ok"));
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
