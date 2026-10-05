/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "modelsettingssync.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>

#include <utility>

namespace
{
struct Entry {
    QString head;
    QJsonValue value;
};
struct Event {
    QString key;
    QStringList parents;
    QJsonValue value;
};

bool validKey(const QString &key)
{
    static const QRegularExpression profile(QStringLiteral("^profile/[A-Za-z0-9_-]{1,100}$"));
    static const QRegularExpression book(QStringLiteral("^book/[a-f0-9]{64}/(selectedProfileId|defaultPrompt)$"));
    return profile.match(key).hasMatch() || book.match(key).hasMatch();
}

QJsonObject encodeProfile(const AiProfile &profile)
{
    return {{QStringLiteral("id"), profile.id},
            {QStringLiteral("name"), profile.name},
            {QStringLiteral("kind"), static_cast<int>(profile.kind)},
            {QStringLiteral("endpoint"), profile.endpoint},
            {QStringLiteral("model"), profile.model},
            {QStringLiteral("extraArguments"), profile.extraArguments},
            {QStringLiteral("vision"), profile.vision}};
}

bool decodeProfile(const QJsonValue &value, const QString &id, AiProfile *profile)
{
    if (!value.isObject()) {
        return false;
    }
    const QJsonObject object = value.toObject();
    const int kind = object.value(QStringLiteral("kind")).toInt(-1);
    const QUrl endpoint(object.value(QStringLiteral("endpoint")).toString());
    if (object.size() != 7 || object.value(QStringLiteral("id")).toString() != id || !object.value(QStringLiteral("name")).isString() || object.value(QStringLiteral("name")).toString().isEmpty() || kind < 0 || kind > 3 ||
        !object.value(QStringLiteral("endpoint")).isString() || !object.value(QStringLiteral("model")).isString() || !object.value(QStringLiteral("extraArguments")).isString() || !object.value(QStringLiteral("vision")).isBool() ||
        !endpoint.userName().isEmpty() || !endpoint.password().isEmpty()) {
        return false;
    }
    *profile = {id,
                object.value(QStringLiteral("name")).toString(),
                static_cast<AiProfile::Kind>(kind),
                object.value(QStringLiteral("endpoint")).toString(),
                object.value(QStringLiteral("model")).toString(),
                object.value(QStringLiteral("extraArguments")).toString(),
                object.value(QStringLiteral("vision")).toBool(),
                {}};
    return true;
}

bool validValue(const QString &key, const QJsonValue &value)
{
    if (value.isNull()) {
        return key.startsWith(QLatin1String("profile/"));
    }
    if (key.startsWith(QLatin1String("book/"))) {
        return value.isString();
    }
    AiProfile profile;
    return decodeProfile(value, key.mid(8), &profile);
}

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

bool readEvents(const BookObjectStore &store, QMap<QString, Event> *events, QString *error)
{
    QStringList keys;
    const QString prefix = QStringLiteral("settings/events/");
    if (!store.listObjects(prefix, &keys, error)) {
        return false;
    }
    static const QRegularExpression fileName(QStringLiteral("^[a-f0-9]{64}\\.json$"));
    for (const QString &objectKey : std::as_const(keys)) {
        if (!objectKey.startsWith(prefix) || !fileName.match(objectKey.mid(prefix.size())).hasMatch()) {
            *error = QStringLiteral("Invalid cloud model setting event key");
            return false;
        }
        const QString id = objectKey.mid(prefix.size(), 64);
        if (events->contains(id)) {
            continue;
        }
        const S3Response response = store.getObject(objectKey);
        const QJsonObject object = QJsonDocument::fromJson(response.body).object();
        const QString key = object.value(QStringLiteral("key")).toString();
        const QJsonValue value = object.value(QStringLiteral("value"));
        if (!response.successful() || QCryptographicHash::hash(response.body, QCryptographicHash::Sha256).toHex() != id.toLatin1() || object.value(QStringLiteral("schemaVersion")).toInt() != 1 || !validKey(key) ||
            !object.contains(QStringLiteral("value")) || !validValue(key, value) || !object.value(QStringLiteral("parents")).isArray()) {
            *error = QStringLiteral("Invalid cloud model setting event");
            return false;
        }
        QStringList parents;
        for (const QJsonValue &parent : object.value(QStringLiteral("parents")).toArray()) {
            const QString parentId = parent.toString();
            if (!parent.isString() || !fileName.match(parentId + QStringLiteral(".json")).hasMatch() || parentId == id || parents.contains(parentId)) {
                *error = QStringLiteral("Invalid cloud model setting ancestry");
                return false;
            }
            parents.append(parentId);
        }
        events->insert(id, {key, parents, value});
    }
    for (auto it = events->cbegin(); it != events->cend(); ++it) {
        for (const QString &parent : it->parents) {
            if (!events->contains(parent) || events->value(parent).key != it->key) {
                *error = QStringLiteral("Missing cloud model setting ancestor");
                return false;
            }
        }
    }
    return true;
}

QMap<QString, QStringList> headsByKey(const QMap<QString, Event> &events)
{
    QMap<QString, QStringList> heads;
    for (auto it = events.cbegin(); it != events.cend(); ++it) {
        heads[it->key].append(it.key());
    }
    for (const Event &event : events) {
        for (const QString &parent : event.parents) {
            heads[event.key].removeAll(parent);
        }
    }
    return heads;
}

bool publish(const BookObjectStore &store, const QString &key, const QJsonValue &value, const QStringList &parents, QString *error, bool *created = nullptr)
{
    if (created) {
        *created = false;
    }
    QJsonArray parentArray;
    for (const QString &parent : parents) {
        parentArray.append(parent);
    }
    const QByteArray bytes = QJsonDocument(QJsonObject {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("key"), key}, {QStringLiteral("value"), value}, {QStringLiteral("parents"), parentArray}}).toJson(QJsonDocument::Compact);
    const QString id = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const QString objectKey = QStringLiteral("settings/events/%1.json").arg(id);
    const S3Response response = store.putObjectIfAbsent(objectKey, bytes);
    if (response.status == 412 && response.error.isEmpty()) {
        const S3Response existing = store.getObject(objectKey);
        if (existing.successful() && existing.body == bytes) {
            return true;
        }
        *error = QStringLiteral("Cloud model setting event hash mismatch");
        return false;
    }
    if (!response.successful()) {
        *error = response.error.isEmpty() ? QStringLiteral("Upload model setting failed (HTTP %1)").arg(response.status) : response.error;
        return false;
    }
    if (created) {
        *created = true;
    }
    return true;
}

