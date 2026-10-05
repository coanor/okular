/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QList>
#include <QString>
#include <QUrl>
#include <QtGui/qwindowdefs.h>

struct AiProfile {
    enum class Kind { OpenAiChat, OpenAiResponses, Anthropic, Codex };

    QString id;
    QString name;
    Kind kind = Kind::OpenAiChat;
    QString endpoint;
    QString model;
    QString extraArguments; // CLI options for Codex, JSON request fields for HTTP providers.
    bool vision = true;
    QString apiKey;           // Kept in memory and, when available, in KWallet.
    QString chatGptAccountId; // OAuth registration; tokens are stored separately on mobile.
};

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

struct AiBookSettings {
    QString selectedProfileId;
    QString defaultPrompt;
};

class AiStore
{
public:
    static QList<AiProfile> loadProfiles(WId windowId);
    static bool saveProfiles(const QList<AiProfile> &profiles, WId windowId, QString *error);
    static QString documentKey(const QUrl &url);
    static AiBookSettings loadBookSettings(const QString &documentKey);
    static bool isManagedBook(const QString &documentKey);
    static bool saveBookSettings(const QString &documentKey, const AiBookSettings &settings);
    static AiConversation loadConversation(const QString &documentKey, const QString &profileId);
    static bool saveConversation(const QString &documentKey, const QString &profileId, const AiConversation &conversation);
    static bool clearConversation(const QString &documentKey, const QString &profileId);
};
