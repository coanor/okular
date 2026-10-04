/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/booklibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QTemporaryDir>
#include <QTest>

#include <future>

class BookLibraryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void importsChangesMadeWhileWaitingForLibrary()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString root = temp.filePath(QStringLiteral("library"));
        const QString original = temp.filePath(QStringLiteral("original.pdf"));
        QFile source(original);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("original"), 8);
        source.close();
        BookProject existing;
        QString error;
        QVERIFY2(BookLibrary::importFile(original, root, &existing, &error), qPrintable(error));
        QVERIFY(QFile::copy(existing.sourcePath, original));
        QLockFile lock(QDir(root).filePath(QStringLiteral(".sync.lock")));
        QVERIFY(lock.tryLock());
        std::promise<void> started;
        auto ready = started.get_future();
        auto imported = std::async(std::launch::async, [&] {
            started.set_value();
            BookProject project;
            QString failure;
            const bool success = BookLibrary::importFile(original, root, &project, &failure);
            return qMakePair(success, project);
        });
        ready.wait();
        // Give the importer time to reach the held library lock.
        QTest::qWait(200);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(source.write("changed"), 7);
        source.close();
        lock.unlock();
        const auto result = imported.get();
        QVERIFY(result.first);
        QVERIFY(result.second.id != existing.id);
        QFile managed(result.second.sourcePath);
        QVERIFY(managed.open(QIODevice::ReadOnly));
        QCOMPARE(managed.readAll(), QByteArray("changed"));
    }

    void importsAndDeduplicatesByContent()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString libraryRoot = temp.filePath(QStringLiteral("library"));
        const QString firstPath = temp.filePath(QStringLiteral("first.pdf"));
        QFile first(firstPath);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QCOMPARE(first.write("book contents"), 13);
        first.close();

        BookProject project;
        QString error;
        QVERIFY2(BookLibrary::importFile(firstPath, libraryRoot, &project, &error), qPrintable(error));
        QVERIFY(!QFileInfo::exists(firstPath));
        QVERIFY(QFileInfo::exists(project.sourcePath));
        QCOMPARE(project.originalName, QStringLiteral("first.pdf"));
        QCOMPARE(project.id.size(), 64);

        const QString duplicatePath = temp.filePath(QStringLiteral("duplicate.pdf"));
        QVERIFY(QFile::copy(project.sourcePath, duplicatePath));
        BookProject duplicate;
        QVERIFY2(BookLibrary::importFile(duplicatePath, libraryRoot, &duplicate, &error), qPrintable(error));
        QCOMPARE(duplicate.id, project.id);
        QCOMPARE(duplicate.sourcePath, project.sourcePath);
        QVERIFY(!QFileInfo::exists(duplicatePath));

        const QString changedPath = temp.filePath(QStringLiteral("changed.pdf"));
        QFile changed(changedPath);
        QVERIFY(changed.open(QIODevice::WriteOnly));
        QCOMPARE(changed.write("book contents 2"), 15);
        changed.close();
        BookProject changedProject;
        QVERIFY2(BookLibrary::importFile(changedPath, libraryRoot, &changedProject, &error), qPrintable(error));
        QVERIFY(changedProject.id != project.id);
        QVERIFY(changedProject.sourcePath != project.sourcePath);
    }

    void failedImportPreservesSource()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString sourcePath = temp.filePath(QStringLiteral("book.pdf"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("test"), 4);
        source.close();
        QString error;
        QVERIFY(!BookLibrary::importFile(sourcePath, {}, nullptr, &error));
        QVERIFY(QFileInfo::exists(sourcePath));
    }

    void corruptExistingProjectDoesNotConsumeSource()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString sourcePath = temp.filePath(QStringLiteral("book.pdf"));
        QFile source(sourcePath);
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write("test"), 4);
        source.close();
        BookProject project;
        QString error;
        const QString libraryRoot = temp.filePath(QStringLiteral("library"));
        QVERIFY2(BookLibrary::importFile(sourcePath, libraryRoot, &project, &error), qPrintable(error));

        const QString duplicatePath = temp.filePath(QStringLiteral("duplicate.pdf"));
        QVERIFY(QFile::copy(project.sourcePath, duplicatePath));
        const QString manifestPath = QFileInfo(project.sourcePath).dir().filePath(QStringLiteral("manifest.json"));
        QVERIFY(QFile::remove(manifestPath));
        QVERIFY(!BookLibrary::importFile(duplicatePath, libraryRoot, nullptr, &error));
        QVERIFY(QFileInfo::exists(duplicatePath));
        QVERIFY(QFileInfo::exists(project.sourcePath));
    }
};

QTEST_MAIN(BookLibraryTest)
#include "booklibrarytest.moc"