bool readCheckpoint(const QString &path, QMap<QString, Entry> *baseline, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1 || !object.value(QStringLiteral("entries")).isArray()) {
        *error = QStringLiteral("Invalid local model setting checkpoint");
        return false;
    }
    static const QRegularExpression eventId(QStringLiteral("^[a-f0-9]{64}$"));
    for (const QJsonValue &item : object.value(QStringLiteral("entries")).toArray()) {
        const QJsonObject entry = item.toObject();
        const QString key = entry.value(QStringLiteral("key")).toString();
        const QString head = entry.value(QStringLiteral("head")).toString();
        const QJsonValue value = entry.value(QStringLiteral("value"));
        if (!validKey(key) || !eventId.match(head).hasMatch() || !entry.contains(QStringLiteral("value")) || !validValue(key, value) || baseline->contains(key)) {
            *error = QStringLiteral("Invalid local model setting checkpoint");
            return false;
        }
        baseline->insert(key, {head, value});
    }
    return true;
}

bool loadLocal(const QString &root, const QList<AiProfile> &profiles, QMap<QString, QJsonValue> *current, QString *error)
{
    for (const AiProfile &profile : profiles) {
        const QString key = QStringLiteral("profile/") + profile.id;
        const QJsonObject publicValue = encodeProfile(profile);
        AiProfile parsed;
        if (!validKey(key) || current->contains(key) || !decodeProfile(publicValue, profile.id, &parsed)) {
            *error = QStringLiteral("Invalid local AI model profile");
            return false;
        }
        current->insert(key, publicValue);
    }
    const QDir books(QDir(root).filePath(QStringLiteral("books")));
    static const QRegularExpression hash(QStringLiteral("^[a-f0-9]{64}$"));
    for (const QFileInfo &project : books.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (project.isSymLink() || !hash.match(project.fileName()).hasMatch()) {
            *error = QStringLiteral("Invalid managed book directory");
            return false;
        }
        QJsonObject settings;
        QFile file(QDir(project.filePath()).filePath(QStringLiteral("ai-settings.json")));
        if (file.exists()) {
            if (QFileInfo(file).isSymLink()) {
                *error = QStringLiteral("Book AI settings may not be a symbolic link");
                return false;
            }
            if (!file.open(QIODevice::ReadOnly)) {
                *error = file.errorString();
                return false;
            }
            settings = QJsonDocument::fromJson(file.readAll()).object();
            if (settings.value(QStringLiteral("schemaVersion")).toInt() != 1 || !settings.value(QStringLiteral("selectedProfileId")).isString() || !settings.value(QStringLiteral("defaultPrompt")).isString()) {
                *error = QStringLiteral("Invalid local book AI settings");
                return false;
            }
        }
        current->insert(QStringLiteral("book/%1/selectedProfileId").arg(project.fileName()), settings.value(QStringLiteral("selectedProfileId")).toString());
        current->insert(QStringLiteral("book/%1/defaultPrompt").arg(project.fileName()), settings.value(QStringLiteral("defaultPrompt")).toString());
    }
    return true;
}

