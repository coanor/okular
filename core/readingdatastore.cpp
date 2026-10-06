/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "readingdatastore_p.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QUuid>

#include <sqlite3.h>

using namespace Okular;

namespace
{
bool execute(QSqlQuery &query, QString *error)
{
    if (query.exec()) {
        return true;
    }
    *error = query.lastError().text();
    return false;
}
}

QString ReadingDataStore::defaultPath()
{
    const QString configuredPath = qEnvironmentVariable("OKULAR_READING_DATA_PATH");
    return QDir::isAbsolutePath(configuredPath) ? configuredPath : QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/okular/reading-data.sqlite");
}

QString ReadingDataStore::deviceId()
{
    static const QString id = [] {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/okular/device.ini");
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            return QString();
        }
        QLockFile lock(path + QStringLiteral(".identity-lock"));
        if (!lock.tryLock(5000)) {
            return QString();
        }
        QSettings settings(path, QSettings::IniFormat);
        QString value = settings.value(QStringLiteral("id")).toString();
        if (value.isEmpty()) {
            value = QUuid::createUuid().toString(QUuid::WithoutBraces);
            settings.setValue(QStringLiteral("id"), value);
            settings.sync();
        }
        return settings.status() == QSettings::NoError ? value : QString();
    }();
    return id;
}

bool ReadingDataStore::isBookHash(const QString &hash)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-f0-9]{64}$"));
    return pattern.match(hash).hasMatch();
}

