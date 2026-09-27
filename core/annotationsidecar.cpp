/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "annotationsidecar_p.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

using namespace Okular;

namespace
{
class Connection
{
public:
    explicit Connection(const QString &path)
        : m_name(QUuid::createUuid().toString(QUuid::WithoutBraces))
        , m_db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name))
    {
        m_db.setDatabaseName(path);
        m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    }

    ~Connection()
    {
        m_db.close();
        m_db = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }

    bool open(QString *error)
    {
        if (m_db.open()) {
            return true;
        }
        *error = m_db.lastError().text();
        return false;
    }

    QSqlDatabase &db()
    {
        return m_db;
    }

private:
    QString m_name;
    QSqlDatabase m_db;
};

bool exec(QSqlQuery &query, QString *error)
{
    if (query.exec()) {
        return true;
    }
    *error = query.lastError().text();
    return false;
}

bool execSql(QSqlDatabase &db, const QString &sql, QString *error)
{
    QSqlQuery query(db);
    if (query.exec(sql)) {
        return true;
    }
    *error = query.lastError().text();
    return false;
}

bool verifyMetadata(QSqlDatabase &db, const QString &hash, QString *error)
{
    QSqlQuery query(db);
    if (!execSql(db, QStringLiteral("CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL)"), error)
        || !query.prepare(QStringLiteral("INSERT OR IGNORE INTO metadata(key, value) VALUES ('schema_version', '2'), ('pdf_sha256', ?)"))) {
        if (error->isEmpty()) {
            *error = query.lastError().text();
        }
        return false;
    }
    query.addBindValue(hash);
    if (!exec(query, error)) {
        return false;
    }
    if (!query.exec(QStringLiteral("SELECT key, value FROM metadata"))) {
        *error = query.lastError().text();
        return false;
    }
    QMap<QString, QString> metadata;
    while (query.next()) {
        metadata.insert(query.value(0).toString(), query.value(1).toString());
    }
    if (metadata.value(QStringLiteral("schema_version")) != QLatin1String("2") || metadata.value(QStringLiteral("pdf_sha256")) != hash) {
        *error = QStringLiteral("Annotation database schema or PDF hash does not match");
        return false;
    }
    return true;
}

bool checkMetadata(QSqlDatabase &db, const QString &hash, QString *error)
{
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT key, value FROM metadata"))) {
        *error = query.lastError().text();
        return false;
    }
    QMap<QString, QString> metadata;
    while (query.next()) {
        metadata.insert(query.value(0).toString(), query.value(1).toString());
    }
    if (metadata.value(QStringLiteral("schema_version")) != QLatin1String("2") || metadata.value(QStringLiteral("pdf_sha256")) != hash) {
        *error = QStringLiteral("Annotation database schema or PDF hash does not match");
        return false;
    }
    return true;
}

QString annotationKey(int page, const QString &id)
{
    return QString::number(page) + QLatin1Char('\x1f') + id;
}
}

QString AnnotationSidecar::pdfHash(const QString &pdfPath, QString *error)
{
    error->clear();
    QFile pdf(pdfPath);
    if (!pdf.open(QIODevice::ReadOnly)) {
        *error = pdf.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&pdf)) {
        *error = pdf.errorString();
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString AnnotationSidecar::pathForHash(const QString &hash)
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/okular/annotations/") + hash + QStringLiteral(".sqlite");
}

bool AnnotationSidecar::load(const QString &hash, QList<SidecarAnnotation> *annotations, QString *error, qint64 *revision)
{
    error->clear();
    annotations->clear();
    if (revision) {
        *revision = 0;
    }
    const QString path = pathForHash(hash);
    if (!QFileInfo::exists(path)) {
        return true;
    }

    Connection connection(path);
    if (!connection.open(error) || !checkMetadata(connection.db(), hash, error)) {
        return false;
    }

    // The event stream is authoritative. The annotations table is a query index.
    QSqlQuery query(connection.db());
    if (!query.exec(QStringLiteral("SELECT annotation_id, page, subtype, xml, operation, sequence FROM events ORDER BY sequence"))) {
        *error = query.lastError().text();
        return false;
    }
    QMap<QString, SidecarAnnotation> state;
    while (query.next()) {
        if (revision) {
            *revision = query.value(5).toLongLong();
        }
        const QString id = query.value(0).toString();
        const QString operation = query.value(4).toString();
        const int page = query.value(1).toInt();
        const QString key = annotationKey(page, id);
        if (id.isEmpty() || page < 0) {
            *error = QStringLiteral("Invalid annotation event key");
            return false;
        }
        if (operation == QLatin1String("delete")) {
            state.remove(key);
        } else if (operation == QLatin1String("upsert") || operation == QLatin1String("hide")) {
            SidecarAnnotation annotation {id, page, query.value(2).toInt(), query.value(3).toString()};
            annotation.hiddenNative = operation == QLatin1String("hide");
            if (!annotation.hiddenNative && annotation.xml.isEmpty()) {
                *error = QStringLiteral("Invalid annotation event");
                return false;
            }
            state.insert(key, annotation);
        } else {
            *error = QStringLiteral("Unknown annotation event operation");
            return false;
        }
    }
    *annotations = state.values();
    return true;
}

