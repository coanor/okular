/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "booklibrarysync.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>

#include <utility>

namespace
{
struct Manifest {
    QString id;
    QString storedName;
    QString originalName;
    QByteArray bytes;
};

bool parseManifest(const QByteArray &bytes, const QString &id, Manifest *manifest)
{
    const QJsonObject json = QJsonDocument::fromJson(bytes).object();
    const QString storedName = json.value(QStringLiteral("storedName")).toString();
    const QString originalName = json.value(QStringLiteral("originalName")).toString();
    if (json.value(QStringLiteral("schemaVersion")).toInt() != 1 || json.value(QStringLiteral("sha256")).toString() != id || storedName.isEmpty()
        || (storedName != QLatin1String("source") && !storedName.startsWith(QLatin1String("source."))) || QFileInfo(storedName).fileName() != storedName
        || storedName.contains(QLatin1Char('\\'))
        || originalName.isEmpty() || QFileInfo(originalName).fileName() != originalName || originalName.contains(QLatin1Char('\\'))) {
        return false;
    }
    *manifest = {id, storedName, originalName, bytes};
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

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

QString responseError(const S3Response &response, const QString &operation)
{
    return response.error.isEmpty() ? QStringLiteral("%1 failed (HTTP %2)").arg(operation).arg(response.status) : operation + QStringLiteral(": ") + response.error;
}
}

BookLibrarySync::BookLibrarySync(const BookObjectStore &store, QString libraryRoot)
    : m_store(store)
    , m_libraryRoot(std::move(libraryRoot))
{
}

bool BookLibrarySync::ensureLibraryIdentity(QString *error) const
{
    if (m_libraryRoot.isEmpty() || !QDir().mkpath(m_libraryRoot)) {
        *error = QStringLiteral("Choose a writable managed library directory");
        return false;
    }
    S3Response response = m_store.getObject(QStringLiteral("version.json"));
    if (response.status == 404 && response.error.isEmpty()) {
        QJsonObject version;
        version.insert(QStringLiteral("schemaVersion"), 1);
        version.insert(QStringLiteral("libraryId"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        response = m_store.putObjectIfAbsent(QStringLiteral("version.json"), QJsonDocument(version).toJson(QJsonDocument::Compact));
        if (response.status != 412 && !response.successful()) {
            *error = responseError(response, QStringLiteral("Create cloud library"));
            return false;
        }
        response = m_store.getObject(QStringLiteral("version.json"));
    }
    if (!response.successful()) {
        *error = responseError(response, QStringLiteral("Read cloud library version"));
        return false;
    }
    const QJsonObject version = QJsonDocument::fromJson(response.body).object();
    const QString libraryId = version.value(QStringLiteral("libraryId")).toString();
    if (version.value(QStringLiteral("schemaVersion")).toInt() != 1 || QUuid(libraryId).isNull()) {
        *error = QStringLiteral("The cloud library format is unsupported or invalid");
        return false;
    }
    const S3Response conditionalProbe = m_store.putObjectIfAbsent(QStringLiteral("version.json"), response.body);
    if (conditionalProbe.status != 412 || !conditionalProbe.error.isEmpty()) {
        *error = QStringLiteral("This S3 endpoint does not enforce conditional writes");
        return false;
    }
    const QString identityPath = QDir(m_libraryRoot).filePath(QStringLiteral(".library-id"));
    if (QFileInfo::exists(identityPath)) {
        QFile file(identityPath);
        if (!file.open(QIODevice::ReadOnly) || QString::fromUtf8(file.readAll()).trimmed() != libraryId) {
            *error = QStringLiteral("This managed directory belongs to a different cloud library");
            return false;
        }
    } else if (!writeFile(identityPath, libraryId.toUtf8())) {
        *error = QStringLiteral("Could not save the local cloud library identity");
        return false;
    }
    return true;
}

BookSyncResult BookLibrarySync::synchronizeSources() const
{
    BookSyncResult result;
    if (m_libraryRoot.isEmpty() || !QDir().mkpath(m_libraryRoot)) {
        result.error = QStringLiteral("Choose a writable managed library directory");
        return result;
    }
    QLockFile lock(QDir(m_libraryRoot).filePath(QStringLiteral(".sync.lock")));
    if (!lock.tryLock(0)) {
        result.error = QStringLiteral("This managed library is already syncing");
        return result;
    }
    if (!ensureLibraryIdentity(&result.error)) {
        return result;
    }
    QStringList keys;
    if (!m_store.listObjects(QStringLiteral("books/"), &keys, &result.error)) {
        return result;
    }
    const QRegularExpression projectKey(QStringLiteral("^books/([a-f0-9]{64})/manifest\\.json$"));
    QMap<QString, Manifest> remote;
    for (const QString &key : std::as_const(keys)) {
        const QRegularExpressionMatch match = projectKey.match(key);
        if (!match.hasMatch()) {
            continue;
        }
        const QString id = match.captured(1);
        const S3Response response = m_store.getObject(key);
        Manifest manifest;
        if (!response.successful() || !parseManifest(response.body, id, &manifest)) {
            result.error = QStringLiteral("The cloud project %1 has an invalid manifest").arg(id);
            return result;
        }
        remote.insert(id, manifest);
    }

    const QString booksPath = QDir(m_libraryRoot).filePath(QStringLiteral("books"));
    if (!QDir().mkpath(booksPath)) {
        result.error = QStringLiteral("Could not create the managed books directory");
        return result;
    }
    QMap<QString, Manifest> local;
    const QRegularExpression projectId(QStringLiteral("^[a-f0-9]{64}$"));
    const QDir books(booksPath);
    for (const QFileInfo &entry : books.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString id = entry.fileName();
        if (!projectId.match(id).hasMatch() || entry.isSymLink()) {
            result.error = QStringLiteral("The managed library contains an invalid project directory");
            return result;
        }
        QFile manifestFile(QDir(entry.filePath()).filePath(QStringLiteral("manifest.json")));
        Manifest manifest;
        if (!manifestFile.open(QIODevice::ReadOnly) || !parseManifest(manifestFile.readAll(), id, &manifest)
            || hashFile(QDir(entry.filePath()).filePath(manifest.storedName)) != id) {
            result.error = QStringLiteral("The local project %1 is incomplete or changed").arg(id);
            return result;
        }
        local.insert(id, manifest);
    }

    for (auto it = local.cbegin(); it != local.cend(); ++it) {
        if (remote.contains(it.key())) {
            continue;
        }
        const QString sourcePath = books.filePath(it.key() + QLatin1Char('/') + it->storedName);
        const QString sourceKey = QStringLiteral("books/%1/source").arg(it.key());
        const S3Response uploaded = m_store.putFileIfAbsent(sourceKey, sourcePath, it.key());
        if (uploaded.status == 412 && uploaded.error.isEmpty()) {
            const QString incomingPath = QDir(m_libraryRoot).filePath(QStringLiteral(".incoming"));
            QDir().mkpath(incomingPath);
            QTemporaryDir check(QDir(incomingPath).filePath(QStringLiteral("verify-XXXXXX")));
            if (!check.isValid() || !m_store.downloadFile(sourceKey, check.filePath(QStringLiteral("source")), it.key()).successful()) {
                result.error = QStringLiteral("An existing cloud source for %1 does not match its hash").arg(it.key());
                return result;
            }
        } else if (!uploaded.successful()) {
            result.error = responseError(uploaded, QStringLiteral("Upload book source"));
            return result;
        }
        const S3Response published = m_store.putObjectIfAbsent(QStringLiteral("books/%1/manifest.json").arg(it.key()), it->bytes);
        if (published.status == 412 && published.error.isEmpty()) {
            Manifest concurrent;
            const S3Response existing = m_store.getObject(QStringLiteral("books/%1/manifest.json").arg(it.key()));
            if (!existing.successful() || !parseManifest(existing.body, it.key(), &concurrent)) {
                result.error = QStringLiteral("A concurrent cloud project has an invalid manifest");
                return result;
            }
        } else if (!published.successful()) {
            result.error = responseError(published, QStringLiteral("Publish book project"));
            return result;
        }
        ++result.uploaded;
    }

    const QString incomingPath = QDir(m_libraryRoot).filePath(QStringLiteral(".incoming"));
    for (auto it = remote.cbegin(); it != remote.cend(); ++it) {
        if (local.contains(it.key())) {
            continue;
        }
        if (!QDir().mkpath(incomingPath)) {
            result.error = QStringLiteral("Could not create the download staging directory");
            return result;
        }
        QTemporaryDir staging(QDir(incomingPath).filePath(QStringLiteral("book-XXXXXX")));
        if (!staging.isValid()) {
            result.error = QStringLiteral("Could not stage a cloud book download");
            return result;
        }
        const S3Response downloaded = m_store.downloadFile(QStringLiteral("books/%1/source").arg(it.key()), staging.filePath(it->storedName), it.key());
        if (!downloaded.successful() || !writeFile(staging.filePath(QStringLiteral("manifest.json")), it->bytes)) {
            result.error = downloaded.successful() ? QStringLiteral("Could not save a downloaded book manifest") : responseError(downloaded, QStringLiteral("Download book source"));
            return result;
        }
        const QString destination = books.filePath(it.key());
        if (QFileInfo::exists(destination) || !QDir().rename(staging.path(), destination)) {
            result.error = QStringLiteral("Could not install a downloaded book project");
            return result;
        }
        staging.setAutoRemove(false);
        ++result.downloaded;
    }
    return result;
}