QString ReadingDataStore::fileHash(const QUrl &url, QString *error)
{
    error->clear();
    if (!url.isLocalFile() && url.scheme() != QLatin1String("content")) {
        *error = QStringLiteral("The document has no persistent source file");
        return {};
    }
    QFile file(url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded));
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        *error = file.errorString();
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool ReadingDataStore::initialize(QSqlDatabase &database, QString *error)
{
    if (!database.transaction()) {
        *error = database.lastError().text();
        return false;
    }
    const QStringList schema {
        QStringLiteral("CREATE TABLE IF NOT EXISTS reading_data_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("INSERT OR IGNORE INTO reading_data_metadata VALUES ('schema_version', '2')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS books (hash TEXT PRIMARY KEY, file_name TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS reading_progress (book_hash TEXT PRIMARY KEY, page INTEGER NOT NULL, page_count INTEGER NOT NULL, updated_at INTEGER NOT NULL, revision TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS book_locations (device_id TEXT NOT NULL, url TEXT NOT NULL, book_hash TEXT NOT NULL, PRIMARY KEY(device_id, url))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS book_locations_by_hash ON book_locations(book_hash, device_id)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS legacy_reading_history (url TEXT NOT NULL, title TEXT NOT NULL, book_id TEXT NOT NULL, page INTEGER NOT NULL, page_count INTEGER NOT NULL, updated_at INTEGER NOT NULL, revision TEXT NOT "
                       "NULL, device_id TEXT NOT NULL, PRIMARY KEY(device_id, url))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS ai_conversations (document_key TEXT NOT NULL, profile_id TEXT NOT NULL, session_id TEXT NOT NULL, instructions TEXT NOT NULL, PRIMARY KEY(document_key, profile_id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS ai_messages (document_key TEXT NOT NULL, profile_id TEXT NOT NULL, ordinal INTEGER NOT NULL, role TEXT NOT NULL, content TEXT NOT NULL, page INTEGER NOT NULL, page_text TEXT NOT NULL, "
                       "selected_text TEXT NOT NULL, page_image BLOB NOT NULL, PRIMARY KEY(document_key, profile_id, ordinal))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS annotation_documents (book_hash TEXT PRIMARY KEY)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS annotation_events (book_hash TEXT NOT NULL, sequence INTEGER NOT NULL, annotation_id TEXT NOT NULL, page INTEGER NOT NULL, subtype INTEGER NOT NULL, xml TEXT, operation TEXT NOT NULL "
                       "CHECK(operation IN ('upsert', 'delete', 'hide')), recorded_utc TEXT NOT NULL, PRIMARY KEY(book_hash, sequence))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS annotation_events_by_id ON annotation_events(book_hash, page, annotation_id, sequence)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS annotations (book_hash TEXT NOT NULL, page INTEGER NOT NULL, annotation_id TEXT NOT NULL, subtype INTEGER NOT NULL, contents TEXT, author TEXT, color TEXT, hidden_native INTEGER NOT NULL "
                       "DEFAULT 0, xml TEXT, PRIMARY KEY(book_hash, page, annotation_id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS annotations_by_page ON annotations(book_hash, page, subtype)")};
    QSqlQuery query(database);
    for (const QString &sql : schema) {
        if (!query.exec(sql)) {
            *error = query.lastError().text();
            database.rollback();
            return false;
        }
        if (sql.startsWith(QLatin1String("INSERT OR IGNORE INTO reading_data_metadata"))) {
            if (!query.exec(QStringLiteral("SELECT value FROM reading_data_metadata WHERE key = 'schema_version'")) || !query.next()) {
                *error = query.lastError().text();
                database.rollback();
                return false;
            }
            const QString version = query.value(0).toString();
            query.finish();
            if (version == QLatin1String("1")) {
                // Schema 1 staged URLs came from one device. Infer its owner
                // only when the database's existing locations agree.
                const QStringList upgrade {
                    QStringLiteral("ALTER TABLE legacy_reading_history RENAME TO legacy_reading_history_v1"),
                    QStringLiteral("CREATE TABLE legacy_reading_history (url TEXT NOT NULL, title TEXT NOT NULL, book_id TEXT NOT NULL, page INTEGER NOT NULL, page_count INTEGER NOT NULL, updated_at INTEGER NOT NULL, revision TEXT NOT "
                                   "NULL, device_id TEXT NOT NULL, PRIMARY KEY(device_id, url))"),
                    QStringLiteral("INSERT INTO legacy_reading_history SELECT *, COALESCE((SELECT CASE WHEN COUNT(DISTINCT device_id) = 1 THEN MIN(device_id) ELSE '' END FROM book_locations), '') FROM legacy_reading_history_v1"),
                    QStringLiteral("DROP TABLE legacy_reading_history_v1"),
                    QStringLiteral("UPDATE reading_data_metadata SET value = '2' WHERE key = 'schema_version'")};
                for (const QString &statement : upgrade) {
                    if (!query.exec(statement)) {
                        *error = query.lastError().text();
                        database.rollback();
                        return false;
                    }
                }
            } else if (version != QLatin1String("2")) {
                *error = QStringLiteral("Unsupported reading database schema");
                database.rollback();
                return false;
            }
            query.finish();
        }
    }
    if (!database.commit()) {
        *error = database.lastError().text();
        database.rollback();
        return false;
    }
    return true;
}

bool ReadingDataStore::ensureBook(QSqlDatabase &database, const QString &hash, const QString &name, QString *error)
{
    if (!isBookHash(hash)) {
        *error = QStringLiteral("Invalid book content hash");
        return false;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO books(hash, file_name) VALUES (?, ?) ON CONFLICT(hash) DO UPDATE SET file_name = CASE WHEN excluded.file_name <> '' THEN excluded.file_name ELSE books.file_name END"));
    query.addBindValue(hash);
    query.addBindValue(name.isNull() ? QStringLiteral("") : name);
    return execute(query, error);
}

namespace
{
// Keep the source read-only and leave it in place for recovery after migration.
class LegacyDatabase
{
public:
    explicit LegacyDatabase(const QString &path)
        : database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QUuid::createUuid().toString()))
    {
        database.setDatabaseName(path);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
    }
    ~LegacyDatabase()
    {
        const QString name = database.connectionName();
        database = {};
        QSqlDatabase::removeDatabase(name);
    }
    QSqlDatabase database;
};

bool copyRows(QSqlDatabase &source, QSqlDatabase &target, const QString &select, const QString &insert, QString *error, const QString &hash = {}, bool decodeImage = false)
{
    QSqlQuery read(source);
    QSqlQuery write(target);
    if (!read.exec(select) || !write.prepare(insert)) {
        *error = read.lastError().isValid() ? read.lastError().text() : write.lastError().text();
        return false;
    }
    while (read.next()) {
        int column = 0;
        if (!hash.isEmpty()) {
            write.bindValue(column++, hash);
        }
        for (int i = 0; i < read.record().count(); ++i) {
            QVariant value = read.value(i);
            if (decodeImage && i == read.record().count() - 1) {
                value = QByteArray::fromBase64(value.toString().toLatin1());
                if (value.toByteArray().isNull()) {
                    value = QByteArray("");
                }
            }
            write.bindValue(column++, value);
        }
        if (decodeImage) {
            write.bindValue(column++, read.value(0));
            write.bindValue(column, read.value(1));
        }
        if (!execute(write, error)) {
            return false;
        }
    }
    if (read.lastError().isValid()) {
        *error = read.lastError().text();
        return false;
    }
    return true;
}

bool finishImport(QSqlDatabase &database, bool success, QString *error)
{
    if (success && database.commit()) {
        return true;
    }
    if (success) {
        *error = database.lastError().text();
    }
    database.rollback();
    return false;
}
}

bool ReadingDataStore::importAiHistory(QSqlDatabase &database, const QString &source, QString *error)
{
    if (!QFileInfo::exists(source) || QFileInfo(source).absoluteFilePath() == QFileInfo(database.databaseName()).absoluteFilePath()) {
        return true;
    }
    LegacyDatabase legacy(source);
    if (!legacy.database.open() || !legacy.database.transaction()) {
        *error = legacy.database.lastError().text();
        return false;
    }
    if (!database.transaction()) {
        *error = database.lastError().text();
        return false;
    }
    // Record which conversations are already present, including cleared ones.
    QSqlQuery query(database);
    bool ok = query.exec(QStringLiteral("CREATE TEMP TABLE importing_ai (document_key TEXT, profile_id TEXT, PRIMARY KEY(document_key, profile_id))"));
    QSqlQuery conversations(legacy.database);
    ok = ok && conversations.exec(QStringLiteral("SELECT document_key, profile_id, session_id, instructions FROM ai_conversations"));
    while (ok && conversations.next()) {
        const QString hash = conversations.value(0).toString();
        if (!isBookHash(hash)) {
            continue;
        }
        ok = ensureBook(database, hash, {}, error);
        query.prepare(QStringLiteral("INSERT OR IGNORE INTO ai_conversations VALUES (?, ?, ?, ?)"));
        for (int i = 0; i < 4; ++i) {
            query.bindValue(i, conversations.value(i));
        }
        ok = ok && execute(query, error);
        if (ok && query.numRowsAffected() > 0) {
            query.prepare(QStringLiteral("INSERT INTO importing_ai VALUES (?, ?)"));
            query.addBindValue(hash);
            query.addBindValue(conversations.value(1));
            ok = execute(query, error);
        }
    }
    if (ok && conversations.lastError().isValid()) {
        *error = conversations.lastError().text();
        ok = false;
    }
    if (ok) {
        // Each insert is guarded by the temporary list of newly imported rows.
        ok = copyRows(legacy.database,
                      database,
                      QStringLiteral("SELECT document_key, profile_id, ordinal, role, content, page, page_text, selected_text, page_image FROM ai_messages ORDER BY ordinal"),
                      QStringLiteral("INSERT INTO ai_messages SELECT ?, ?, ?, ?, ?, ?, ?, ?, ? WHERE EXISTS (SELECT 1 FROM importing_ai WHERE document_key = ? AND profile_id = ?)"),
                      error,
                      {},
                      true);
    }
    if (!ok && error->isEmpty()) {
        *error = conversations.lastError().isValid() ? conversations.lastError().text() : query.lastError().text();
    }
    const bool success = finishImport(database, ok, error);
    query.exec(QStringLiteral("DROP TABLE IF EXISTS importing_ai"));
    return success;
}

bool ReadingDataStore::importReadingHistory(QSqlDatabase &database, const QString &source, QString *error)
{
    if (!QFileInfo::exists(source) || QFileInfo(source).absoluteFilePath() == QFileInfo(database.databaseName()).absoluteFilePath()) {
        return true;
    }
    const QString owner = deviceId();
    if (owner.isEmpty()) {
        *error = QStringLiteral("Could not prepare the local device identity");
        return false;
    }
    LegacyDatabase legacy(source);
    if (!legacy.database.open() || !legacy.database.transaction()) {
        *error = legacy.database.lastError().text();
        return false;
    }
    if (!database.transaction()) {
        *error = database.lastError().text();
        return false;
    }
    return finishImport(database,
                        copyRows(legacy.database,
                                 database,
                                 QStringLiteral("SELECT url, title, book_id, page, page_count, updated_at, revision FROM reading_history"),
                                 QStringLiteral("INSERT OR IGNORE INTO legacy_reading_history(device_id, url, title, book_id, page, page_count, updated_at, revision) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"),
                                 error,
                                 owner),
                        error);
}

bool ReadingDataStore::importAnnotations(QSqlDatabase &database, const QString &directory, QString *error, const QString &onlyHash)
{
    const QDir root(directory);
    if (onlyHash.isEmpty()) {
        for (const QString &file : root.entryList({QStringLiteral("*.sqlite")}, QDir::Files)) {
            const QString hash = QFileInfo(file).completeBaseName();
            if (!isBookHash(hash)) {
                continue;
            }
            QString importError;
            if (!importAnnotations(database, directory, &importError, hash)) {
                qWarning() << "Could not import annotations for" << hash << importError;
            }
        }
        return true;
    }
    const QStringList files = onlyHash.isEmpty() ? root.entryList({QStringLiteral("*.sqlite")}, QDir::Files) : QStringList {onlyHash + QStringLiteral(".sqlite")};
    for (const QString &file : files) {
        const QString hash = QFileInfo(file).completeBaseName();
        if (!isBookHash(hash) || !QFileInfo::exists(root.filePath(file))) {
            continue;
        }
        QSqlQuery existing(database);
        existing.prepare(QStringLiteral("SELECT 1 FROM annotation_documents WHERE book_hash = ?"));
        existing.addBindValue(hash);
        if (!execute(existing, error)) {
            return false;
        }
        if (existing.next()) {
            continue;
        }
        existing.finish();
        LegacyDatabase legacy(root.filePath(file));
        if (!legacy.database.open() || !legacy.database.transaction()) {
            *error = legacy.database.lastError().text();
            return false;
        }
        QSqlQuery metadata(legacy.database);
        if (!metadata.exec(QStringLiteral("SELECT key, value FROM metadata"))) {
            *error = metadata.lastError().text();
            return false;
        }
        QMap<QString, QString> values;
        while (metadata.next()) {
            values.insert(metadata.value(0).toString(), metadata.value(1).toString());
        }
        if (metadata.lastError().isValid()) {
            *error = metadata.lastError().text();
            return false;
        }
        if (values.value(QStringLiteral("schema_version")) != QLatin1String("2") || values.value(QStringLiteral("pdf_sha256")) != hash) {
            *error = QStringLiteral("Legacy annotation database schema or PDF hash does not match");
            return false;
        }
        if (!database.transaction()) {
            *error = database.lastError().text();
            return false;
        }
        QSqlQuery marker(database);
        marker.prepare(QStringLiteral("INSERT OR IGNORE INTO annotation_documents VALUES (?)"));
        marker.addBindValue(hash);
        bool ok = execute(marker, error);
        if (ok && marker.numRowsAffected() > 0) {
            ok = ensureBook(database, hash, {}, error) &&
                copyRows(legacy.database,
                         database,
                         QStringLiteral("SELECT sequence, annotation_id, page, subtype, xml, operation, recorded_utc FROM events ORDER BY sequence"),
                         QStringLiteral("INSERT INTO annotation_events VALUES (?, ?, ?, ?, ?, ?, ?, ?)"),
                         error,
                         hash) &&
                copyRows(legacy.database,
                         database,
                         QStringLiteral("SELECT page, annotation_id, subtype, contents, author, color, hidden_native, xml FROM annotations"),
                         QStringLiteral("INSERT INTO annotations VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"),
                         error,
                         hash);
        }
        if (!finishImport(database, ok, error)) {
            return false;
        }
    }
    return true;
}

bool ReadingDataStore::importLegacyData(QSqlDatabase &database, LegacyData kind, QString *error)
{
    const QString marker = kind == LegacyData::AiHistory ? QStringLiteral("legacy_ai_import_complete") : QStringLiteral("legacy_reading_import_complete");
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT value FROM reading_data_metadata WHERE key = ? OR key = 'legacy_import_complete'"));
    query.addBindValue(marker);
    if (!execute(query, error)) {
        return false;
    }
    if (query.next()) {
        return true;
    }
    query.finish();
    const QString app = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (kind == LegacyData::AiHistory) {
        if (!importAiHistory(database, app + QStringLiteral("/ai-history.sqlite"), error)) {
            return false;
        }
    } else {
        if (!importReadingHistory(database, app + QStringLiteral("/reading-history.sqlite"), error) ||
            !importAnnotations(database, QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/okular/annotations"), error)) {
            return false;
        }
    }
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO reading_data_metadata VALUES (?, '1')"));
    query.addBindValue(marker);
    return execute(query, error);
}

bool ReadingDataStore::exportDatabase(const QUrl &destination, QString *error)
{
    error->clear();
    if (!destination.isLocalFile() && destination.scheme() != QLatin1String("content")) {
        *error = QStringLiteral("Choose a local export destination");
        return false;
    }
    if (destination.isLocalFile()) {
        const QFileInfo database(defaultPath());
        const QFileInfo target(destination.toLocalFile());
        const QString canonicalDatabase = database.canonicalFilePath();
        const QString canonicalTarget = target.exists() ? target.canonicalFilePath() : QFileInfo(target.dir().canonicalPath(), target.fileName()).absoluteFilePath();
#ifdef Q_OS_WIN
        constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
        constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
        if (database == target || canonicalTarget.compare(canonicalDatabase, sensitivity) == 0 || canonicalTarget.startsWith(canonicalDatabase + QLatin1Char('-'), sensitivity)) {
            *error = QStringLiteral("Choose a destination outside the active reading database");
            return false;
        }
    }
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("reading-data.sqlite"));
    if (!directory.isValid() || !snapshot(defaultPath(), path, error)) {
        if (error->isEmpty()) {
            *error = QStringLiteral("Could not create export snapshot");
        }
        return false;
    }
    QFile source(path);
    QSaveFile localTarget(destination.toLocalFile());
    QFile contentTarget(destination.toString(QUrl::FullyEncoded));
    QFileDevice &target = destination.isLocalFile() ? static_cast<QFileDevice &>(localTarget) : static_cast<QFileDevice &>(contentTarget);
    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
        *error = target.isOpen() ? source.errorString() : target.errorString();
        return false;
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            *error = source.errorString();
            return false;
        }
        qint64 written = 0;
        while (written < chunk.size()) {
            const qint64 count = target.write(chunk.constData() + written, chunk.size() - written);
            if (count <= 0) {
                *error = target.errorString();
                return false;
            }
            written += count;
        }
    }
    const bool ok = destination.isLocalFile() ? localTarget.commit() : contentTarget.flush();
    if (!ok) {
        *error = target.errorString();
    }
    return ok;
}