bool saveBooks(const QString &root, const QMap<QString, QJsonValue> &original, const QMap<QString, QJsonValue> &current, QString *error)
{
    QSet<QString> hashes;
    for (auto it = current.cbegin(); it != current.cend(); ++it) {
        if (it.key().startsWith(QLatin1String("book/"))) {
            hashes.insert(it.key().section(QLatin1Char('/'), 1, 1));
        }
    }
    for (const QString &hash : std::as_const(hashes)) {
        const QString selectedKey = QStringLiteral("book/%1/selectedProfileId").arg(hash);
        const QString promptKey = QStringLiteral("book/%1/defaultPrompt").arg(hash);
        if (original.value(selectedKey) == current.value(selectedKey) && original.value(promptKey) == current.value(promptKey)) {
            continue;
        }
        const QString project = QDir(root).filePath(QStringLiteral("books/") + hash);
        if (!QFileInfo(project).isDir() || QFileInfo(project).isSymLink()) {
            *error = QStringLiteral("Model settings reference an unknown book");
            return false;
        }
        const QString path = QDir(project).filePath(QStringLiteral("ai-settings.json"));
        QFile existing(path);
        QJsonObject previous;
        if (existing.exists()) {
            if (QFileInfo(existing).isSymLink()) {
                *error = QStringLiteral("Book AI settings may not be a symbolic link");
                return false;
            }
            if (!existing.open(QIODevice::ReadOnly)) {
                *error = existing.errorString();
                return false;
            }
            previous = QJsonDocument::fromJson(existing.readAll()).object();
            if (previous.value(QStringLiteral("schemaVersion")).toInt() != 1 || !previous.value(QStringLiteral("selectedProfileId")).isString() || !previous.value(QStringLiteral("defaultPrompt")).isString()) {
                *error = QStringLiteral("Invalid local book AI settings");
                return false;
            }
        }
        if (previous.value(QStringLiteral("selectedProfileId")).toString() != original.value(selectedKey).toString() || previous.value(QStringLiteral("defaultPrompt")).toString() != original.value(promptKey).toString()) {
            *error = QStringLiteral("Book AI settings changed during sync; retry");
            return false;
        }
        const QJsonObject object {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("selectedProfileId"), current.value(selectedKey).toString()}, {QStringLiteral("defaultPrompt"), current.value(promptKey).toString()}};
        if (!writeJson(path, object, error)) {
            return false;
        }
    }
    return true;
}
}

