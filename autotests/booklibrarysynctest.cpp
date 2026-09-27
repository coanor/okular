/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/booklibrarysync.h"
#include "../part/booklibrary.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>

class MemoryBookStore : public BookObjectStore
{
public:
    mutable QMap<QString, QByteArray> objects;
    bool ignoreConditionalWrites = false;

    S3Response getObject(const QString &key) const override
    {
        return objects.contains(key) ? S3Response {200, objects.value(key), {}} : S3Response {404, {}, {}};
    }

    S3Response putObjectIfAbsent(const QString &key, const QByteArray &body) const override
    {
        if (objects.contains(key) && !ignoreConditionalWrites) {
            return {412, {}, {}};
        }
        objects.insert(key, body);
        return {200, {}, {}};
    }

    S3Response putFileIfAbsent(const QString &key, const QString &sourcePath, const QString &expectedSha256) const override
    {
        QFile file(sourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return {0, {}, QStringLiteral("Cannot read source")};
        }
        const QByteArray contents = file.readAll();
        if (QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()) != expectedSha256) {
            return {0, {}, QStringLiteral("Source hash mismatch")};
        }
        return putObjectIfAbsent(key, contents);
    }

    S3Response downloadFile(const QString &key, const QString &destinationPath, const QString &expectedSha256) const override
    {
        if (!objects.contains(key)) {
            return {404, {}, {}};
        }
        const QByteArray contents = objects.value(key);
        if (QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()) != expectedSha256) {
            return {200, {}, QStringLiteral("Hash mismatch")};
        }
        QSaveFile file(destinationPath);
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
            return {0, {}, QStringLiteral("Cannot save download")};
        }
        return {200, {}, {}};
    }

    bool listObjects(const QString &prefix, QStringList *keys, QString *error) const override
    {
        keys->clear();
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            if (it.key().startsWith(prefix)) {
                keys->append(it.key());
            }
        }
        if (error) {
            error->clear();
        }
        return true;
    }
};

class BookLibrarySyncTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void uploadsAndDownloadsDistinctBooks()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString firstSource = temp.filePath(QStringLiteral("first.pdf"));
        QFile first(firstSource);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QCOMPARE(first.write("first book"), 10);
        first.close();
        const QString rootA = temp.filePath(QStringLiteral("device-a"));
        const QString rootB = temp.filePath(QStringLiteral("device-b"));
        BookProject firstProject;
        QString error;
        QVERIFY2(BookLibrary::importFile(firstSource, rootA, &firstProject, &error), qPrintable(error));

        MemoryBookStore store;
        const BookSyncResult firstSync = BookLibrarySync(store, rootA).synchronizeSources();
        QVERIFY2(firstSync.successful(), qPrintable(firstSync.error));
        QCOMPARE(firstSync.uploaded, 1);
        QVERIFY(store.objects.contains(QStringLiteral("version.json")));
        QCOMPARE(store.objects.value(QStringLiteral("books/%1/source").arg(firstProject.id)), QByteArray("first book"));

        const BookSyncResult secondSync = BookLibrarySync(store, rootB).synchronizeSources();
        QVERIFY2(secondSync.successful(), qPrintable(secondSync.error));
        QCOMPARE(secondSync.downloaded, 1);
        const QString downloadedPath = QDir(rootB).filePath(QStringLiteral("books/%1/source.pdf").arg(firstProject.id));
        QFile downloaded(downloadedPath);
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(downloaded.readAll(), QByteArray("first book"));
        QCOMPARE(BookLibrarySync(store, rootB).synchronizeSources().downloaded, 0);

        const QString changedSource = temp.filePath(QStringLiteral("changed.pdf"));
        QFile changed(changedSource);
        QVERIFY(changed.open(QIODevice::WriteOnly));
        QCOMPARE(changed.write("changed book"), 12);
        changed.close();
        BookProject changedProject;
        QVERIFY2(BookLibrary::importFile(changedSource, rootA, &changedProject, &error), qPrintable(error));
        QVERIFY(changedProject.id != firstProject.id);
        const BookSyncResult added = BookLibrarySync(store, rootA).synchronizeSources();
        QVERIFY2(added.successful(), qPrintable(added.error));
        QCOMPARE(added.uploaded, 1);
        const BookSyncResult received = BookLibrarySync(store, rootB).synchronizeSources();
        QVERIFY2(received.successful(), qPrintable(received.error));
        QCOMPARE(received.downloaded, 1);
        QVERIFY(QFile::exists(QDir(rootB).filePath(QStringLiteral("books/%1/source.pdf").arg(changedProject.id))));

        store.objects.insert(QStringLiteral("books/%1/source").arg(changedProject.id), QByteArray("corrupt bytes"));
        const QString rootC = temp.filePath(QStringLiteral("device-c"));
        const BookSyncResult corrupt = BookLibrarySync(store, rootC).synchronizeSources();
        QVERIFY(!corrupt.successful());
        QVERIFY(!QFile::exists(QDir(rootC).filePath(QStringLiteral("books/%1/source.pdf").arg(changedProject.id))));
    }

    void refusesAnotherCloudLibraryInSameDirectory()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        MemoryBookStore store;
        const QString root = temp.filePath(QStringLiteral("library"));
        QVERIFY(BookLibrarySync(store, root).synchronizeSources().successful());
        store.objects.insert(QStringLiteral("version.json"), QByteArray("{\"schemaVersion\":1,\"libraryId\":\"a0000000-0000-0000-0000-000000000001\"}"));
        const BookSyncResult other = BookLibrarySync(store, root).synchronizeSources();
        QVERIFY(!other.successful());
        QVERIFY(other.error.contains(QStringLiteral("different cloud library")));
    }

    void refusesEndpointWithoutConditionalWrites()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        MemoryBookStore store;
        store.ignoreConditionalWrites = true;
        const BookSyncResult result = BookLibrarySync(store, temp.filePath(QStringLiteral("library"))).synchronizeSources();
        QVERIFY(!result.successful());
        QVERIFY(result.error.contains(QStringLiteral("conditional writes")));
    }

    void refusesChangedManagedSource()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString sourcePath = temp.filePath(QStringLiteral("book.pdf"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("original"), 8);
        source.close();
        const QString root = temp.filePath(QStringLiteral("library"));
        BookProject project;
        QString error;
        QVERIFY2(BookLibrary::importFile(sourcePath, root, &project, &error), qPrintable(error));
        QFile managed(project.sourcePath);
        QVERIFY(managed.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(managed.write("modified"), 8);
        managed.close();
        MemoryBookStore store;
        const BookSyncResult result = BookLibrarySync(store, root).synchronizeSources();
        QVERIFY(!result.successful());
        QVERIFY(!store.objects.contains(QStringLiteral("books/%1/manifest.json").arg(project.id)));
    }
};

QTEST_GUILESS_MAIN(BookLibrarySyncTest)
#include "booklibrarysynctest.moc"
