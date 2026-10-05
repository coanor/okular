/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/annotationsync.h"
#include "../core/annotationsidecar_p.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>
#include <QTest>

class MemoryAnnotationStore : public BookObjectStore
{
public:
    mutable QMap<QString, QByteArray> objects;
    mutable int eventListings = 0;
    int failEventListing = -1;

    S3Response getObject(const QString &key) const override
    {
        return objects.contains(key) ? S3Response {200, objects.value(key), {}} : S3Response {404, {}, {}};
    }

    S3Response putObjectIfAbsent(const QString &key, const QByteArray &body) const override
    {
        if (objects.contains(key)) {
            return {412, {}, {}};
        }
        objects.insert(key, body);
        return {200, {}, {}};
    }

    S3Response putFileIfAbsent(const QString &key, const QString &path, const QString &expectedHash) const override
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {0, {}, QStringLiteral("Unreadable file")};
        }
        const QByteArray bytes = file.readAll();
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != expectedHash.toLatin1()) {
            return {0, {}, QStringLiteral("Wrong file hash")};
        }
        return putObjectIfAbsent(key, bytes);
    }

    S3Response downloadFile(const QString &key, const QString &path, const QString &expectedHash) const override
    {
        const S3Response response = getObject(key);
        if (!response.successful() || QCryptographicHash::hash(response.body, QCryptographicHash::Sha256).toHex() != expectedHash.toLatin1()) {
            return {0, {}, QStringLiteral("Wrong remote file hash")};
        }
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(response.body) != response.body.size()) {
            return {0, {}, QStringLiteral("Unwritable file")};
        }
        return {200, {}, {}};
    }

    bool listObjects(const QString &prefix, QStringList *keys, QString *error) const override
    {
        if (prefix.contains(QStringLiteral("/annotations/events/")) && ++eventListings == failEventListing) {
            *error = QStringLiteral("Temporary listing failure");
            return false;
        }
        keys->clear();
        for (auto it = objects.cbegin(); it != objects.cend(); ++it) {
            if (it.key().startsWith(prefix)) {
                keys->append(it.key());
            }
        }
        error->clear();
        return true;
    }
};

class AnnotationSyncTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void publishesRevertedValuesAndRepeatedDeletions()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray originalDataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", temp.filePath(QStringLiteral("data")).toUtf8());
        const QString hash = QString::fromLatin1(QCryptographicHash::hash("reverted PDF bytes", QCryptographicHash::Sha256).toHex());
        const QString root = temp.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("books/") + hash)));
        MemoryAnnotationStore store;
        QString error;
        const Okular::SidecarAnnotation original {QStringLiteral("note"), 0, 1, QStringLiteral("<original/>")};
        Okular::SidecarAnnotation changed = original;
        changed.xml = QStringLiteral("<changed/>");
        const QList<QList<Okular::SidecarAnnotation>> states {{original}, {changed}, {original}, {}, {original}, {}};
        for (const auto &state : states) {
            QVERIFY2(Okular::AnnotationSidecar::save(hash, state, &error), qPrintable(error));
            const auto result = AnnotationSync::synchronize(store, root, hash);
            QVERIFY2(result.successful(), qPrintable(result.error));
            QCOMPARE(result.uploaded, 1);
            QCOMPARE(result.applied, 0);
            QCOMPARE(result.conflicts, 0);
            QList<Okular::SidecarAnnotation> loaded;
            QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
            QCOMPARE(loaded.size(), state.size());
            if (!state.isEmpty()) {
                QCOMPARE(loaded.first().xml, state.first().xml);
            }
        }
        qputenv("XDG_DATA_HOME", originalDataHome);
    }

    void resolvesChosenHeadAndRejectsStaleSelection()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray originalDataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", temp.filePath(QStringLiteral("data")).toUtf8());
        const QString hash = QString::fromLatin1(QCryptographicHash::hash("resolve PDF bytes", QCryptographicHash::Sha256).toHex());
        const QString root = temp.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("books/") + hash)));
        MemoryAnnotationStore store;
        QString error;
        Okular::SidecarAnnotation note {QStringLiteral("note"), 0, 1, QStringLiteral("<original/>")};
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {note}, &error), qPrintable(error));
        auto result = AnnotationSync::synchronize(store, root, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        const QString originalHead = store.objects.firstKey().section(QLatin1Char('/'), -1).left(64);

        note.xml = QStringLiteral("<first/>");
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {note}, &error), qPrintable(error));
        result = AnnotationSync::synchronize(store, root, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QString firstHead;
        for (auto it = store.objects.cbegin(); it != store.objects.cend(); ++it) {
            if (it.key().contains(QStringLiteral("/events/")) && QJsonDocument::fromJson(it.value()).object().value(QStringLiteral("parents")).toArray().contains(originalHead)) {
                firstHead = it.key().section(QLatin1Char('/'), -1).left(64);
            }
        }
        QVERIFY(!firstHead.isEmpty());
        note.xml = QStringLiteral("<second/>");
        QJsonObject sibling {{QStringLiteral("schemaVersion"), 1},
                             {QStringLiteral("page"), 0},
                             {QStringLiteral("id"), QStringLiteral("note")},
                             {QStringLiteral("parents"), QJsonArray {originalHead}},
                             {QStringLiteral("value"),
                              QJsonObject {{QStringLiteral("id"), QStringLiteral("note")},
                                           {QStringLiteral("page"), 0},
                                           {QStringLiteral("subtype"), 1},
                                           {QStringLiteral("xml"), note.xml},
                                           {QStringLiteral("contents"), QString()},
                                           {QStringLiteral("author"), QString()},
                                           {QStringLiteral("color"), QString()},
                                           {QStringLiteral("hiddenNative"), false}}}};
        const QByteArray bytes = QJsonDocument(sibling).toJson(QJsonDocument::Compact);
        const QString secondHead = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        store.objects.insert(QStringLiteral("books/%1/annotations/events/%2.json").arg(hash, secondHead), bytes);
        result = AnnotationSync::synchronize(store, root, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.conflicts, 1);
        result = AnnotationSync::resolveConflict(store, root, hash, 0, QStringLiteral("note"), {originalHead, firstHead}, firstHead);
        QVERIFY(!result.successful());
        result = AnnotationSync::resolveConflict(store, root, hash, 0, QStringLiteral("note"), {firstHead, secondHead}, secondHead);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.conflicts, 0);
        QList<Okular::SidecarAnnotation> loaded;
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().xml, note.xml);
        qputenv("XDG_DATA_HOME", originalDataHome);
    }

    void retriesAfterUploadWithoutDuplicatingEvent()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray originalDataHome = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", temp.filePath(QStringLiteral("data")).toUtf8());
        const QString hash = QString::fromLatin1(QCryptographicHash::hash("retry PDF bytes", QCryptographicHash::Sha256).toHex());
        const QString root = temp.filePath(QStringLiteral("library"));
        QVERIFY(QDir().mkpath(QDir(root).filePath(QStringLiteral("books/") + hash)));
        QString error;
        const Okular::SidecarAnnotation annotation {QStringLiteral("retry"), 0, 1, QStringLiteral("<retry/>")};
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {annotation}, &error), qPrintable(error));
        MemoryAnnotationStore store;
        store.failEventListing = 2;
        auto result = AnnotationSync::synchronize(store, root, hash);
        QVERIFY(!result.successful());
        QCOMPARE(result.error, QStringLiteral("Temporary listing failure"));
        QVERIFY(!QFile::exists(QDir(root).filePath(QStringLiteral("books/%1/annotation-sync.json").arg(hash))));

        store.failEventListing = -1;
        result = AnnotationSync::synchronize(store, root, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.uploaded, 0);
        QStringList events;
        QVERIFY(store.listObjects(QStringLiteral("books/%1/annotations/events/").arg(hash), &events, &error));
        QCOMPARE(events.size(), 1);
        qputenv("XDG_DATA_HOME", originalDataHome);
    }

    void mergesIndependentChangesAndRetainsConflicts()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString hash = QString::fromLatin1(QCryptographicHash::hash("same PDF bytes", QCryptographicHash::Sha256).toHex());
        const QString rootA = temp.filePath(QStringLiteral("library-a"));
        const QString rootB = temp.filePath(QStringLiteral("library-b"));
        QVERIFY(QDir().mkpath(QDir(rootA).filePath(QStringLiteral("books/") + hash)));
        QVERIFY(QDir().mkpath(QDir(rootB).filePath(QStringLiteral("books/") + hash)));
        const QByteArray originalDataHome = qgetenv("XDG_DATA_HOME");
        const auto device = [&](const QString &name) {
            qputenv("XDG_DATA_HOME", temp.filePath(name).toUtf8());
            return Okular::AnnotationSidecar::pathForHash(hash);
        };
        const QString pathA = device(QStringLiteral("data-a"));
        const QString pathB = device(QStringLiteral("data-b"));
        QVERIFY(pathA != pathB);
        MemoryAnnotationStore store;
        QString error;
        const Okular::SidecarAnnotation noteA {QStringLiteral("first"), 0, 1, QStringLiteral("<first/>")};
        device(QStringLiteral("data-a"));
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {noteA}, &error), qPrintable(error));
        auto result = AnnotationSync::synchronize(store, rootA, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.uploaded, 1);
        QCOMPARE(result.snapshotsUploaded, 1);

        device(QStringLiteral("data-b"));
        result = AnnotationSync::synchronize(store, rootB, hash, true);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.deferred, 1);
        QList<Okular::SidecarAnnotation> loaded;
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QVERIFY(loaded.isEmpty());
        result = AnnotationSync::synchronize(store, rootB, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.applied, 1);
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().xml, noteA.xml);

        const Okular::SidecarAnnotation noteB {QStringLiteral("second"), 0, 1, QStringLiteral("<second/>")};
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {noteA, noteB}, &error), qPrintable(error));
        device(QStringLiteral("data-a"));
        const Okular::SidecarAnnotation noteC {QStringLiteral("third"), 0, 1, QStringLiteral("<third/>")};
        QVERIFY2(Okular::AnnotationSidecar::save(hash, {noteA, noteC}, &error), qPrintable(error));
        result = AnnotationSync::synchronize(store, rootA, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        device(QStringLiteral("data-b"));
        result = AnnotationSync::synchronize(store, rootB, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.applied, 1);
        QCOMPARE(result.conflicts, 0);
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.size(), 3);
        device(QStringLiteral("data-a"));
        result = AnnotationSync::synchronize(store, rootA, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.applied, 1);
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.size(), 3);

        Okular::SidecarAnnotation changedA = noteA;
        changedA.xml = QStringLiteral("<changed-a/>");
        device(QStringLiteral("data-a"));
        for (auto &annotation : loaded) {
            if (annotation.id == noteA.id) {
                annotation = changedA;
            }
        }
        QVERIFY2(Okular::AnnotationSidecar::save(hash, loaded, &error), qPrintable(error));
        device(QStringLiteral("data-b"));
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        Okular::SidecarAnnotation changedB = noteA;
        changedB.xml = QStringLiteral("<changed-b/>");
        for (auto &annotation : loaded) {
            if (annotation.id == noteA.id) {
                annotation = changedB;
            }
        }
        QVERIFY2(Okular::AnnotationSidecar::save(hash, loaded, &error), qPrintable(error));
        device(QStringLiteral("data-a"));
        result = AnnotationSync::synchronize(store, rootA, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        device(QStringLiteral("data-b"));
        result = AnnotationSync::synchronize(store, rootB, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.conflicts, 1);
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.first().xml, changedB.xml);
        QFile conflictFile(QDir(rootB).filePath(QStringLiteral("books/%1/annotation-conflicts.json").arg(hash)));
        QVERIFY(conflictFile.open(QIODevice::ReadOnly));
        const QJsonObject conflict = QJsonDocument::fromJson(conflictFile.readAll()).object();
        QCOMPARE(conflict.value(QStringLiteral("conflicts")).toArray().size(), 1);
        QCOMPARE(conflict.value(QStringLiteral("conflicts")).toArray().first().toObject().value(QStringLiteral("variants")).toArray().size(), 2);
        Okular::SidecarAnnotation changedAgain = changedB;
        changedAgain.xml = QStringLiteral("<changed-b-again/>");
        for (auto &annotation : loaded) {
            if (annotation.id == noteA.id) {
                annotation = changedAgain;
            }
        }
        QVERIFY2(Okular::AnnotationSidecar::save(hash, loaded, &error), qPrintable(error));
        result = AnnotationSync::synchronize(store, rootB, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.conflicts, 1);
        QFile updatedConflictFile(QDir(rootB).filePath(QStringLiteral("books/%1/annotation-conflicts.json").arg(hash)));
        QVERIFY(updatedConflictFile.open(QIODevice::ReadOnly));
        const QJsonArray currentHeads = QJsonDocument::fromJson(updatedConflictFile.readAll()).object().value(QStringLiteral("conflicts")).toArray().first().toObject().value(QStringLiteral("variants")).toArray();
        QCOMPARE(currentHeads.size(), 2);
        device(QStringLiteral("data-a"));
        result = AnnotationSync::synchronize(store, rootA, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.conflicts, 1);
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.first().xml, changedA.xml);

        device(QStringLiteral("data-b"));
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        loaded.removeIf([](const Okular::SidecarAnnotation &annotation) { return annotation.id == QLatin1String("second"); });
        QVERIFY2(Okular::AnnotationSidecar::save(hash, loaded, &error), qPrintable(error));
        result = AnnotationSync::synchronize(store, rootB, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        device(QStringLiteral("data-a"));
        result = AnnotationSync::synchronize(store, rootA, hash);
        QVERIFY2(result.successful(), qPrintable(result.error));
        QCOMPARE(result.applied, 1);
        QVERIFY2(Okular::AnnotationSidecar::load(hash, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.size(), 2);
        qputenv("XDG_DATA_HOME", originalDataHome);
    }
};

QTEST_GUILESS_MAIN(AnnotationSyncTest)
#include "annotationsynctest.moc"