ModelSettingsSyncResult ModelSettingsSync::synchronize(const BookObjectStore &store, const QString &root, const QList<AiProfile> &localProfiles)
{
    ModelSettingsSyncResult result;
    QMap<QString, QJsonValue> current;
    if (!loadLocal(root, localProfiles, &current, &result.error)) {
        return result;
    }
    const QMap<QString, QJsonValue> original = current;
    QMap<QString, Entry> baseline;
    if (!readCheckpoint(QDir(root).filePath(QStringLiteral("model-settings-sync.json")), &baseline, &result.error)) {
        return result;
    }
    QMap<QString, Event> events;
    if (!readEvents(store, &events, &result.error)) {
        return result;
    }
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) {
        if (!events.contains(it->head) || events.value(it->head).key != it.key() || events.value(it->head).value != it->value) {
            result.error = QStringLiteral("Local model setting checkpoint references a missing event");
            return result;
        }
    }
    QSet<QString> keys;
    for (auto it = current.cbegin(); it != current.cend(); ++it)
        keys.insert(it.key());
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it)
        keys.insert(it.key());
    const QMap<QString, QStringList> previousHeads = headsByKey(events);
    bool attemptedUpload = false;
    for (const QString &key : std::as_const(keys)) {
        const QJsonValue local = current.contains(key) ? current.value(key) : QJsonValue(QJsonValue::Null);
        if ((baseline.contains(key) && baseline.value(key).value == local) || (!baseline.contains(key) && (local.isNull() || (key.startsWith(QLatin1String("book/")) && local.toString().isEmpty())))) {
            continue;
        }
        bool publishedValue = false;
        for (const QString &head : previousHeads.value(key)) {
            if (events.value(head).value == local) {
                publishedValue = true;
                break;
            }
        }
        if (publishedValue) {
            continue;
        }
        bool created = false;
        attemptedUpload = true;
        if (!publish(store, key, local, baseline.contains(key) ? QStringList {baseline.value(key).head} : QStringList {}, &result.error, &created)) {
            return result;
        }
        if (created) {
            ++result.uploaded;
        }
    }
    if (attemptedUpload) {
        events.clear();
        if (!readEvents(store, &events, &result.error)) {
            return result;
        }
    }
    QJsonArray conflicts;
    const QMap<QString, QStringList> heads = headsByKey(events);
    for (auto it = heads.cbegin(); it != heads.cend(); ++it) {
        if (it->isEmpty()) {
            result.error = QStringLiteral("Cloud model setting history has no current version");
            return result;
        }
        if (it->size() > 1) {
            QJsonArray variants;
            for (const QString &head : *it) {
                variants.append(QJsonObject {{QStringLiteral("eventId"), head}, {QStringLiteral("value"), events.value(head).value}});
                if (current.value(it.key(), QJsonValue(QJsonValue::Null)) == events.value(head).value) {
                    baseline.insert(it.key(), {head, events.value(head).value});
                }
            }
            conflicts.append(QJsonObject {{QStringLiteral("key"), it.key()}, {QStringLiteral("variants"), variants}});
            ++result.conflicts;
            continue;
        }
        const QString head = it->first();
        const QJsonValue value = events.value(head).value;
        if (current.value(it.key(), QJsonValue(QJsonValue::Null)) != value) {
            if (value.isNull())
                current.remove(it.key());
            else
                current.insert(it.key(), value);
            ++result.applied;
        }
        baseline.insert(it.key(), {head, value});
    }
    if (!saveBooks(root, original, current, &result.error)) {
        return result;
    }
    for (auto it = current.cbegin(); it != current.cend(); ++it) {
        if (it.key().startsWith(QLatin1String("profile/"))) {
            AiProfile profile;
            if (!decodeProfile(it.value(), it.key().mid(8), &profile)) {
                result.error = QStringLiteral("Invalid synced AI profile");
                return result;
            }
            result.profiles.append(profile);
        }
    }
    QJsonArray checkpointEntries;
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) {
        checkpointEntries.append(QJsonObject {{QStringLiteral("key"), it.key()}, {QStringLiteral("head"), it->head}, {QStringLiteral("value"), it->value}});
    }
    if (!writeJson(QDir(root).filePath(QStringLiteral("model-settings-conflicts.json")), {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("conflicts"), conflicts}}, &result.error)) {
        return result;
    }
    result.checkpoint = QJsonDocument(QJsonObject {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("entries"), checkpointEntries}}).toJson(QJsonDocument::Compact);
    return result;
}

ModelSettingsSyncResult ModelSettingsSync::resolveConflict(const BookObjectStore &store, const QString &root, const QList<AiProfile> &localProfiles, const QString &key, const QStringList &expectedHeads, const QString &chosenHead)
{
    ModelSettingsSyncResult result = synchronize(store, root, localProfiles);
    if (!result.successful())
        return result;
    QMap<QString, Event> events;
    if (!readEvents(store, &events, &result.error))
        return result;
    const QStringList heads = headsByKey(events).value(key);
    QStringList expected = expectedHeads;
    expected.sort();
    if (!validKey(key) || heads.size() < 2 || heads != expected || !heads.contains(chosenHead)) {
        result.error = QStringLiteral("Model setting versions changed; sync and choose again");
        return result;
    }
    const QJsonValue selected = events.value(chosenHead).value;
    if (key.startsWith(QLatin1String("profile/"))) {
        result.profiles.removeIf([&](const AiProfile &profile) { return profile.id == key.mid(8); });
        if (!selected.isNull()) {
            AiProfile profile;
            if (!decodeProfile(selected, key.mid(8), &profile)) {
                result.error = QStringLiteral("Invalid selected AI profile");
                return result;
            }
            result.profiles.append(profile);
        }
    } else {
        const QString hash = key.section(QLatin1Char('/'), 1, 1);
        const QString field = key.section(QLatin1Char('/'), 2, 2);
        const QString path = QDir(root).filePath(QStringLiteral("books/%1/ai-settings.json").arg(hash));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            result.error = file.errorString();
            return result;
        }
        QJsonObject settings = QJsonDocument::fromJson(file.readAll()).object();
        settings.insert(field, selected);
        if (!writeJson(path, settings, &result.error))
            return result;
    }
    if (!publish(store, key, selected, heads, &result.error))
        return result;
    // The merged head is now the sole cloud value. The next pass applies it.
    return synchronize(store, root, result.profiles);
}

bool ModelSettingsSync::commit(const QString &root, const ModelSettingsSyncResult &result, QString *error)
{
    if (!result.successful() || result.checkpoint.isEmpty()) {
        *error = QStringLiteral("No successful model setting sync to commit");
        return false;
    }
    QSaveFile file(QDir(root).filePath(QStringLiteral("model-settings-sync.json")));
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    if (file.write(result.checkpoint) != result.checkpoint.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
