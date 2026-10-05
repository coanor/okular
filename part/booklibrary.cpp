/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "booklibrary.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

namespace
{
bool fail(const QString &reason, QString *error)
{
    if (error) {
        *error = reason;
    }
    return false;
}

QString hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool copyAtomically(const QString &sourcePath, const QString &destinationPath)
{
    QFile source(sourcePath);
    QSaveFile destination(destinationPath);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        return false;
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() || destination.write(chunk) != chunk.size()) {
            destination.cancelWriting();
            return false;
        }
    }
    return destination.commit();
}

bool writeManifest(const QString &path, const QString &id, const QString &storedName, const QString &originalName)
{
    QJsonObject manifest;
    manifest.insert(QStringLiteral("schemaVersion"), 1);
    manifest.insert(QStringLiteral("sha256"), id);
    manifest.insert(QStringLiteral("storedName"), storedName);
    manifest.insert(QStringLiteral("originalName"), originalName);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}
}

bool BookLibrary::importFile(const QString &sourcePath, const QString &libraryRoot, BookProject *project, QString *error)
{
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile() || sourceInfo.isSymLink() || !sourceInfo.isReadable()) {
        return fail(QStringLiteral("The source must be a readable regular file"), error);
    }
    if (libraryRoot.isEmpty()) {
        return fail(QStringLiteral("Choose a managed library directory first"), error);
    }
    const QString booksPath = QDir(libraryRoot).filePath(QStringLiteral("books"));
    if (!QDir().mkpath(booksPath)) {
        return fail(QStringLiteral("Could not create the managed library directory"), error);
    }
    QLockFile libraryLock(QDir(libraryRoot).filePath(QStringLiteral(".sync.lock")));
    if (!libraryLock.tryLock(30000)) {
        return fail(QStringLiteral("The managed library is busy"), error);
    }
    // The source can change while waiting for another import or sync.
    const QString id = hashFile(sourcePath);
    if (id.isEmpty()) {
        return fail(QStringLiteral("Could not hash the source file"), error);
    }
    QLockFile lock(QDir(booksPath).filePath(id + QStringLiteral(".lock")));
    if (!lock.tryLock(30000)) {
        return fail(QStringLiteral("Could not lock the book project"), error);
    }

    const QString projectPath = QDir(booksPath).filePath(id);
    const QString manifestPath = QDir(projectPath).filePath(QStringLiteral("manifest.json"));
    QString storedName;
    QString originalName = sourceInfo.fileName();
    const bool existingProject = QFileInfo::exists(projectPath);
    if (existingProject) {
        const QFileInfo projectInfo(projectPath);
        if (!projectInfo.isDir() || projectInfo.isSymLink() || QFileInfo(manifestPath).isSymLink()) {
            return fail(QStringLiteral("The existing book project is invalid"), error);
        }
        QFile manifestFile(manifestPath);
        if (!manifestFile.open(QIODevice::ReadOnly)) {
            return fail(QStringLiteral("The existing book project has no readable manifest"), error);
        }
        const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        storedName = manifest.value(QStringLiteral("storedName")).toString();
        originalName = manifest.value(QStringLiteral("originalName")).toString();
        const QFileInfo storedInfo(QDir(projectPath).filePath(storedName));
        if (manifest.value(QStringLiteral("schemaVersion")).toInt() != 1 || manifest.value(QStringLiteral("sha256")).toString() != id || storedName.isEmpty() || originalName.isEmpty() || QFileInfo(storedName).fileName() != storedName ||
            storedInfo.isSymLink() || hashFile(storedInfo.filePath()) != id) {
            return fail(QStringLiteral("The existing book project is invalid"), error);
        }
    } else {
        if (!QDir().mkpath(projectPath)) {
            return fail(QStringLiteral("Could not create the book project directory"), error);
        }
        storedName = QStringLiteral("source");
        if (!sourceInfo.suffix().isEmpty()) {
            storedName += QLatin1Char('.') + sourceInfo.suffix();
        }
        const QString destinationPath = QDir(projectPath).filePath(storedName);
        if (!copyAtomically(sourcePath, destinationPath) || hashFile(destinationPath) != id || !writeManifest(manifestPath, id, storedName, originalName)) {
            QFile::remove(destinationPath);
            QDir().rmdir(projectPath);
            return fail(QStringLiteral("Could not store the book in the managed library"), error);
        }
    }

    const QString destinationPath = QDir(projectPath).filePath(storedName);
    if (QFileInfo(sourcePath).canonicalFilePath() != QFileInfo(destinationPath).canonicalFilePath()) {
        if (hashFile(sourcePath) != id) {
            return fail(QStringLiteral("The original file changed during import and was kept"), error);
        }
        if (!QFile::remove(sourcePath)) {
            return fail(QStringLiteral("The book was stored, but the original file could not be removed"), error);
        }
    }
    if (project) {
        project->id = id;
        project->sourcePath = destinationPath;
        project->originalName = originalName;
    }
    return true;
}
