/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "aistore.h"
#include <config-okular.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <utility>

#include <KConfigGroup>
#include <KSharedConfig>

#if HAVE_KWALLET
#include <KWallet>
#endif

namespace
{
QString conversationPath(const QString &documentKey, const QString &profileId)
{
    const QByteArray profileHash = QCryptographicHash::hash(profileId.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ai-conversations");
    return directory + QLatin1Char('/') + documentKey + QLatin1Char('-') + QString::fromLatin1(profileHash) + QStringLiteral(".json");
}

QJsonObject messageToJson(const AiMessage &message)
{
    QJsonObject json;
    json.insert(QStringLiteral("role"), message.role);
    json.insert(QStringLiteral("content"), message.content);
    json.insert(QStringLiteral("page"), message.page);
    json.insert(QStringLiteral("pageText"), message.pageText);
    json.insert(QStringLiteral("selectedText"), message.selectedText);
    json.insert(QStringLiteral("pageImage"), message.pageImage);
    return json;
}

AiMessage messageFromJson(const QJsonObject &json)
{
    AiMessage message;
    message.role = json.value(QStringLiteral("role")).toString();
    message.content = json.value(QStringLiteral("content")).toString();
    message.page = json.value(QStringLiteral("page")).toInt(-1);
    message.pageText = json.value(QStringLiteral("pageText")).toString();
    message.selectedText = json.value(QStringLiteral("selectedText")).toString();
    message.pageImage = json.value(QStringLiteral("pageImage")).toString();
    return message;
}
}

QList<AiProfile> AiStore::loadProfiles(WId windowId)
{
    QList<AiProfile> profiles;
    const auto config = KSharedConfig::openConfig();
    const KConfigGroup group(config, QStringLiteral("AI Reading Assistant"));
    const QJsonDocument document = QJsonDocument::fromJson(group.readEntry("Profiles", QByteArray()));
#if HAVE_KWALLET
    KWallet::Wallet *wallet = nullptr;
    if (KWallet::Wallet::isEnabled()) {
        wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), windowId);
        if (wallet && !wallet->hasFolder(QStringLiteral("Okular AI Reading Assistant"))) {
            wallet->createFolder(QStringLiteral("Okular AI Reading Assistant"));
        }
        if (wallet) {
            wallet->setFolder(QStringLiteral("Okular AI Reading Assistant"));
        }
    }
#else
    Q_UNUSED(windowId)
#endif
    for (const QJsonValue &value : document.array()) {
        const QJsonObject json = value.toObject();
        AiProfile profile;
        profile.id = json.value(QStringLiteral("id")).toString();
        profile.name = json.value(QStringLiteral("name")).toString();
        profile.kind = static_cast<AiProfile::Kind>(json.value(QStringLiteral("kind")).toInt());
        profile.endpoint = json.value(QStringLiteral("endpoint")).toString();
        profile.model = json.value(QStringLiteral("model")).toString();
        profile.vision = json.value(QStringLiteral("vision")).toBool(true);
        if (profile.id.isEmpty() || profile.name.isEmpty()) {
            continue;
        }
#if HAVE_KWALLET
        if (wallet) {
            wallet->readPassword(profile.id, profile.apiKey);
        }
#endif
        profiles.append(profile);
    }
#if HAVE_KWALLET
    delete wallet;
#endif
    return profiles;
}

bool AiStore::saveProfiles(const QList<AiProfile> &profiles, WId windowId, QString *error)
{
    QJsonArray list;
#if HAVE_KWALLET
    KWallet::Wallet *wallet = nullptr;
    if (KWallet::Wallet::isEnabled()) {
        wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), windowId);
        if (wallet && !wallet->hasFolder(QStringLiteral("Okular AI Reading Assistant"))) {
            wallet->createFolder(QStringLiteral("Okular AI Reading Assistant"));
        }
        if (wallet) {
            wallet->setFolder(QStringLiteral("Okular AI Reading Assistant"));
        }
    }
#else
    Q_UNUSED(windowId)
#endif
    for (const AiProfile &profile : profiles) {
        QJsonObject json;
        json.insert(QStringLiteral("id"), profile.id);
        json.insert(QStringLiteral("name"), profile.name);
        json.insert(QStringLiteral("kind"), static_cast<int>(profile.kind));
        json.insert(QStringLiteral("endpoint"), profile.endpoint);
        json.insert(QStringLiteral("model"), profile.model);
        json.insert(QStringLiteral("vision"), profile.vision);
        list.append(json);
#if HAVE_KWALLET
        if (wallet && !profile.apiKey.isEmpty() && wallet->writePassword(profile.id, profile.apiKey) != 0 && error) {
            *error = QStringLiteral("Failed to save API key to KWallet");
        }
#else
        if (!profile.apiKey.isEmpty() && error) {
            *error = QStringLiteral("No wallet is available; API keys will only be kept until Okular exits");
        }
#endif
    }
#if HAVE_KWALLET
    delete wallet;
#endif
    auto config = KSharedConfig::openConfig();
    KConfigGroup group(config, QStringLiteral("AI Reading Assistant"));
    group.writeEntry("Profiles", QJsonDocument(list).toJson(QJsonDocument::Compact));
    config->sync();
    return true;
}

QString AiStore::documentKey(const QUrl &url)
{
    if (!url.isValid() || url.isEmpty()) {
        return {};
    }
    if (url.isLocalFile()) {
        QFile file(url.toLocalFile());
        if (file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&file)) {
                return QString::fromLatin1(hash.result().toHex());
            }
        }
    }
    return QString::fromLatin1(QCryptographicHash::hash(url.toString(QUrl::FullyEncoded).toUtf8(), QCryptographicHash::Sha256).toHex());
}

AiConversation AiStore::loadConversation(const QString &documentKey, const QString &profileId)
{
    AiConversation conversation;
    if (documentKey.isEmpty() || profileId.isEmpty()) {
        return conversation;
    }
    QFile file(conversationPath(documentKey, profileId));
    if (!file.open(QIODevice::ReadOnly)) {
        return conversation;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    conversation.sessionId = root.value(QStringLiteral("sessionId")).toString();
    for (const QJsonValue &value : root.value(QStringLiteral("messages")).toArray()) {
        AiMessage message = messageFromJson(value.toObject());
        if (message.role == QLatin1String("user") || message.role == QLatin1String("assistant")) {
            conversation.messages.append(std::move(message));
        }
    }
    return conversation;
}

bool AiStore::saveConversation(const QString &documentKey, const QString &profileId, const AiConversation &conversation)
{
    if (documentKey.isEmpty() || profileId.isEmpty()) {
        return false;
    }
    const QString path = conversationPath(documentKey, profileId);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("sessionId"), conversation.sessionId);
    QJsonArray messages;
    for (const AiMessage &message : conversation.messages) {
        messages.append(messageToJson(message));
    }
    root.insert(QStringLiteral("messages"), messages);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0) {
        return false;
    }
    return file.commit();
}

bool AiStore::clearConversation(const QString &documentKey, const QString &profileId)
{
    const QString path = conversationPath(documentKey, profileId);
    return !QFile::exists(path) || QFile::remove(path);
}
