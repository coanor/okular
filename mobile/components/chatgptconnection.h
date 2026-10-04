/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTcpServer>
#include <QTimer>
#include <QVariantList>
#include <functional>

class QNetworkReply;

// Owns ChatGPT OAuth registrations and credentials. Only account labels and
// model names are exposed to QML; bearer tokens stay in the native provider.
class ChatGptConnection : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(QString modelAccountId READ modelAccountId NOTIFY modelsChanged)

public:
    explicit ChatGptConnection(QObject *parent = nullptr);
    ChatGptConnection(const QUrl &issuer, const QUrl &apiUrl, const QString &storagePath, QObject *parent = nullptr);
    ~ChatGptConnection() override;

    bool available() const;
    bool busy() const;
    QString status() const;
    QVariantList accounts() const;
    QVariantList models() const;
    QString modelAccountId() const;

    Q_INVOKABLE bool isConnected(const QString &accountId) const;
    Q_INVOKABLE void signIn(const QString &accountId = QString());
    Q_INVOKABLE void loadModels(const QString &accountId);
    Q_INVOKABLE void signOut(const QString &accountId);
    Q_INVOKABLE void cancel();

    void requestAccessToken(const QString &accountId, const std::function<void(const QString &)> &onReady, const std::function<void(const QString &)> &onFailed);

Q_SIGNALS:
    void busyChanged();
    void statusChanged();
    void accountsChanged();
    void modelsChanged();
    void firstPlanUse();
    void authorizationRequested(const QUrl &url);

private:
    enum class Operation { Idle, SignIn, Models, Access, SignOut };
    using JsonCallback = std::function<void(const QJsonObject &)>;
    bool begin(Operation operation, const QString &accountId);
    void finish(const QString &error = QString());
    bool save();
    QJsonObject account(const QString &id) const;
    void replaceAccount(const QJsonObject &account);
    void clearCredentials(const QString &id);
    void discover(const std::function<void()> &callback);
    void requestJson(const QUrl &url, const QByteArray &body, bool post, const QByteArray &bearer, const JsonCallback &callback);
    void acceptCallback();
    void exchangeCode(const QString &code, const QString &clientId);
    void validateTokens(const QJsonObject &tokens, const QString &clientId, bool refreshing, const std::function<void()> &callback);
    void ensureAccessToken(const QString &id, const std::function<void(const QString &)> &callback);
    bool trustedEndpoint(const QUrl &url) const;

    QUrl m_issuer;
    QUrl m_apiUrl;
    QString m_storagePath;
    QString m_hostId;
    QList<QJsonObject> m_accounts;
    QVariantList m_models;
    QString m_modelAccountId;
    QString m_status;
    Operation m_operation = Operation::Idle;
    QString m_pendingAccountId;
    QString m_state;
    QString m_nonce;
    QString m_verifier;
    QUrl m_callbackUrl;
    QJsonObject m_discovery;
    int m_generation = 0;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTcpServer m_callbackServer;
    QTimer m_loginTimer;
#ifdef Q_OS_ANDROID
    bool m_signInServiceRunning = false;
#endif
    std::function<void(const QString &)> m_accessReady;
    std::function<void(const QString &)> m_accessFailed;
};
