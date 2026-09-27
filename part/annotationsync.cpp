/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "annotationsync.h"

#include "core/annotationsidecar_p.h"

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
#include <QTemporaryDir>

#include <optional>
#include <utility>

namespace
{
using Value = std::optional<Okular::SidecarAnnotation>;

struct Baseline {
    QString head;
    Value value;
};

struct Event {
    QString id;
    QString key;
    QStringList parents;
    Value value;
};

QString annotationKey(int page, const QString &id)
{
    return QString::number(page) + QLatin1Char('\x1f') + id;
}

bool sameValue(const Value &left, const Value &right)
{
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left) {
        return true;
    }
    return left->id == right->id && left->page == right->page && left->subtype == right->subtype && left->xml == right->xml && left->contents == right->contents && left->author == right->author && left->color == right->color &&
        left->hiddenNative == right->hiddenNative;
}

QJsonValue encodeValue(const Value &value)
{
    if (!value) {
        return QJsonValue(QJsonValue::Null);
    }
    return QJsonObject {{QStringLiteral("id"), value->id},
                        {QStringLiteral("page"), value->page},
                        {QStringLiteral("subtype"), value->subtype},
                        {QStringLiteral("xml"), value->xml},
                        {QStringLiteral("contents"), value->contents},
                        {QStringLiteral("author"), value->author},
                        {QStringLiteral("color"), value->color},
                        {QStringLiteral("hiddenNative"), value->hiddenNative}};
}

bool decodeValue(const QJsonValue &json, int page, const QString &id, Value *value)
{
    if (json.isNull()) {
        *value = std::nullopt;
        return true;
    }
    if (!json.isObject()) {
        return false;
    }
    const QJsonObject object = json.toObject();
    if (object.value(QStringLiteral("id")).toString() != id || object.value(QStringLiteral("page")).toInt(-1) != page || !object.value(QStringLiteral("subtype")).isDouble() || !object.value(QStringLiteral("xml")).isString() ||
        !object.value(QStringLiteral("contents")).isString() || !object.value(QStringLiteral("author")).isString() || !object.value(QStringLiteral("color")).isString() || !object.value(QStringLiteral("hiddenNative")).isBool()) {
        return false;
    }
    Okular::SidecarAnnotation annotation {id, page, object.value(QStringLiteral("subtype")).toInt(), object.value(QStringLiteral("xml")).toString()};
    annotation.contents = object.value(QStringLiteral("contents")).toString();
    annotation.author = object.value(QStringLiteral("author")).toString();
    annotation.color = object.value(QStringLiteral("color")).toString();
    annotation.hiddenNative = object.value(QStringLiteral("hiddenNative")).toBool();
    if (!annotation.hiddenNative && annotation.xml.isEmpty()) {
        return false;
    }
    *value = annotation;
    return true;
}

bool decodeEntry(const QJsonObject &object, QString *key, Value *value)
{
    const QString id = object.value(QStringLiteral("id")).toString();
    const int page = object.value(QStringLiteral("page")).toInt(-1);
    if (id.isEmpty() || page < 0 || !object.contains(QStringLiteral("value")) || !decodeValue(object.value(QStringLiteral("value")), page, id, value)) {
        return false;
    }
    *key = annotationKey(page, id);
    return true;
}

bool readBaseline(const QString &path, const QString &hash, QMap<QString, Baseline> *baseline, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1 || root.value(QStringLiteral("pdfSha256")).toString() != hash || !root.value(QStringLiteral("entries")).isArray()) {
        *error = QStringLiteral("Invalid local annotation sync checkpoint");
        return false;
    }
    static const QRegularExpression eventId(QStringLiteral("^[a-f0-9]{64}$"));
    for (const QJsonValue &entry : root.value(QStringLiteral("entries")).toArray()) {
        if (!entry.isObject()) {
            *error = QStringLiteral("Invalid local annotation sync checkpoint");
            return false;
        }
        const QJsonObject object = entry.toObject();
        QString key;
        Value value;
        const QString head = object.value(QStringLiteral("head")).toString();
        if (!eventId.match(head).hasMatch() || !decodeEntry(object, &key, &value) || baseline->contains(key)) {
            *error = QStringLiteral("Invalid local annotation sync checkpoint");
            return false;
        }
        baseline->insert(key, {head, value});
    }
    return true;
}

