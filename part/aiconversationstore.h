/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QList>
#include <QString>
#include <memory>

class QSqlDatabase;

struct AiMessage {
    QString role;
    QString content;
    int page = -1;
    QString pageText;
    QString selectedText;
    QString pageImage; // JPEG encoded as base64; only user messages use this.
};

struct AiConversation {
    QString sessionId;
    QString instructions;
    QList<AiMessage> messages;
};

// Construct and use on one thread; no connection is shared with other stores.
class AiConversationStore
{
public:
    explicit AiConversationStore(const QString &path = defaultPath(), const QString &legacyDirectory = defaultLegacyDirectory());
    ~AiConversationStore();
    AiConversationStore(const AiConversationStore &) = delete;
    AiConversationStore &operator=(const AiConversationStore &) = delete;

    static QString defaultPath();
    static QString defaultLegacyDirectory();
    bool load(const QString &documentKey, const QString &profileId, AiConversation *conversation, QString *error);
    bool save(const QString &documentKey, const QString &profileId, const AiConversation &conversation, QString *error);
    bool clear(const QString &documentKey, const QString &profileId, QString *error);

private:
    bool open(QString *error);
    bool write(const QString &documentKey, const QString &profileId, const AiConversation &conversation, bool replace, QString *error);
    QString legacyPath(const QString &documentKey, const QString &profileId) const;
    QString m_path;
    QString m_legacyDirectory;
    std::unique_ptr<QSqlDatabase> m_database;
};
