/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "readinghistory.h"
#include "core/readingdatastore_p.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <utility>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif

namespace
{
QString providerFileName(const QUrl &url)
{
#ifdef Q_OS_ANDROID
    const QJniObject activity(QNativeInterface::QAndroidApplication::context());
    const QJniObject name = activity.callObjectMethod("documentName", "(Ljava/lang/String;)Ljava/lang/String;", QJniObject::fromString(url.toString(QUrl::FullyEncoded)).object<jstring>());
    QJniEnvironment environment;
    environment.checkAndClearExceptions(QJniEnvironment::OutputMode::Silent);
    return name.toString();
#else
    Q_UNUSED(url)
    return {};
#endif
}

ReadingRecord fromQuery(const QSqlQuery &query)
{
    return {QUrl(query.value(0).toString()), query.value(1).toString(), query.value(2).toString(), query.value(3).toInt(), query.value(4).toInt(), query.value(5).toLongLong(), query.value(6).toString()};
}
}

ReadingHistory::ReadingHistory(const QString &path, FileNameResolver fileNameResolver)
    : m_path(path)
    , m_fileNameResolver(fileNameResolver ? std::move(fileNameResolver) : providerFileName)
{
}

ReadingHistory::~ReadingHistory()
{
    const QString connection = m_database.connectionName();
    m_database = {};
    if (!connection.isEmpty()) {
        QSqlDatabase::removeDatabase(connection);
    }
}

QString ReadingHistory::defaultPath()
{
    return Okular::ReadingDataStore::defaultPath();
}

ReadingRecord ReadingHistory::record(const QUrl &url, const QString &title, int page, int pageCount)
{
    ReadingRecord record {url, url.fileName().isEmpty() ? title : url.fileName(), {}, page, pageCount, QDateTime::currentMSecsSinceEpoch(), QUuid::createUuid().toString(QUuid::WithoutBraces)};
    if (url.isLocalFile()) {
        const QFileInfo source(url.toLocalFile());
        const QDir project = source.dir();
        QFile manifestFile(project.filePath(QStringLiteral("manifest.json")));
        if (QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(project.dirName()).hasMatch() && !QFileInfo(project.path()).isSymLink() && manifestFile.open(QIODevice::ReadOnly)) {
            const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
            if (manifest.value(QStringLiteral("schemaVersion")).toInt() == 1 && manifest.value(QStringLiteral("sha256")).toString() == project.dirName() && manifest.value(QStringLiteral("storedName")).toString() == source.fileName()) {
                record.title = manifest.value(QStringLiteral("originalName")).toString(title);
            }
        }
    }
    if (record.title.isEmpty()) {
        record.title = url.fileName();
    }
    return record;
}

bool ReadingHistory::open(QString *error)
{
    if (m_database.isOpen()) {
        return true;
    }
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        *error = QStringLiteral("Could not create the reading history directory");
        return false;
    }
    if (!m_database.isValid()) {
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QUuid::createUuid().toString());
        m_database.setDatabaseName(m_path);
        m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    }
    if (!m_database.open()) {
        *error = m_database.lastError().text();
        return false;
    }
    if (!Okular::ReadingDataStore::initialize(m_database, error) || (m_path == defaultPath() && !Okular::ReadingDataStore::importLegacyData(m_database, Okular::ReadingDataStore::LegacyData::ReadingHistory, error))) {
        m_database.close();
        return false;
    }
    return true;
}