bool writeJson(const QString &path, const QJsonObject &root, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

QString hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QJsonObject encodeEntry(const Value &value, const QString &key)
{
    const int separator = key.indexOf(QLatin1Char('\x1f'));
    return {{QStringLiteral("page"), key.left(separator).toInt()}, {QStringLiteral("id"), key.mid(separator + 1)}, {QStringLiteral("value"), encodeValue(value)}};
}

bool listEvents(const BookObjectStore &store, const QString &hash, QMap<QString, Event> *events, QString *error)
{
    const QString prefix = QStringLiteral("books/%1/annotations/events/").arg(hash);
    QStringList keys;
    if (!store.listObjects(prefix, &keys, error)) {
        return false;
    }
    static const QRegularExpression eventName(QStringLiteral("^[a-f0-9]{64}\\.json$"));
    for (const QString &objectKey : std::as_const(keys)) {
        if (!objectKey.startsWith(prefix) || !eventName.match(objectKey.mid(prefix.size())).hasMatch()) {
            *error = QStringLiteral("Invalid cloud annotation event key");
            return false;
        }
        const QString id = objectKey.mid(prefix.size(), 64);
        if (events->contains(id)) {
            continue;
        }
        const S3Response response = store.getObject(objectKey);
        const QByteArray digest = QCryptographicHash::hash(response.body, QCryptographicHash::Sha256).toHex();
        const QJsonObject object = QJsonDocument::fromJson(response.body).object();
        QString key;
        Value value;
        if (!response.successful() || digest != id.toLatin1() || object.value(QStringLiteral("schemaVersion")).toInt() != 1 || !decodeEntry(object, &key, &value) || !object.value(QStringLiteral("parents")).isArray()) {
            *error = QStringLiteral("Invalid cloud annotation event for book %1").arg(hash);
            return false;
        }
        QStringList parents;
        for (const QJsonValue &parent : object.value(QStringLiteral("parents")).toArray()) {
            if (!parent.isString() || !eventName.match(parent.toString() + QStringLiteral(".json")).hasMatch() || parents.contains(parent.toString()) || parent.toString() == id) {
                *error = QStringLiteral("Invalid cloud annotation event ancestry");
                return false;
            }
            parents.append(parent.toString());
        }
        events->insert(id, {id, key, parents, value});
    }
    for (const Event &event : std::as_const(*events)) {
        for (const QString &parent : event.parents) {
            if (!events->contains(parent) || events->value(parent).key != event.key) {
                *error = QStringLiteral("Missing or mismatched cloud annotation ancestor");
                return false;
            }
        }
    }
    return true;
}

QMap<QString, QStringList> headsByKey(const QMap<QString, Event> &events)
{
    QMap<QString, QStringList> heads;
    for (const Event &event : events) {
        heads[event.key].append(event.id);
    }
    for (const Event &event : events) {
        for (const QString &parent : event.parents) {
            heads[event.key].removeAll(parent);
        }
    }
    return heads;
}
}

