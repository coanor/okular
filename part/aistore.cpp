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
#include <QLockFile>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
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

QString bookSettingsPath(const QString &documentKey)
{
    if (documentKey.size() != 64 || !std::all_of(documentKey.cbegin(), documentKey.cend(), [](QChar c) { return c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')); })) {
        return {};
    }
    const KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("Cloud Book Library"));
    const QString root = group.readEntry("ManagedDirectory", QString());
    const QString project = QDir(root).filePath(QStringLiteral("books/") + documentKey);
    if (root.isEmpty() || !QFileInfo(project).isDir() || QFileInfo(project).isSymLink()) {
        return {};
    }
    return QDir(project).filePath(QStringLiteral("ai-settings.json"));
}

QString credentialScope(int kind, const QString &endpoint)
{
    QByteArray scope = QByteArray::number(kind);
    scope.append('\0');
    scope.append(endpoint.toUtf8());
    return QString::fromLatin1(QCryptographicHash::hash(scope, QCryptographicHash::Sha256).toHex());
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
    const QByteArray bindingsBytes = group.readEntry("CredentialBindings", QByteArray());
    const QJsonObject bindings = QJsonDocument::fromJson(bindingsBytes).object();
    KWallet::Wallet *wallet = nullptr;
    if (KWallet::Wallet::isEnabled()) {
        wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), windowId);
        if (wallet && !wallet->hasFolder(QStringLiteral("Okular AI Reading Assistant"))) {
            wallet->createFolder(QStringLiteral("Okular AI Reading Assistant"));
        }
        if (wallet) {
            if (!wallet->setFolder(QStringLiteral("Okular AI Reading Assistant"))) {
                delete wallet;
                wallet = nullptr;
            }
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
        profile.chatGptAccountId = json.value(QStringLiteral("chatGptAccountId")).toString();
        profile.extraArguments = json.value(QStringLiteral("extraArguments")).toString();
        profile.vision = json.value(QStringLiteral("vision")).toBool(true);
        if (profile.id.isEmpty() || profile.name.isEmpty()) {
            continue;
        }
#if HAVE_KWALLET
        if (wallet && (bindingsBytes.isEmpty() || bindings.value(profile.id).toString() == credentialScope(static_cast<int>(profile.kind), profile.endpoint))) {
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
    const auto config = KSharedConfig::openConfig();
#if HAVE_KWALLET
    const KConfigGroup group(config, QStringLiteral("AI Reading Assistant"));
    const QByteArray previousBindingsBytes = group.readEntry("CredentialBindings", QByteArray());
    const QJsonObject previousBindings = QJsonDocument::fromJson(previousBindingsBytes).object();
    QJsonObject bindings;
    QMap<QString, QJsonObject> previous;
    for (const QJsonValue &value : QJsonDocument::fromJson(group.readEntry("Profiles", QByteArray())).array()) {
        const QJsonObject profile = value.toObject();
        previous.insert(profile.value(QStringLiteral("id")).toString(), profile);
    }
    QSet<QString> retained;
    KWallet::Wallet *wallet = nullptr;
    if (KWallet::Wallet::isEnabled()) {
        wallet = KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), windowId);
        if (wallet && !wallet->hasFolder(QStringLiteral("Okular AI Reading Assistant"))) {
            wallet->createFolder(QStringLiteral("Okular AI Reading Assistant"));
        }
        if (wallet) {
            if (!wallet->setFolder(QStringLiteral("Okular AI Reading Assistant"))) {
                delete wallet;
                wallet = nullptr;
            }
        }
    }
#else
    Q_UNUSED(windowId)
#endif
    for (const AiProfile &profile : profiles) {
#if HAVE_KWALLET
        retained.insert(profile.id);
#endif
        QJsonObject json;
        json.insert(QStringLiteral("id"), profile.id);
        json.insert(QStringLiteral("name"), profile.name);
        json.insert(QStringLiteral("kind"), static_cast<int>(profile.kind));
        json.insert(QStringLiteral("endpoint"), profile.endpoint);
        json.insert(QStringLiteral("model"), profile.model);
        if (!profile.chatGptAccountId.isEmpty()) {
            json.insert(QStringLiteral("chatGptAccountId"), profile.chatGptAccountId);
        }
        json.insert(QStringLiteral("extraArguments"), profile.extraArguments);
        json.insert(QStringLiteral("vision"), profile.vision);
        list.append(json);
#if HAVE_KWALLET
        const QJsonObject old = previous.value(profile.id);
        const bool sameScope = !old.isEmpty() && old.value(QStringLiteral("kind")).toInt() == static_cast<int>(profile.kind) && old.value(QStringLiteral("endpoint")).toString() == profile.endpoint;
        const QString scope = credentialScope(static_cast<int>(profile.kind), profile.endpoint);
        if (wallet && !old.isEmpty() && !sameScope && wallet->hasEntry(profile.id) && wallet->removeEntry(profile.id) != 0) {
            if (error) {
                *error = QStringLiteral("Failed to remove the old API key after changing its endpoint");
            }
            delete wallet;
            return false;
        }
        if (wallet && !profile.apiKey.isEmpty()) {
            if (wallet->writePassword(profile.id, profile.apiKey) == 0) {
                bindings.insert(profile.id, scope);
            } else if (error) {
                *error = QStringLiteral("Failed to save API key to KWallet");
            }
        } else if (!wallet && !profile.apiKey.isEmpty() && error) {
            *error = QStringLiteral("KWallet is unavailable; API keys will only be kept until Okular exits");
        }
        if (sameScope && profile.apiKey.isEmpty() && (previousBindingsBytes.isEmpty() || previousBindings.value(profile.id).toString() == scope)) {
            bindings.insert(profile.id, scope);
        }
#else
        if (!profile.apiKey.isEmpty() && error) {
            *error = QStringLiteral("No wallet is available; API keys will only be kept until Okular exits");
        }
#endif
    }
#if HAVE_KWALLET
    if (wallet) {
        for (auto it = previous.cbegin(); it != previous.cend(); ++it) {
            if (!retained.contains(it.key()) && wallet->hasEntry(it.key()) && wallet->removeEntry(it.key()) != 0) {
                if (error) {
                    *error = QStringLiteral("Failed to remove an unused API key");
                }
                delete wallet;
                return false;
            }
        }
    }
    delete wallet;
#endif
    KConfigGroup mutableGroup(config, QStringLiteral("AI Reading Assistant"));
    mutableGroup.writeEntry("Profiles", QJsonDocument(list).toJson(QJsonDocument::Compact));
#if HAVE_KWALLET
    mutableGroup.writeEntry("CredentialBindings", QJsonDocument(bindings).toJson(QJsonDocument::Compact));
#endif
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

AiBookSettings AiStore::loadBookSettings(const QString &documentKey)
{
    const QString path = bookSettingsPath(documentKey);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return {};
    }
    return {object.value(QStringLiteral("selectedProfileId")).toString(), object.value(QStringLiteral("defaultPrompt")).toString()};
}