bool AnnotationSidecar::save(const QString &hash, const QList<SidecarAnnotation> &annotations, QString *error, qint64 expectedRevision, qint64 *newRevision)
{
    error->clear();
    QMap<QString, SidecarAnnotation> newState;
    for (const SidecarAnnotation &annotation : annotations) {
        const QString key = annotationKey(annotation.page, annotation.id);
        if (annotation.id.isEmpty() || annotation.page < 0 || (!annotation.hiddenNative && annotation.xml.isEmpty()) || newState.contains(key)) {
            *error = QStringLiteral("Invalid or duplicate annotation ID");
            return false;
        }
        newState.insert(key, annotation);
    }

    const QString path = pathForHash(hash);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        *error = QStringLiteral("Could not create annotation directory");
        return false;
    }
    Connection connection(path);
    if (!connection.open(error) || !connection.db().transaction()) {
        if (error->isEmpty()) {
            *error = connection.db().lastError().text();
        }
        return false;
    }
    QSqlDatabase &db = connection.db();
    if (!verifyMetadata(db, hash, error)
        || !execSql(db, QStringLiteral("CREATE TABLE IF NOT EXISTS annotations (page INTEGER NOT NULL, annotation_id TEXT NOT NULL, subtype INTEGER NOT NULL, contents TEXT, author TEXT, color TEXT, hidden_native INTEGER NOT NULL DEFAULT 0, xml TEXT, PRIMARY KEY(page, annotation_id))"), error)
        || !execSql(db, QStringLiteral("CREATE INDEX IF NOT EXISTS annotations_by_page ON annotations(page, subtype)"), error)
        || !execSql(db, QStringLiteral("CREATE INDEX IF NOT EXISTS annotations_by_contents ON annotations(contents)"), error)
        || !execSql(db,
                    QStringLiteral("CREATE TABLE IF NOT EXISTS events (sequence INTEGER PRIMARY KEY AUTOINCREMENT, annotation_id TEXT NOT NULL, page INTEGER NOT NULL, subtype INTEGER NOT NULL, xml TEXT, operation TEXT NOT NULL CHECK (operation IN ('upsert', 'delete', 'hide')), recorded_utc TEXT NOT NULL)"),
                    error)
        || !execSql(db, QStringLiteral("CREATE INDEX IF NOT EXISTS events_by_annotation ON events(page, annotation_id, sequence)"), error)) {
        db.rollback();
        return false;
    }

    qint64 revision = 0;
    {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT COALESCE(MAX(sequence), 0) FROM events")) || !query.next()) {
            *error = query.lastError().text();
            db.rollback();
            return false;
        }
        revision = query.value(0).toLongLong();
    }
    if (expectedRevision >= 0 && revision != expectedRevision) {
        *error = QStringLiteral("Annotation database changed since the PDF was opened");
        db.rollback();
        return false;
    }

    QMap<QString, SidecarAnnotation> oldState;
    {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT annotation_id, page, subtype, xml, hidden_native FROM annotations"))) {
            *error = query.lastError().text();
            db.rollback();
            return false;
        }
        while (query.next()) {
            SidecarAnnotation annotation {query.value(0).toString(), query.value(1).toInt(), query.value(2).toInt(), query.value(3).toString()};
            annotation.hiddenNative = query.value(4).toBool();
            oldState.insert(annotationKey(annotation.page, annotation.id), annotation);
        }
    }

    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery event(db);
    QSqlQuery upsert(db);
    QSqlQuery remove(db);
    if (!event.prepare(QStringLiteral("INSERT INTO events(annotation_id, page, subtype, xml, operation, recorded_utc) VALUES (?, ?, ?, ?, ?, ?)"))) {
        *error = event.lastError().text();
        db.rollback();
        return false;
    }
    if (!upsert.prepare(QStringLiteral("INSERT OR REPLACE INTO annotations(page, annotation_id, subtype, contents, author, color, hidden_native, xml) VALUES (?, ?, ?, ?, ?, ?, ?, ?)"))) {
        *error = upsert.lastError().text();
        db.rollback();
        return false;
    }
    if (!remove.prepare(QStringLiteral("DELETE FROM annotations WHERE page = ? AND annotation_id = ?"))) {
        *error = remove.lastError().text();
        db.rollback();
        return false;
    }

    for (auto it = oldState.cbegin(); it != oldState.cend(); ++it) {
        if (newState.contains(it.key())) {
            continue;
        }
        event.bindValue(0, it->id);
        event.bindValue(1, it->page);
        event.bindValue(2, it->subtype);
        event.bindValue(3, QVariant());
        event.bindValue(4, QStringLiteral("delete"));
        event.bindValue(5, now);
        remove.bindValue(0, it->page);
        remove.bindValue(1, it->id);
        if (!exec(event, error) || !exec(remove, error)) {
            db.rollback();
            return false;
        }
        ++revision;
    }
    for (auto it = newState.cbegin(); it != newState.cend(); ++it) {
        const auto previous = oldState.constFind(it.key());
        if (previous != oldState.cend() && previous->subtype == it->subtype && previous->xml == it->xml && previous->hiddenNative == it->hiddenNative) {
            continue;
        }
        event.bindValue(0, it->id);
        event.bindValue(1, it->page);
        event.bindValue(2, it->subtype);
        event.bindValue(3, it->hiddenNative ? QVariant() : QVariant(it->xml));
        event.bindValue(4, it->hiddenNative ? QStringLiteral("hide") : QStringLiteral("upsert"));
        event.bindValue(5, now);
        upsert.bindValue(0, it->page);
        upsert.bindValue(1, it->id);
        upsert.bindValue(2, it->subtype);
        upsert.bindValue(3, it->contents);
        upsert.bindValue(4, it->author);
        upsert.bindValue(5, it->color);
        upsert.bindValue(6, it->hiddenNative);
        upsert.bindValue(7, it->hiddenNative ? QVariant() : QVariant(it->xml));
        if (!exec(event, error) || !exec(upsert, error)) {
            db.rollback();
            return false;
        }
        ++revision;
    }
    if (!db.commit()) {
        *error = db.lastError().text();
        return false;
    }
    if (newRevision) {
        *newRevision = revision;
    }
    return true;
}