AnnotationSyncResult AnnotationSync::synchronize(const BookObjectStore &store, const QString &libraryRoot, const QString &pdfHash, bool deferRemoteApply)
{
    AnnotationSyncResult result;
    static const QRegularExpression validHash(QStringLiteral("^[a-f0-9]{64}$"));
    if (!validHash.match(pdfHash).hasMatch()) {
        result.error = QStringLiteral("Invalid PDF project hash");
        return result;
    }
    const QString projectPath = QDir(libraryRoot).filePath(QStringLiteral("books/%1").arg(pdfHash));
    if (!QFileInfo(projectPath).isDir() || QFileInfo(projectPath).isSymLink()) {
        result.error = QStringLiteral("PDF project is not in the managed library");
        return result;
    }
    QMap<QString, Baseline> baseline;
    const QString checkpointPath = QDir(projectPath).filePath(QStringLiteral("annotation-sync.json"));
    if (!readBaseline(checkpointPath, pdfHash, &baseline, &result.error)) {
        return result;
    }
    QList<Okular::SidecarAnnotation> annotations;
    qint64 revision = 0;
    if (!Okular::AnnotationSidecar::load(pdfHash, &annotations, &result.error, &revision)) {
        return result;
    }
    QMap<QString, Value> current;
    for (const Okular::SidecarAnnotation &annotation : std::as_const(annotations)) {
        current.insert(annotationKey(annotation.page, annotation.id), annotation);
    }
    QMap<QString, Event> events;
    if (!listEvents(store, pdfHash, &events, &result.error)) {
        return result;
    }
    QMap<QString, QStringList> heads = headsByKey(events);
    for (auto it = heads.cbegin(); it != heads.cend(); ++it) {
        if (it->isEmpty()) {
            result.error = QStringLiteral("Cloud annotation event history has no current version");
            return result;
        }
    }
    QSet<QString> keys;
    for (auto it = current.cbegin(); it != current.cend(); ++it) {
        keys.insert(it.key());
    }
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) {
        keys.insert(it.key());
        if (!events.contains(it->head) || events.value(it->head).key != it.key() || !sameValue(it->value, events.value(it->head).value)) {
            result.error = QStringLiteral("Local annotation checkpoint references a missing cloud event");
            return result;
        }
    }
    bool attemptedUpload = false;
    for (const QString &key : std::as_const(keys)) {
        const Value local = current.value(key);
        const Value previous = baseline.value(key).value;
        if (sameValue(local, previous)) {
            continue;
        }
        bool alreadyPublished = false;
        for (const Event &event : std::as_const(events)) {
            if (event.key == key && sameValue(event.value, local)) {
                alreadyPublished = true;
                break;
            }
        }
        if (alreadyPublished) {
            continue;
        }
        QJsonObject body = encodeEntry(local, key);
        body.insert(QStringLiteral("schemaVersion"), 1);
        QJsonArray parents;
        if (baseline.contains(key)) {
            parents.append(baseline.value(key).head);
        }
        body.insert(QStringLiteral("parents"), parents);
        const QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
        const QString id = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const QString objectKey = QStringLiteral("books/%1/annotations/events/%2.json").arg(pdfHash, id);
        attemptedUpload = true;
        const S3Response uploaded = store.putObjectIfAbsent(objectKey, bytes);
        if (uploaded.status == 412 && uploaded.error.isEmpty()) {
            const S3Response existing = store.getObject(objectKey);
            if (!existing.successful() || existing.body != bytes) {
                result.error = QStringLiteral("A cloud annotation event does not match its hash");
                return result;
            }
        } else if (!uploaded.successful()) {
            result.error = uploaded.error.isEmpty() ? QStringLiteral("Upload annotation failed (HTTP %1)").arg(uploaded.status) : uploaded.error;
            return result;
        } else {
            ++result.uploaded;
        }
    }
    if (attemptedUpload) {
        if (!listEvents(store, pdfHash, &events, &result.error)) {
            return result;
        }
        heads = headsByKey(events);
        for (auto it = heads.cbegin(); it != heads.cend(); ++it) {
            if (it->isEmpty()) {
                result.error = QStringLiteral("Cloud annotation event history has no current version");
                return result;
            }
        }
    }
    QJsonArray conflicts;
    for (auto it = heads.cbegin(); it != heads.cend(); ++it) {
        if (it->size() > 1) {
            // A device may keep editing its own version while another branch
            // remains unresolved. Advance its local parent to that head so
            // the next edit replaces the local head instead of making a new
            // sibling branch.
            for (const QString &head : *it) {
                if (sameValue(current.value(it.key()), events.value(head).value)) {
                    baseline.insert(it.key(), {head, events.value(head).value});
                    break;
                }
            }
            QJsonObject conflict = encodeEntry(current.value(it.key()), it.key());
            QJsonArray variants;
            for (const QString &head : *it) {
                QJsonObject variant {{QStringLiteral("eventId"), head}, {QStringLiteral("value"), encodeValue(events.value(head).value)}};
                variants.append(variant);
            }
            conflict.insert(QStringLiteral("variants"), variants);
            conflicts.append(conflict);
            ++result.conflicts;
            continue;
        }
        const QString head = it->first();
        const Value remote = events.value(head).value;
        if (!sameValue(current.value(it.key()), remote)) {
            if (deferRemoteApply) {
                ++result.deferred;
                continue;
            }
            if (remote) {
                current.insert(it.key(), remote);
            } else {
                current.remove(it.key());
            }
            ++result.applied;
        }
        baseline.insert(it.key(), {head, remote});
    }
    if (result.applied) {
        QList<Okular::SidecarAnnotation> merged;
        for (const Value &value : std::as_const(current)) {
            if (value) {
                merged.append(*value);
            }
        }
        if (!Okular::AnnotationSidecar::save(pdfHash, merged, &result.error, revision)) {
            return result;
        }
    }
    QJsonArray checkpointEntries;
    for (auto it = baseline.cbegin(); it != baseline.cend(); ++it) {
        QJsonObject entry = encodeEntry(it->value, it.key());
        entry.insert(QStringLiteral("head"), it->head);
        checkpointEntries.append(entry);
    }
    if (!writeJson(checkpointPath, {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("pdfSha256"), pdfHash}, {QStringLiteral("entries"), checkpointEntries}}, &result.error) ||
        !writeJson(QDir(projectPath).filePath(QStringLiteral("annotation-conflicts.json")), {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("pdfSha256"), pdfHash}, {QStringLiteral("conflicts"), conflicts}}, &result.error)) {
        return result;
    }
    if (QFileInfo::exists(Okular::AnnotationSidecar::pathForHash(pdfHash))) {
        const QString stagingRoot = QDir(libraryRoot).filePath(QStringLiteral(".incoming"));
        if (!QDir().mkpath(stagingRoot)) {
            result.error = QStringLiteral("Could not stage annotation snapshot");
            return result;
        }
        QTemporaryDir staging(QDir(stagingRoot).filePath(QStringLiteral("annotation-XXXXXX")));
        const QString snapshotPath = staging.filePath(QStringLiteral("snapshot.sqlite"));
        if (!staging.isValid() || !Okular::AnnotationSidecar::snapshot(pdfHash, snapshotPath, &result.error)) {
            return result;
        }
        const QString digest = hashFile(snapshotPath);
        if (digest.isEmpty()) {
            result.error = QStringLiteral("Could not hash annotation snapshot");
            return result;
        }
        const QString snapshotKey = QStringLiteral("books/%1/annotations/snapshots/%2.sqlite").arg(pdfHash, digest);
        const S3Response uploaded = store.putFileIfAbsent(snapshotKey, snapshotPath, digest);
        if (uploaded.status == 412 && uploaded.error.isEmpty()) {
            const S3Response verified = store.downloadFile(snapshotKey, staging.filePath(QStringLiteral("remote.sqlite")), digest);
            if (!verified.successful()) {
                result.error = QStringLiteral("An existing cloud annotation snapshot does not match its hash");
                return result;
            }
        } else if (!uploaded.successful()) {
            result.error = uploaded.error.isEmpty() ? QStringLiteral("Upload annotation snapshot failed (HTTP %1)").arg(uploaded.status) : uploaded.error;
            return result;
        } else {
            ++result.snapshotsUploaded;
        }
    }
    return result;
}