bool AiStore::isManagedBook(const QString &documentKey)
{
    return !bookSettingsPath(documentKey).isEmpty();
}

bool AiStore::saveBookSettings(const QString &documentKey, const AiBookSettings &settings)
{
    const QString path = bookSettingsPath(documentKey);
    if (path.isEmpty()) {
        return false;
    }
    const KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("Cloud Book Library"));
    QLockFile lock(QDir(group.readEntry("ManagedDirectory", QString())).filePath(QStringLiteral(".sync.lock")));
    if (!lock.tryLock(0)) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QByteArray bytes =
        QJsonDocument(QJsonObject {{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("selectedProfileId"), settings.selectedProfileId}, {QStringLiteral("defaultPrompt"), settings.defaultPrompt}}).toJson(QJsonDocument::Compact);
    return file.write(bytes) == bytes.size() && file.commit();
}

AiConversation AiStore::loadConversation(const QString &documentKey, const QString &profileId)
{
    AiConversation conversation;
    conversation.instructions = loadBookSettings(documentKey).defaultPrompt;
    if (documentKey.isEmpty() || profileId.isEmpty()) {
        return conversation;
    }
    QFile file(conversationPath(documentKey, profileId));
    if (!file.open(QIODevice::ReadOnly)) {
        return conversation;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    conversation.sessionId = root.value(QStringLiteral("sessionId")).toString();
    conversation.instructions = root.value(QStringLiteral("instructions")).toString(conversation.instructions);
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
    root.insert(QStringLiteral("instructions"), conversation.instructions);
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