bool ReadingDataStore::snapshot(const QString &sourcePath, const QString &destination, QString *error)
{
    error->clear();
    if (!QFileInfo::exists(sourcePath) || QFileInfo::exists(destination) || !QDir().mkpath(QFileInfo(destination).absolutePath())) {
        *error = QStringLiteral("Reading database source is missing or snapshot destination is unavailable");
        return false;
    }
    QTemporaryFile staging(QFileInfo(destination).absolutePath() + QStringLiteral("/.reading-snapshot-XXXXXX"));
    if (!staging.open()) {
        *error = staging.errorString();
        return false;
    }
    staging.close();
    sqlite3 *source = nullptr;
    sqlite3 *target = nullptr;
    int code = sqlite3_open_v2(sourcePath.toUtf8().constData(), &source, SQLITE_OPEN_READONLY, nullptr);
    if (code == SQLITE_OK) {
        code = sqlite3_open_v2(staging.fileName().toUtf8().constData(), &target, SQLITE_OPEN_READWRITE, nullptr);
    }
    if (code == SQLITE_OK) {
        sqlite3_busy_timeout(source, 5000);
        sqlite3_busy_timeout(target, 5000);
        sqlite3_backup *backup = sqlite3_backup_init(target, "main", source, "main");
        if (backup) {
            code = sqlite3_backup_step(backup, -1);
            const int finish = sqlite3_backup_finish(backup);
            if (code == SQLITE_DONE) {
                code = finish;
            }
        } else {
            code = sqlite3_errcode(target);
        }
    }
    if (code != SQLITE_OK) {
        *error = target || source ? QString::fromUtf8(sqlite3_errmsg(target ? target : source)) : QStringLiteral("Could not open reading database");
    }
    if (target) {
        sqlite3_close(target);
    }
    if (source) {
        sqlite3_close(source);
    }
    if (code != SQLITE_OK) {
        return false;
    }
    LegacyDatabase check(staging.fileName());
    if (!check.database.open()) {
        *error = check.database.lastError().text();
        return false;
    }
    QSqlQuery query(check.database);
    if (!query.exec(QStringLiteral("PRAGMA quick_check")) || !query.next() || query.value(0).toString() != QLatin1String("ok")) {
        *error = QStringLiteral("Reading database snapshot failed integrity check");
        return false;
    }
    query.finish();
    check.database.close();
    if (!QFile::rename(staging.fileName(), destination)) {
        *error = QStringLiteral("Could not install reading database snapshot");
        return false;
    }
    staging.setAutoRemove(false);
    return true;
}
