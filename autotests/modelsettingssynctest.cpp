/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/modelsettingssync.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>
#include <QTest>
#include <functional>
#include <utility>

class MemorySettingsStore : public BookObjectStore
{
public:
    mutable QMap<QString, QByteArray> objects;
    mutable std::function<void()> beforeListing;

    S3Response getObject(const QString &key) const override
    {
        return objects.contains(key) ? S3Response {200, objects.value(key), {}} : S3Response {404, {}, {}};
    }
    S3Response putObjectIfAbsent(const QString &key, const QByteArray &body) const override
    {
        if (objects.contains(key))
            return {412, {}, {}};
        objects.insert(key, body);
        return {200, {}, {}};
    }
    S3Response putFileIfAbsent(const QString &, const QString &, const QString &) const override
    {
        return {0, {}, QStringLiteral("Unused")};
    }
    S3Response downloadFile(const QString &, const QString &, const QString &) const override
    {
        return {0, {}, QStringLiteral("Unused")};
    }
    bool listObjects(const QString &prefix, QStringList *keys, QString *error) const override
    {
        if (beforeListing) {
            auto callback = std::move(beforeListing);
            beforeListing = {};
            callback();
        }
        keys->clear();
        for (auto it = objects.cbegin(); it != objects.cend(); ++it)
            if (it.key().startsWith(prefix))
                keys->append(it.key());
        error->clear();
        return true;
    }
};

class ModelSettingsSyncTest : public QObject
{
    Q_OBJECT
private:
    static ModelSettingsSyncResult synced(const BookObjectStore &store, const QString &root, const QList<AiProfile> &profiles)
    {
        ModelSettingsSyncResult result = ModelSettingsSync::synchronize(store, root, profiles);
        if (result.successful()) {
            ModelSettingsSync::commit(root, result, &result.error);
        }
        return result;
    }

    static ModelSettingsSyncResult resolved(const BookObjectStore &store, const QString &root, const QList<AiProfile> &profiles, const QString &key, const QStringList &heads, const QString &chosen)
    {
        ModelSettingsSyncResult result = ModelSettingsSync::resolveConflict(store, root, profiles, key, heads, chosen);
        if (result.successful()) {
            ModelSettingsSync::commit(root, result, &result.error);
        }
        return result;
    }

    static void writeSettings(const QString &root, const QString &hash, const QString &profile, const QString &prompt)
    {
        QFile file(QDir(root).filePath(QStringLiteral("books/%1/ai-settings.json").arg(hash)));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = QJsonDocument(QJsonObject {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("selectedProfileId"), profile}, {QStringLiteral("defaultPrompt"), prompt}}).toJson();
        QCOMPARE(file.write(bytes), bytes.size());
    }

