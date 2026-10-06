/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "aiconversationstore.h"
#include "core/readingdatastore_p.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <utility>

namespace
{
QString nonNull(const QString &text)
{
    return text.isNull() ? QStringLiteral("") : text;
}
}

AiConversationStore::AiConversationStore(const QString &path, const QString &legacyDirectory)
    : m_path(path)
    , m_legacyDirectory(legacyDirectory)
{
}

AiConversationStore::~AiConversationStore()
{
    if (m_database) {
        const QString name = m_database->connectionName();
        m_database.reset();
        QSqlDatabase::removeDatabase(name);
    }
}

QString AiConversationStore::defaultPath()
{
    return Okular::ReadingDataStore::defaultPath();
}

QString AiConversationStore::defaultLegacyDirectory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ai-conversations");
}

QString AiConversationStore::legacyPath(const QString &documentKey, const QString &profileId) const
{
    if (m_legacyDirectory.isEmpty() || !QRegularExpression(QStringLiteral("^[a-zA-Z0-9_-]+$")).match(documentKey).hasMatch()) {
        return {};
    }
    const QByteArray profileHash = QCryptographicHash::hash(profileId.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(m_legacyDirectory).filePath(documentKey + QLatin1Char('-') + QString::fromLatin1(profileHash) + QStringLiteral(".json"));
}

bool AiConversationStore::open(QString *error)
{
    if (m_database && m_database->isOpen()) {
        return true;
    }
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath())) {
        *error = QStringLiteral("Could not create the AI history directory");
        return false;
    }
    if (!m_database) {
        m_database = std::make_unique<QSqlDatabase>(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QUuid::createUuid().toString()));
        m_database->setDatabaseName(m_path);
        m_database->setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    }
    if (!m_database->open()) {
        *error = m_database->lastError().text();
        return false;
    }
    if (!Okular::ReadingDataStore::initialize(*m_database, error) || (m_path == defaultPath() && !Okular::ReadingDataStore::importLegacyData(*m_database, Okular::ReadingDataStore::LegacyData::AiHistory, error))) {
        m_database->close();
        return false;
    }
    return true;
}