AnnotationSyncResult AnnotationSync::resolveConflict(const BookObjectStore &store, const QString &libraryRoot, const QString &pdfHash, int page, const QString &annotationId, const QStringList &expectedHeads, const QString &chosenHead)
{
    AnnotationSyncResult result;
    if (page < 0 || annotationId.isEmpty() || expectedHeads.size() < 2) {
        result.error = QStringLiteral("Invalid annotation conflict selection");
        return result;
    }
    // Publish local edits before choosing a version. A stale dialog must not
    // silently resolve a branch that appeared after the reader made a choice.
    result = synchronize(store, libraryRoot, pdfHash);
    if (!result.successful()) {
        return result;
    }
    QMap<QString, Event> events;
    if (!listEvents(store, pdfHash, &events, &result.error)) {
        return result;
    }
    const QString key = annotationKey(page, annotationId);
    const QStringList heads = headsByKey(events).value(key);
    QStringList sortedExpected = expectedHeads;
    sortedExpected.sort();
    if (heads.size() < 2 || heads != sortedExpected || !heads.contains(chosenHead)) {
        result.error = QStringLiteral("Annotation versions changed; sync and choose again");
        return result;
    }
    QList<Okular::SidecarAnnotation> annotations;
    qint64 revision = 0;
    if (!Okular::AnnotationSidecar::load(pdfHash, &annotations, &result.error, &revision)) {
        return result;
    }
    annotations.removeIf([&](const Okular::SidecarAnnotation &annotation) { return annotation.page == page && annotation.id == annotationId; });
    const Value selected = events.value(chosenHead).value;
    if (selected) {
        annotations.append(*selected);
    }
    if (!Okular::AnnotationSidecar::save(pdfHash, annotations, &result.error, revision)) {
        return result;
    }
    QJsonObject body = encodeEntry(selected, key);
    body.insert(QStringLiteral("schemaVersion"), 1);
    QJsonArray parents;
    for (const QString &head : heads) {
        parents.append(head);
    }
    body.insert(QStringLiteral("parents"), parents);
    const QByteArray bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    const QString id = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const S3Response uploaded = store.putObjectIfAbsent(QStringLiteral("books/%1/annotations/events/%2.json").arg(pdfHash, id), bytes);
    if (uploaded.status == 412 && uploaded.error.isEmpty()) {
        const S3Response existing = store.getObject(QStringLiteral("books/%1/annotations/events/%2.json").arg(pdfHash, id));
        if (!existing.successful() || existing.body != bytes) {
            result.error = QStringLiteral("A cloud annotation event does not match its hash");
            return result;
        }
    } else if (!uploaded.successful()) {
        result.error = uploaded.error.isEmpty() ? QStringLiteral("Resolve annotation failed (HTTP %1)").arg(uploaded.status) : uploaded.error;
        return result;
    }
    return synchronize(store, libraryRoot, pdfHash);
}
