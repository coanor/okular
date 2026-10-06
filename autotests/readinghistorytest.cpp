/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/readinghistory.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

class ReadingHistoryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void resolvesProviderFileNamesAndPreservesProgress()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("history.sqlite"));
        const QUrl url(QStringLiteral("content://com.android.providers.downloads.documents/document/msf%3A42"));
        ReadingHistory history(path, [url](const QUrl &source) { return source == url ? QStringLiteral("A real book.pdf") : QString(); });
        QString error;
        const ReadingRecord original = ReadingHistory::record(url, {}, 12, 100);
        QVERIFY(history.save(original, &error));
        QList<ReadingRecord> books;
        QVERIFY(history.entries(&books, &error));
        QCOMPARE(books.size(), 1);
        QCOMPARE(books.first().title, QStringLiteral("A real book.pdf"));
        ReadingRecord saved;
        QVERIFY(ReadingHistory(path).read(url, &saved, &error));
        QCOMPARE(saved.title, QStringLiteral("A real book.pdf"));
        QCOMPARE(saved.page, original.page);
        QCOMPARE(saved.updatedAt, original.updatedAt);
        QCOMPARE(saved.revision, original.revision);
    }

    void providerNameIsStoredForContentHash()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("history.sqlite"));
        const QUrl url(QStringLiteral("content://downloads/document/msf%3A42"));
        ReadingHistory history(path, [](const QUrl &) { return QStringLiteral("Real name.pdf"); });
        auto record = ReadingHistory::record(url, {}, 2, 10);
        record.bookId = QString(64, QLatin1Char('a'));
        QString error;
        QVERIFY2(history.save(record, &error), qPrintable(error));
        ReadingRecord saved;
        QVERIFY(history.read(record.bookId, url, &saved, &error));
        QCOMPARE(saved.title, QStringLiteral("Real name.pdf"));
        // Losing provider access must not overwrite the last resolved name.
        record.updatedAt++;
        QVERIFY(ReadingHistory(path, [](const QUrl &) { return QString(); }).save(record, &error));
        QVERIFY(history.read(record.bookId, url, &saved, &error));
        QCOMPARE(saved.title, QStringLiteral("Real name.pdf"));
    }

    void persistsLocalAndContentUrls()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("history.sqlite"));
        QString error;
        const QUrl local = QUrl::fromLocalFile(directory.filePath(QStringLiteral("a book.pdf")));
        const QUrl content(QStringLiteral("content://documents/tree/primary%3ABooks/document/primary%3ABooks%2Fbook.pdf"));
        auto first = ReadingHistory::record(local, QStringLiteral("First book"), 20, 100);
        auto second = ReadingHistory::record(content, QStringLiteral("Second book"), 4, 10);
        second.updatedAt = first.updatedAt + 1;
        {
            ReadingHistory history(path);
            QVERIFY2(history.save(first, &error), qPrintable(error));
            QVERIFY2(history.save(second, &error), qPrintable(error));
        }
        ReadingHistory reopened(path);
        ReadingRecord saved;
        QVERIFY(reopened.read(local, &saved, &error));
        QCOMPARE(saved.page, 20);
        QCOMPARE(saved.pageCount, 100);
        QCOMPARE(saved.title, first.title);
        QVERIFY(reopened.read(content, &saved, &error));
        QCOMPARE(saved.url, content);
        QCOMPARE(saved.page, 4);
        QList<ReadingRecord> entries;
        QVERIFY(reopened.entries(&entries, &error));
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().url, content);
    }

    void newerPositionWinsIncludingBackwardNavigation()
    {
        QTemporaryDir directory;
        ReadingHistory history(directory.filePath(QStringLiteral("history.sqlite")));
        QString error;
        auto older = ReadingHistory::record(QUrl(QStringLiteral("file:///book.pdf")), QStringLiteral("Book"), 50, 100);
        auto newer = older;
        newer.updatedAt++;
        newer.page = 5;
        QVERIFY(history.save(newer, &error));
        QVERIFY(history.save(older, &error));
        ReadingRecord saved;
        QVERIFY(history.read(older.url, &saved, &error));
        QCOMPARE(saved.page, 5);
        QList<ReadingRecord> entries;
        QVERIFY(history.entries(&entries, &error));
        QCOMPARE(entries.size(), 1);
    }

    void rejectsTransientDescriptorsAndInvalidPages()
    {
        QTemporaryDir directory;
        ReadingHistory history(directory.filePath(QStringLiteral("history.sqlite")));
        QString error;
        QVERIFY(!history.save(ReadingHistory::record(QUrl(QStringLiteral("fd:///42")), {}, 1, 5), &error));
        QVERIFY(!history.save(ReadingHistory::record(QUrl(QStringLiteral("file:///book.pdf")), {}, 5, 5), &error));
        QVERIFY(!history.save(ReadingHistory::record(QUrl(QStringLiteral("file:///book.pdf")), {}, -1, 5), &error));
        ReadingRecord saved;
        QVERIFY(history.read(QUrl(QStringLiteral("file:///missing.pdf")), &saved, &error));
        QVERIFY(saved.url.isEmpty());
    }

    void importsLegacyDownloadedBooksWithoutReplacingProgress()
    {
        QTemporaryDir directory;
        ReadingHistory history(directory.filePath(QStringLiteral("history.sqlite")));
        QString error;
        const QString id = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("original book"), QCryptographicHash::Sha256).toHex());
        const QString root = directory.filePath(QStringLiteral("library"));
        const QDir project(QDir(root).filePath(QStringLiteral("books/") + id));
        QVERIFY(QDir().mkpath(project.path()));
        QFile source(project.filePath(QStringLiteral("source.pdf")));
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write("original book") > 0);
        source.close();
        QFile manifest(project.filePath(QStringLiteral("manifest.json")));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        const QByteArray data =
            QJsonDocument(QJsonObject {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("sha256"), id}, {QStringLiteral("storedName"), QStringLiteral("source.pdf")}, {QStringLiteral("originalName"), QStringLiteral("Downloaded book")}})
                .toJson();
        QCOMPARE(manifest.write(data), data.size());
        manifest.close();
        QVERIFY2(history.importLegacyLibrary(root, &error), qPrintable(error));
        const QUrl url = QUrl::fromLocalFile(source.fileName());
        ReadingRecord imported;
        QVERIFY(history.read(url, &imported, &error));
        QCOMPARE(imported.bookId, id);
        QCOMPARE(imported.title, QStringLiteral("Downloaded book"));
        QCOMPARE(imported.pageCount, 0);
        QVERIFY(QFile::exists(source.fileName()));
        imported = ReadingHistory::record(url, imported.title, 5, 100);
        QVERIFY(history.save(imported, &error));
        {
            ReadingHistory reopened(directory.filePath(QStringLiteral("history.sqlite")));
            QVERIFY(reopened.importLegacyLibrary(root, &error));
            ReadingRecord saved;
            QVERIFY(reopened.read(url, &saved, &error));
            QCOMPARE(saved.page, 5);
            QCOMPARE(saved.updatedAt, imported.updatedAt);
            QCOMPARE(saved.revision, imported.revision);
            QList<ReadingRecord> entries;
            QVERIFY(reopened.entries(&entries, &error));
            QCOMPARE(entries.size(), 1);
        }
    }

    void rejectsInvalidLegacyBookPaths_data()
    {
        QTest::addColumn<QString>("storedName");
        QTest::addColumn<QString>("id");
        QTest::newRow("traversal") << QStringLiteral("../outside.pdf") << QString(64, QLatin1Char('a'));
        QTest::newRow("absolute") << QStringLiteral("/outside.pdf") << QString(64, QLatin1Char('a'));
        QTest::newRow("backslash") << QStringLiteral("..\\outside.pdf") << QString(64, QLatin1Char('a'));
        QTest::newRow("empty") << QString() << QString(64, QLatin1Char('a'));
        QTest::newRow("invalid identity") << QStringLiteral("source.pdf") << QStringLiteral("invalid-id");
        QTest::newRow("missing source") << QStringLiteral("missing.pdf") << QString(64, QLatin1Char('a'));
    }

    void rejectsInvalidLegacyBookPaths()
    {
        QFETCH(QString, storedName);
        QFETCH(QString, id);
        QTemporaryDir directory;
        const QDir books(directory.filePath(QStringLiteral("books")));
        const QDir project(books.filePath(id));
        QVERIFY(QDir().mkpath(project.path()));
        for (const QString &path : {books.filePath(QStringLiteral("outside.pdf")), project.filePath(QStringLiteral("source.pdf")), project.filePath(QStringLiteral("..\\outside.pdf"))}) {
            QFile source(path);
            QVERIFY(source.open(QIODevice::WriteOnly));
            source.close();
        }
        QFile manifest(project.filePath(QStringLiteral("manifest.json")));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        const QByteArray data = QJsonDocument(QJsonObject {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("sha256"), id}, {QStringLiteral("storedName"), storedName}}).toJson();
        QCOMPARE(manifest.write(data), data.size());
        manifest.close();
        ReadingHistory history(directory.filePath(QStringLiteral("history.sqlite")));
        QString error;
        QVERIFY(history.importLegacyLibrary(directory.path(), &error));
        QList<ReadingRecord> entries;
        QVERIFY(history.entries(&entries, &error));
        QVERIFY(entries.isEmpty());
    }

    void reportsDatabaseErrors()
    {
        QTemporaryDir directory;
        ReadingHistory history(directory.path());
        QList<ReadingRecord> entries;
        QString error;
        QVERIFY(!history.entries(&entries, &error));
        QVERIFY(!error.isEmpty());
    }

    void preservesLegacyEntriesWithUnknownProgress()
    {
        QTemporaryDir directory;
        ReadingHistory history(directory.filePath(QStringLiteral("history.sqlite")));
        QString error;
        const auto legacy = ReadingHistory::record(QUrl(QStringLiteral("file:///old-book.pdf")), QStringLiteral("Old book"), 0, 0);
        QVERIFY(history.save(legacy, &error));
        ReadingRecord saved;
        QVERIFY(history.read(legacy.url, &saved, &error));
        QCOMPARE(saved.title, legacy.title);
        QCOMPARE(saved.pageCount, 0);
        QVERIFY(!history.save(ReadingHistory::record(legacy.url, legacy.title, 1, 0), &error));
    }
};

QTEST_GUILESS_MAIN(ReadingHistoryTest)
#include "readinghistorytest.moc"