bool ReadingHistory::read(const QUrl &url, ReadingRecord *record, QString *error)
{
    error->clear();
    QString ignored;
    const QString hash = Okular::ReadingDataStore::fileHash(url, &ignored);
    if (!hash.isEmpty()) {
        return read(hash, url, record, error);
    }
    if (!open(error)) {
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT book_hash FROM book_locations WHERE device_id = ? AND url = ?"));
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    query.addBindValue(url.toString(QUrl::FullyEncoded));
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    if (query.next()) {
        const QString storedHash = query.value(0).toString();
        query.finish();
        return read(storedHash, url, record, error);
    }
    query.finish();
    query.prepare(QStringLiteral("SELECT url, title, book_id, page, page_count, updated_at, revision FROM legacy_reading_history WHERE url = ? AND device_id = ?"));
    query.addBindValue(url.toString(QUrl::FullyEncoded));
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    *record = query.next() ? fromQuery(query) : ReadingRecord();
    return true;
}

bool ReadingHistory::read(const QString &hash, const QUrl &url, ReadingRecord *record, QString *error)
{
    error->clear();
    if (!Okular::ReadingDataStore::isBookHash(hash) || !open(error)) {
        if (error->isEmpty()) {
            *error = QStringLiteral("Invalid book content hash");
        }
        return false;
    }
    // Adopt an old URL record once the actual file bytes are available.
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT url, title, book_id, page, page_count, updated_at, revision FROM legacy_reading_history WHERE url = ? AND device_id = ?"));
    query.addBindValue(url.toString(QUrl::FullyEncoded));
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    if (query.next()) {
        ReadingRecord legacy = fromQuery(query);
        legacy.bookId = hash;
        query.finish();
        if (!save(legacy, error)) {
            return false;
        }
    }
    query.finish();
    query.prepare(QStringLiteral("SELECT ?, books.file_name, books.hash, p.page, p.page_count, p.updated_at, p.revision FROM books JOIN reading_progress p ON p.book_hash = books.hash WHERE books.hash = ?"));
    query.addBindValue(url.toString(QUrl::FullyEncoded));
    query.addBindValue(hash);
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    *record = query.next() ? fromQuery(query) : ReadingRecord();
    return true;
}

bool ReadingHistory::entries(QList<ReadingRecord> *records, QString *error)
{
    error->clear();
    records->clear();
    if (!open(error)) {
        return false;
    }
    // Resolve accessible legacy files on this worker; unavailable ones remain
    // in the staging table until the user grants access or locates the book.
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT url, title, book_id, page, page_count, updated_at, revision FROM legacy_reading_history WHERE device_id = ?"));
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    QList<ReadingRecord> legacy;
    while (query.next()) {
        legacy.append(fromQuery(query));
    }
    query.finish();
    for (ReadingRecord &record : legacy) {
        QString ignored;
        record.bookId = Okular::ReadingDataStore::fileHash(record.url, &ignored);
        if (!record.bookId.isEmpty() && !save(record, error)) {
            return false;
        }
    }
    query.prepare(
        QStringLiteral("SELECT COALESCE((SELECT url FROM book_locations l WHERE l.book_hash = b.hash AND l.device_id = ? ORDER BY rowid DESC LIMIT 1), ''), b.file_name, b.hash, "
                       "COALESCE(p.page, 0), COALESCE(p.page_count, 0), COALESCE(p.updated_at, 0), COALESCE(p.revision, '') FROM books b JOIN reading_progress p ON p.book_hash = b.hash "
                       "UNION ALL SELECT CASE WHEN device_id = ? THEN url ELSE '' END, title, '', page, page_count, updated_at, revision FROM legacy_reading_history ORDER BY 6 DESC, 7 DESC"));
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    if (!query.exec()) {
        *error = query.lastError().text();
        return false;
    }
    while (query.next()) {
        records->append(fromQuery(query));
    }
    query.finish();
    for (ReadingRecord &record : *records) {
        if (record.url.scheme() != QLatin1String("content") || (!record.title.isEmpty() && record.title != record.url.fileName())) {
            continue;
        }
        const QString name = m_fileNameResolver(record.url);
        if (name.isEmpty() || name == record.title) {
            continue;
        }
        QSqlQuery rename(m_database);
        if (Okular::ReadingDataStore::isBookHash(record.bookId)) {
            rename.prepare(QStringLiteral("UPDATE books SET file_name = ? WHERE hash = ? AND file_name = ?"));
            rename.addBindValue(name);
            rename.addBindValue(record.bookId);
        } else {
            rename.prepare(QStringLiteral("UPDATE legacy_reading_history SET title = ? WHERE url = ? AND device_id = ? AND title = ?"));
            rename.addBindValue(name);
            rename.addBindValue(record.url.toString(QUrl::FullyEncoded));
            rename.addBindValue(Okular::ReadingDataStore::deviceId());
        }
        rename.addBindValue(record.title);
        if (!rename.exec()) {
            *error = rename.lastError().text();
            return false;
        }
        record.title = name;
    }
    return true;
}

bool ReadingHistory::save(const ReadingRecord &input, QString *error)
{
    error->clear();
    if (input.url.isEmpty() || input.url.scheme() == QLatin1String("fd") || input.page < 0 || input.pageCount < 0 || (input.pageCount == 0 ? input.page != 0 : input.page >= input.pageCount) || input.updatedAt <= 0 ||
        QUuid(input.revision).isNull()) {
        *error = QStringLiteral("Invalid reading history record");
        return false;
    }
    if (!open(error)) {
        return false;
    }
    if (Okular::ReadingDataStore::deviceId().isEmpty()) {
        *error = QStringLiteral("Could not prepare the local device identity");
        return false;
    }
    ReadingRecord record = input;
    if (!Okular::ReadingDataStore::isBookHash(record.bookId)) {
        QString ignored;
        record.bookId = Okular::ReadingDataStore::fileHash(record.url, &ignored);
    }
    QSqlQuery query(m_database);
    if (record.bookId.isEmpty()) {
        // Preserve inaccessible legacy entries without inventing a path hash.
        query.prepare(QStringLiteral(
            "INSERT INTO legacy_reading_history VALUES (?, ?, '', ?, ?, ?, ?, ?) ON CONFLICT(device_id, url) DO UPDATE SET title=excluded.title, page=excluded.page, page_count=excluded.page_count, updated_at=excluded.updated_at, "
            "revision=excluded.revision "
            "WHERE excluded.updated_at > legacy_reading_history.updated_at OR (excluded.updated_at = legacy_reading_history.updated_at AND excluded.revision > legacy_reading_history.revision)"));
        query.addBindValue(record.url.toString(QUrl::FullyEncoded));
        query.addBindValue(record.title.isNull() ? QStringLiteral("") : record.title);
        query.addBindValue(record.page);
        query.addBindValue(record.pageCount);
        query.addBindValue(record.updatedAt);
        query.addBindValue(record.revision);
        query.addBindValue(Okular::ReadingDataStore::deviceId());
        if (!query.exec()) {
            *error = query.lastError().text();
            return false;
        }
        return true;
    }
    if (!m_database.transaction()) {
        *error = m_database.lastError().text();
        return false;
    }
    const auto execute = [&] {
        if (query.exec()) {
            return true;
        }
        *error = query.lastError().text();
        m_database.rollback();
        return false;
    };
    QString name = record.title;
    if (record.url.scheme() == QLatin1String("content")) {
        const QString resolved = m_fileNameResolver(record.url);
        // A provider identifier must not overwrite a previously resolved name.
        name = resolved.isEmpty() ? QString() : resolved;
    }
    if (!Okular::ReadingDataStore::ensureBook(m_database, record.bookId, name, error)) {
        m_database.rollback();
        return false;
    }
    query.prepare(
        QStringLiteral("INSERT INTO reading_progress VALUES (?, ?, ?, ?, ?) ON CONFLICT(book_hash) DO UPDATE SET page=excluded.page, page_count=excluded.page_count, updated_at=excluded.updated_at, revision=excluded.revision "
                       "WHERE excluded.updated_at > reading_progress.updated_at OR (excluded.updated_at = reading_progress.updated_at AND excluded.revision > reading_progress.revision)"));
    query.addBindValue(record.bookId);
    query.addBindValue(record.page);
    query.addBindValue(record.pageCount);
    query.addBindValue(record.updatedAt);
    query.addBindValue(record.revision);
    if (!execute()) {
        return false;
    }
    query.prepare(QStringLiteral("INSERT INTO book_locations VALUES (?, ?, ?) ON CONFLICT(device_id, url) DO UPDATE SET book_hash=excluded.book_hash"));
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    query.addBindValue(record.url.toString(QUrl::FullyEncoded));
    query.addBindValue(record.bookId);
    if (!execute()) {
        return false;
    }
    query.prepare(QStringLiteral("DELETE FROM legacy_reading_history WHERE url = ? AND device_id = ?"));
    query.addBindValue(record.url.toString(QUrl::FullyEncoded));
    query.addBindValue(Okular::ReadingDataStore::deviceId());
    if (!execute()) {
        return false;
    }
    if (!m_database.commit()) {
        *error = m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    return true;
}

bool ReadingHistory::importLegacyLibrary(const QString &root, QString *error)
{
    const QDir books(QDir(root).filePath(QStringLiteral("books")));
    if (QFileInfo(books.path()).isSymLink()) {
        return true;
    }
    const QRegularExpression hash(QStringLiteral("^[a-f0-9]{64}$"));
    for (const QFileInfo &entry : books.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (entry.isSymLink() || !hash.match(entry.fileName()).hasMatch()) {
            continue;
        }
        const QDir project(entry.filePath());
        QFile manifestFile(project.filePath(QStringLiteral("manifest.json")));
        if (QFileInfo(manifestFile).isSymLink() || !manifestFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        const QString name = manifest.value(QStringLiteral("storedName")).toString();
        const QFileInfo source(project.filePath(name));
        if (manifest.value(QStringLiteral("schemaVersion")).toInt() != 1 || manifest.value(QStringLiteral("sha256")).toString() != entry.fileName() || name.isEmpty() || QFileInfo(name).fileName() != name ||
            name.contains(QLatin1Char('\\')) || source.isSymLink() || !source.isFile()) {
            continue;
        }
        const QUrl url = QUrl::fromLocalFile(source.filePath());
        ReadingRecord existing;
        if (!read(url, &existing, error)) {
            return false;
        }
        if (!existing.url.isEmpty()) {
            continue;
        }
        ReadingRecord imported {url, manifest.value(QStringLiteral("originalName")).toString(name), {}, 0, 0, 1, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        imported.bookId = Okular::ReadingDataStore::fileHash(url, error);
        if (imported.bookId.isEmpty() || !save(imported, error)) {
            return false;
        }
    }
    return true;
}