private Q_SLOTS:
    void doesNotOverwriteBookSettingsChangedDuringSync()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString hash(64, QLatin1Char('d'));
        const QString rootA = temp.filePath(QStringLiteral("a"));
        const QString rootB = temp.filePath(QStringLiteral("b"));
        QVERIFY(QDir().mkpath(QDir(rootA).filePath(QStringLiteral("books/") + hash)));
        QVERIFY(QDir().mkpath(QDir(rootB).filePath(QStringLiteral("books/") + hash)));
        MemorySettingsStore store;
        writeSettings(rootA, hash, QString(), QStringLiteral("Remote prompt"));
        const auto a = synced(store, rootA, {});
        QVERIFY2(a.successful(), qPrintable(a.error));
        store.beforeListing = [&] { writeSettings(rootB, hash, QString(), QStringLiteral("Local prompt")); };
        const auto b = ModelSettingsSync::synchronize(store, rootB, {});
        QVERIFY(!b.successful());
        QFile local(QDir(rootB).filePath(QStringLiteral("books/%1/ai-settings.json").arg(hash)));
        QVERIFY(local.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(local.readAll()).object().value(QStringLiteral("defaultPrompt")).toString(), QStringLiteral("Local prompt"));
    }

    void rejectsCredentialsInEndpoint()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        MemorySettingsStore store;
        AiProfile profile {QStringLiteral("unsafe"), QStringLiteral("Unsafe"), AiProfile::Kind::OpenAiChat, QStringLiteral("https://user:password@example.org/v1"), {}, {}, true, {}};
        const auto result = ModelSettingsSync::synchronize(store, temp.path(), {profile});
        QVERIFY(!result.successful());
        QVERIFY(store.objects.isEmpty());
    }

    void retryBeforeSavingProfilesDoesNotDeleteRemoteProfile()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString rootA = temp.filePath(QStringLiteral("a"));
        const QString rootB = temp.filePath(QStringLiteral("b"));
        QVERIFY(QDir().mkpath(rootA));
        QVERIFY(QDir().mkpath(rootB));
        MemorySettingsStore store;
        AiProfile profile {QStringLiteral("shared"), QStringLiteral("Shared"), AiProfile::Kind::Codex, {}, {}, {}, true, {}};
        auto a = synced(store, rootA, {profile});
        QVERIFY2(a.successful(), qPrintable(a.error));
        const int eventCount = store.objects.size();
        auto b = ModelSettingsSync::synchronize(store, rootB, {});
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.profiles.size(), 1);
        QVERIFY(!QFile::exists(QDir(rootB).filePath(QStringLiteral("model-settings-sync.json"))));
        b = ModelSettingsSync::synchronize(store, rootB, {});
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.profiles.size(), 1);
        QCOMPARE(store.objects.size(), eventCount);
    }

    void syncsPublicProfilesAndBookFieldsWithoutSecrets()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString hash(64, QLatin1Char('a'));
        const QString rootA = temp.filePath(QStringLiteral("a"));
        const QString rootB = temp.filePath(QStringLiteral("b"));
        QVERIFY(QDir().mkpath(QDir(rootA).filePath(QStringLiteral("books/") + hash)));
        QVERIFY(QDir().mkpath(QDir(rootB).filePath(QStringLiteral("books/") + hash)));
        writeSettings(rootA, hash, QStringLiteral("model-one"), QStringLiteral("Answer in Chinese"));
        AiProfile profile {
            QStringLiteral("model-one"), QStringLiteral("Model One"), AiProfile::Kind::OpenAiChat, QStringLiteral("https://example.com/v1"), QStringLiteral("a-model"), QStringLiteral("{}"), true, QStringLiteral("TOP-SECRET")};
        MemorySettingsStore store;
        auto a = synced(store, rootA, {profile});
        QVERIFY2(a.successful(), qPrintable(a.error));
        QVERIFY(a.uploaded >= 3);
        for (const QByteArray &body : std::as_const(store.objects))
            QVERIFY(!body.contains("TOP-SECRET"));
        auto b = synced(store, rootB, {});
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.profiles.size(), 1);
        QCOMPARE(b.profiles.first().id, profile.id);
        QVERIFY(b.profiles.first().apiKey.isEmpty());
        QFile downloaded(QDir(rootB).filePath(QStringLiteral("books/%1/ai-settings.json").arg(hash)));
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(downloaded.readAll()).object().value(QStringLiteral("defaultPrompt")).toString(), QStringLiteral("Answer in Chinese"));
        downloaded.close();

        writeSettings(rootA, hash, QStringLiteral("other-model"), QStringLiteral("Answer in Chinese"));
        writeSettings(rootB, hash, QStringLiteral("model-one"), QStringLiteral("Be brief"));
        a = synced(store, rootA, {profile});
        QVERIFY2(a.successful(), qPrintable(a.error));
        b = synced(store, rootB, b.profiles);
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.conflicts, 0);
        QVERIFY(downloaded.open(QIODevice::ReadOnly));
        const QJsonObject merged = QJsonDocument::fromJson(downloaded.readAll()).object();
        QCOMPARE(merged.value(QStringLiteral("selectedProfileId")).toString(), QStringLiteral("other-model"));
        QCOMPARE(merged.value(QStringLiteral("defaultPrompt")).toString(), QStringLiteral("Be brief"));
    }

    void resolvesSameFieldConflict()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString hash(64, QLatin1Char('b'));
        const QString rootA = temp.filePath(QStringLiteral("a"));
        const QString rootB = temp.filePath(QStringLiteral("b"));
        QVERIFY(QDir().mkpath(QDir(rootA).filePath(QStringLiteral("books/") + hash)));
        QVERIFY(QDir().mkpath(QDir(rootB).filePath(QStringLiteral("books/") + hash)));
        MemorySettingsStore store;
        writeSettings(rootA, hash, QString(), QStringLiteral("Original"));
        auto a = synced(store, rootA, {});
        QVERIFY2(a.successful(), qPrintable(a.error));
        auto b = synced(store, rootB, {});
        QVERIFY2(b.successful(), qPrintable(b.error));
        writeSettings(rootA, hash, QString(), QStringLiteral("First"));
        writeSettings(rootB, hash, QString(), QStringLiteral("Second"));
        a = synced(store, rootA, {});
        QVERIFY2(a.successful(), qPrintable(a.error));
        b = synced(store, rootB, {});
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.conflicts, 1);
        QFile file(QDir(rootB).filePath(QStringLiteral("model-settings-conflicts.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject conflict = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("conflicts")).toArray().first().toObject();
        const QJsonArray variants = conflict.value(QStringLiteral("variants")).toArray();
        QCOMPARE(variants.size(), 2);
        QStringList heads;
        QString chosen;
        for (const QJsonValue &variant : variants) {
            heads.append(variant.toObject().value(QStringLiteral("eventId")).toString());
            if (variant.toObject().value(QStringLiteral("value")).toString() == QLatin1String("First"))
                chosen = heads.last();
        }
        QVERIFY(!chosen.isEmpty());
        auto stale = resolved(store, rootB, {}, conflict.value(QStringLiteral("key")).toString(), {QStringLiteral("not-a-head")}, chosen);
        QVERIFY(!stale.successful());
        b = resolved(store, rootB, {}, conflict.value(QStringLiteral("key")).toString(), heads, chosen);
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.conflicts, 0);
        QFile selected(QDir(rootB).filePath(QStringLiteral("books/%1/ai-settings.json").arg(hash)));
        QVERIFY(selected.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(selected.readAll()).object().value(QStringLiteral("defaultPrompt")).toString(), QStringLiteral("First"));
    }

    void resolvesProfileConflictWithoutUploadingKey()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString rootA = temp.filePath(QStringLiteral("a"));
        const QString rootB = temp.filePath(QStringLiteral("b"));
        QVERIFY(QDir().mkpath(rootA));
        QVERIFY(QDir().mkpath(rootB));
        MemorySettingsStore store;
        AiProfile profile {QStringLiteral("shared"), QStringLiteral("Shared"), AiProfile::Kind::OpenAiChat, QString(), QStringLiteral("original"), QString(), true, QStringLiteral("device-a-key")};
        auto a = synced(store, rootA, {profile});
        QVERIFY2(a.successful(), qPrintable(a.error));
        auto b = synced(store, rootB, {});
        QVERIFY2(b.successful(), qPrintable(b.error));
        profile.model = QStringLiteral("first");
        a = synced(store, rootA, {profile});
        QVERIFY2(a.successful(), qPrintable(a.error));
        AiProfile other = b.profiles.first();
        other.model = QStringLiteral("second");
        other.apiKey = QStringLiteral("device-b-key");
        b = synced(store, rootB, {other});
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.conflicts, 1);
        QFile file(QDir(rootB).filePath(QStringLiteral("model-settings-conflicts.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject conflict = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("conflicts")).toArray().first().toObject();
        QStringList heads;
        QString chosen;
        for (const QJsonValue &variant : conflict.value(QStringLiteral("variants")).toArray()) {
            const QJsonObject item = variant.toObject();
            heads.append(item.value(QStringLiteral("eventId")).toString());
            if (item.value(QStringLiteral("value")).toObject().value(QStringLiteral("model")).toString() == QLatin1String("first"))
                chosen = heads.last();
        }
        QVERIFY(!chosen.isEmpty());
        b = resolved(store, rootB, {other}, conflict.value(QStringLiteral("key")).toString(), heads, chosen);
        QVERIFY2(b.successful(), qPrintable(b.error));
        QCOMPARE(b.conflicts, 0);
        QCOMPARE(b.profiles.first().model, QStringLiteral("first"));
        QVERIFY(b.profiles.first().apiKey.isEmpty());
        for (const QByteArray &body : std::as_const(store.objects)) {
            QVERIFY(!body.contains("device-a-key"));
            QVERIFY(!body.contains("device-b-key"));
        }
    }
};

QTEST_GUILESS_MAIN(ModelSettingsSyncTest)
#include "modelsettingssynctest.moc"