bool AiConversationStore::load(const QString &documentKey, const QString &profileId, AiConversation *conversation, QString *error)
{
    error->clear();
    if (!Okular::ReadingDataStore::isBookHash(documentKey) || profileId.isEmpty()) {
        *error = QStringLiteral("Missing AI conversation identity");
        return false;
    }
    if (!open(error)) {
        return false;
    }
    // Read metadata and messages from the same snapshot if another process saves.
    if (!m_database->transaction()) {
        *error = m_database->lastError().text();
        return false;
    }
    AiConversation loaded;
    bool found = false;
    {
        QSqlQuery query(*m_database);
        query.prepare(QStringLiteral("SELECT session_id, instructions FROM ai_conversations WHERE document_key = ? AND profile_id = ?"));
        query.addBindValue(documentKey);
        query.addBindValue(profileId);
        if (!query.exec()) {
            *error = query.lastError().text();
        } else if (query.next()) {
            found = true;
            loaded.sessionId = query.value(0).toString();
            loaded.instructions = query.value(1).toString();
        }
        query.finish();
        if (found) {
            query.prepare(QStringLiteral("SELECT role, content, page, page_text, selected_text, page_image FROM ai_messages WHERE document_key = ? AND profile_id = ? ORDER BY ordinal"));
            query.addBindValue(documentKey);
            query.addBindValue(profileId);
            if (!query.exec()) {
                *error = query.lastError().text();
            } else {
                while (query.next()) {
                    loaded.messages.append({query.value(0).toString(), query.value(1).toString(), query.value(2).toInt(), query.value(3).toString(), query.value(4).toString(), QString::fromLatin1(query.value(5).toByteArray().toBase64())});
                }
            }
        }
    }
    if (!error->isEmpty()) {
        m_database->rollback();
        return false;
    }
    if (!m_database->commit()) {
        *error = m_database->lastError().text();
        m_database->rollback();
        return false;
    }
    if (found) {
        *conversation = std::move(loaded);
        return true;
    }

    // Import the old JSON only when SQLite has no record for this identity.
    QFile legacy(legacyPath(documentKey, profileId));
    if (!legacy.exists()) {
        return true;
    }
    if (!legacy.open(QIODevice::ReadOnly)) {
        *error = legacy.errorString();
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(legacy.readAll(), &parseError);
    const QJsonObject root = json.object();
    if (parseError.error != QJsonParseError::NoError || !json.isObject() || root.value(QStringLiteral("version")).toInt() != 1 || !root.value(QStringLiteral("messages")).isArray()) {
        *error = QStringLiteral("Invalid legacy AI conversation");
        return false;
    }
    loaded.sessionId = root.value(QStringLiteral("sessionId")).toString();
    loaded.instructions = root.value(QStringLiteral("instructions")).toString(conversation->instructions);
    for (const QJsonValue &value : root.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject message = value.toObject();
        const QString role = message.value(QStringLiteral("role")).toString();
        if (role == QLatin1String("user") || role == QLatin1String("assistant")) {
            loaded.messages.append({role,
                                    message.value(QStringLiteral("content")).toString(),
                                    message.value(QStringLiteral("page")).toInt(-1),
                                    message.value(QStringLiteral("pageText")).toString(),
                                    message.value(QStringLiteral("selectedText")).toString(),
                                    message.value(QStringLiteral("pageImage")).toString()});
        }
    }
    legacy.close();
    if (!write(documentKey, profileId, loaded, false, error)) {
        return false;
    }
    // Another process may have saved while the legacy file was being read.
    return load(documentKey, profileId, conversation, error);
}

bool AiConversationStore::save(const QString &documentKey, const QString &profileId, const AiConversation &conversation, QString *error)
{
    return write(documentKey, profileId, conversation, true, error);
}

bool AiConversationStore::write(const QString &documentKey, const QString &profileId, const AiConversation &conversation, bool replace, QString *error)
{
    error->clear();
    if (!Okular::ReadingDataStore::isBookHash(documentKey) || profileId.isEmpty()) {
        *error = QStringLiteral("Missing AI conversation identity");
        return false;
    }
    if (!open(error)) {
        return false;
    }
    if (!m_database->transaction()) {
        *error = m_database->lastError().text();
        return false;
    }
    if (!Okular::ReadingDataStore::ensureBook(*m_database, documentKey, {}, error)) {
        m_database->rollback();
        return false;
    }
    QSqlQuery query(*m_database);
    const auto execute = [&] {
        if (query.exec()) {
            return true;
        }
        *error = query.lastError().text();
        m_database->rollback();
        return false;
    };
    query.prepare(replace ? QStringLiteral("INSERT INTO ai_conversations VALUES (?, ?, ?, ?) ON CONFLICT(document_key, profile_id) DO UPDATE SET session_id=excluded.session_id, instructions=excluded.instructions")
                          : QStringLiteral("INSERT OR IGNORE INTO ai_conversations VALUES (?, ?, ?, ?)"));
    query.addBindValue(documentKey);
    query.addBindValue(profileId);
    query.addBindValue(nonNull(conversation.sessionId));
    query.addBindValue(nonNull(conversation.instructions));
    if (!execute()) {
        return false;
    }
    if (!replace && query.numRowsAffected() == 0) {
        m_database->rollback();
        return true;
    }
    query.prepare(QStringLiteral("DELETE FROM ai_messages WHERE document_key = ? AND profile_id = ?"));
    query.addBindValue(documentKey);
    query.addBindValue(profileId);
    if (!execute()) {
        return false;
    }
    query.prepare(QStringLiteral("INSERT INTO ai_messages VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    int ordinal = 0;
    for (const AiMessage &message : conversation.messages) {
        query.bindValue(0, documentKey);
        query.bindValue(1, profileId);
        query.bindValue(2, ordinal++);
        query.bindValue(3, nonNull(message.role));
        query.bindValue(4, nonNull(message.content));
        query.bindValue(5, message.page);
        query.bindValue(6, nonNull(message.pageText));
        query.bindValue(7, nonNull(message.selectedText));
        const QByteArray image = QByteArray::fromBase64(message.pageImage.toLatin1());
        query.bindValue(8, image.isNull() ? QByteArray("") : image);
        if (!execute()) {
            return false;
        }
    }
    if (!m_database->commit()) {
        *error = m_database->lastError().text();
        m_database->rollback();
        return false;
    }
    return true;
}

bool AiConversationStore::clear(const QString &documentKey, const QString &profileId, QString *error)
{
    // Keep an empty row so an undeletable legacy file cannot revive the history.
    return save(documentKey, profileId, {}, error);
}
