/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "chatgptconnection.h"
#include "chatgpttoken.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QUuid>
#include <cstring>
#include <utility>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#endif

namespace
{
const QString planScope = QStringLiteral("chatgpt.tokens.use.direct");
const QString dynamicClient = QStringLiteral("dynamic_agent_client");

QString randomString()
{
    QByteArray bytes(32, Qt::Uninitialized);
    for (int i = 0; i < bytes.size(); i += 4) {
        const quint32 value = QRandomGenerator::system()->generate();
        memcpy(bytes.data() + i, &value, sizeof(value));
    }
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray form(const QList<QPair<QString, QString>> &fields)
{
    QByteArray result;
    for (const auto &field : fields) {
        if (!result.isEmpty()) {
            result += '&';
        }
        result += QUrl::toPercentEncoding(field.first) + '=' + QUrl::toPercentEncoding(field.second);
    }
    return result;
}
}

ChatGptConnection::ChatGptConnection(QObject *parent)
    : ChatGptConnection(QUrl(QStringLiteral("https://auth.openai.com")),
                        QUrl(QStringLiteral("https://api.openai.com/v1")),
                        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ai-chatgpt/credentials.json"),
                        parent)
{
}

ChatGptConnection::ChatGptConnection(const QUrl &issuer, const QUrl &apiUrl, const QString &storagePath, QObject *parent)
    : QObject(parent)
    , m_issuer(issuer)
    , m_apiUrl(apiUrl)
    , m_storagePath(storagePath)
{
    QFile file(m_storagePath);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject saved = QJsonDocument::fromJson(file.read(1024 * 1024)).object();
        m_hostId = saved.value(QStringLiteral("hostId")).toString();
        for (const QJsonValue &value : saved.value(QStringLiteral("accounts")).toArray()) {
            const QJsonObject record = value.toObject();
            if (record.value(QStringLiteral("issuer")) == m_issuer.toString() && !record.value(QStringLiteral("client_id")).toString().isEmpty() && !record.value(QStringLiteral("subject")).toString().isEmpty()) {
                m_accounts.append(record);
            }
        }
    }
    if (m_hostId.isEmpty()) {
        m_hostId = QStringLiteral("urn:uuid:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    m_loginTimer.setSingleShot(true);
    m_loginTimer.setInterval(600000);
    connect(&m_loginTimer, &QTimer::timeout, this, [this] {
        cancel();
        m_status = tr("ChatGPT sign-in timed out. Please try again.");
        Q_EMIT statusChanged();
    });
    connect(&m_callbackServer, &QTcpServer::newConnection, this, &ChatGptConnection::acceptCallback);
}

ChatGptConnection::~ChatGptConnection()
{
    cancel();
}

bool ChatGptConnection::available() const
{
    return HAVE_CHATGPT_AUTH;
}

bool ChatGptConnection::busy() const
{
    return m_operation != Operation::Idle;
}

QString ChatGptConnection::status() const
{
    return m_status;
}

QVariantList ChatGptConnection::accounts() const
{
    QVariantList result;
    for (const QJsonObject &record : m_accounts) {
        const QString id = record.value(QStringLiteral("client_id")).toString();
        const QString email = record.value(QStringLiteral("email")).toString();
        const QString label = (email.isEmpty() ? tr("ChatGPT account") : email) + QStringLiteral(" · ") + id.right(6);
        result.append(QVariantMap {{QStringLiteral("id"), id}, {QStringLiteral("label"), label}, {QStringLiteral("connected"), isConnected(id)}});
    }
    return result;
}

QVariantList ChatGptConnection::models() const
{
    return m_models;
}

QString ChatGptConnection::modelAccountId() const
{
    return m_modelAccountId;
}

QJsonObject ChatGptConnection::account(const QString &id) const
{
    for (const QJsonObject &record : m_accounts) {
        if (record.value(QStringLiteral("client_id")) == id) {
            return record;
        }
    }
    return {};
}

bool ChatGptConnection::isConnected(const QString &id) const
{
    const QJsonObject record = account(id);
    return !record.value(QStringLiteral("access_token")).toString().isEmpty() && !record.value(QStringLiteral("refresh_token")).toString().isEmpty();
}

bool ChatGptConnection::save()
{
    const QString directory = QFileInfo(m_storagePath).absolutePath();
    if (!QDir().mkpath(directory) || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        return false;
    }
    QJsonArray records;
    for (const QJsonObject &record : std::as_const(m_accounts)) {
        records.append(record);
    }
    const QByteArray data = QJsonDocument(QJsonObject {{QStringLiteral("hostId"), m_hostId}, {QStringLiteral("accounts"), records}}).toJson(QJsonDocument::Compact);
    QSaveFile file(m_storagePath);
    return file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && file.write(data) == data.size() && file.commit();
}

void ChatGptConnection::replaceAccount(const QJsonObject &record)
{
    const QString id = record.value(QStringLiteral("client_id")).toString();
    for (QJsonObject &existing : m_accounts) {
        if (existing.value(QStringLiteral("client_id")) == id) {
            existing = record;
            return;
        }
    }
    m_accounts.append(record);
}

void ChatGptConnection::clearCredentials(const QString &id)
{
    QJsonObject record = account(id);
    for (const QString &key : {QStringLiteral("access_token"), QStringLiteral("refresh_token"), QStringLiteral("id_token"), QStringLiteral("expires_at"), QStringLiteral("scope")}) {
        record.remove(key);
    }
    replaceAccount(record);
    if (m_modelAccountId == id) {
        m_models.clear();
        Q_EMIT modelsChanged();
    }
    Q_EMIT accountsChanged();
}

bool ChatGptConnection::begin(Operation operation, const QString &accountId)
{
    if (busy()) {
        return false;
    }
    if (!available()) {
        m_status = tr("ChatGPT sign-in requires a build with OpenSSL support.");
        Q_EMIT statusChanged();
        return false;
    }
    ++m_generation;
    m_operation = operation;
    m_pendingAccountId = accountId;
    m_status.clear();
    Q_EMIT statusChanged();
    Q_EMIT busyChanged();
    return true;
}

void ChatGptConnection::finish(const QString &error)
{
    const Operation operation = m_operation;
    const auto accessFailed = std::move(m_accessFailed);
    m_accessReady = {};
    m_accessFailed = {};
    QString message = error;
    if (operation == Operation::SignOut) {
        clearCredentials(m_pendingAccountId);
        if (!save()) {
            message = tr("Could not remove the saved ChatGPT credentials.");
        } else if (!error.isEmpty()) {
            message = tr("Signed out locally. Remote revocation was not confirmed; disconnect Okular in ChatGPT Settings.");
        }
    }
    m_operation = Operation::Idle;
    m_pendingAccountId.clear();
    m_state.clear();
    m_nonce.clear();
    m_verifier.clear();
    m_callbackServer.close();
    m_loginTimer.stop();
#ifdef Q_OS_ANDROID
    if (m_signInServiceRunning) {
        QJniObject::callStaticMethod<void>("org/kde/something/ChatGptSignInService", "stop", "(Landroid/content/Context;)V", QNativeInterface::QAndroidApplication::context().object<jobject>());
        m_signInServiceRunning = false;
    }
#endif
    m_status = message;
    Q_EMIT statusChanged();
    Q_EMIT busyChanged();
    if (operation == Operation::Access && !error.isEmpty() && accessFailed) {
        accessFailed(message);
    }
}

bool ChatGptConnection::trustedEndpoint(const QUrl &url) const
{
    return url.isValid() && url.scheme() == m_issuer.scheme() && url.host() == m_issuer.host() && url.port() == m_issuer.port() && url.userInfo().isEmpty() && url.fragment().isEmpty();
}

void ChatGptConnection::requestJson(const QUrl &url, const QByteArray &body, bool post, const QByteArray &bearer, const JsonCallback &callback)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(45000);
    if (post) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    }
    if (!bearer.isEmpty()) {
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearer);
    }
    auto *reply = post ? m_network.post(request, body) : m_network.get(request);
    m_reply = reply;
    const int generation = m_generation;
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > 1024 * 1024) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, callback] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (generation != m_generation) {
            return;
        }
        m_reply = nullptr;
        const QJsonDocument json = QJsonDocument::fromJson(body);
        if (networkError != QNetworkReply::NoError || status != 200 || (!body.isEmpty() && !json.isObject())) {
            const QString code = json.object().value(QStringLiteral("error")).toString();
            if (code == QLatin1String("invalid_grant") && !m_pendingAccountId.isEmpty()) {
                clearCredentials(m_pendingAccountId);
                save();
            }
            finish(code == QLatin1String("invalid_grant") ? tr("ChatGPT authorization expired or was revoked. Sign in again.") : tr("ChatGPT connection failed (HTTP %1). Please retry.").arg(status));
            return;
        }
        callback(json.object());
    });
}

void ChatGptConnection::discover(const std::function<void()> &callback)
{
    if (!m_discovery.isEmpty()) {
        callback();
        return;
    }
    requestJson(m_issuer.resolved(QUrl(QStringLiteral("/.well-known/openid-configuration"))), {}, false, {}, [this, callback](const QJsonObject &json) {
        if (json.value(QStringLiteral("issuer")) != m_issuer.toString()) {
            finish(tr("ChatGPT returned an invalid identity provider."));
            return;
        }
        for (const QString &key : {QStringLiteral("authorization_endpoint"), QStringLiteral("token_endpoint"), QStringLiteral("jwks_uri"), QStringLiteral("revocation_endpoint")}) {
            if (!trustedEndpoint(QUrl(json.value(key).toString()))) {
                finish(tr("ChatGPT returned an untrusted authorization endpoint."));
                return;
            }
        }
        m_discovery = json;
        callback();
    });
}

void ChatGptConnection::signIn(const QString &accountId)
{
    if (!begin(Operation::SignIn, accountId)) {
        return;
    }
    if ((!accountId.isEmpty() && account(accountId).isEmpty()) || !save() || !m_callbackServer.listen(QHostAddress::LocalHost)) {
        finish(tr("Could not prepare ChatGPT sign-in on this device."));
        return;
    }
#ifdef Q_OS_ANDROID
    // Start while the activity is still in front. A loopback listener alone
    // does not prevent Android from freezing the app during browser sign-in.
    m_signInServiceRunning = QJniObject::callStaticMethod<jboolean>("org/kde/something/ChatGptSignInService", "start", "(Landroid/content/Context;)Z", QNativeInterface::QAndroidApplication::context().object<jobject>());
    if (!m_signInServiceRunning) {
        finish(tr("Could not keep ChatGPT sign-in running while the browser is open. Please try again."));
        return;
    }
#endif
    m_callbackUrl = QUrl(QStringLiteral("http://127.0.0.1:%1/auth/callback").arg(m_callbackServer.serverPort()));
    m_state = randomString();
    m_nonce = randomString();
    m_verifier = randomString();
    m_loginTimer.start();
    discover([this, accountId] {
        QUrl url(m_discovery.value(QStringLiteral("authorization_endpoint")).toString());
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("client_id"), accountId.isEmpty() ? dynamicClient : accountId);
        if (accountId.isEmpty()) {
            query.addQueryItem(QStringLiteral("agent_name_hint"), QStringLiteral("Okular"));
        }
        query.addQueryItem(QStringLiteral("ext_agent_host_id"), m_hostId);
        query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
        query.addQueryItem(QStringLiteral("redirect_uri"), m_callbackUrl.toString());
        query.addQueryItem(QStringLiteral("scope"), QStringLiteral("openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"));
        query.addQueryItem(QStringLiteral("resource"), m_apiUrl.toString());
        query.addQueryItem(QStringLiteral("state"), m_state);
        query.addQueryItem(QStringLiteral("nonce"), m_nonce);
        query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
        query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)));
        // Keep authorization URLs out of logs and QML. A retained ID token is
        // intentionally omitted so the browser shows which account is selected.
        url.setQuery(query);
        m_status = tr("Complete sign-in and allow ChatGPT plan usage in your browser, then return to Okular.");
        Q_EMIT statusChanged();
        Q_EMIT authorizationRequested(url);
    });
}

void ChatGptConnection::acceptCallback()
{
    while (m_callbackServer.hasPendingConnections()) {
        auto *socket = m_callbackServer.nextPendingConnection();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(5000, socket, [socket] { socket->disconnectFromHost(); });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, data = QByteArray()]() mutable {
            data += socket->readAll();
            if (data.size() > 16384) {
                socket->disconnectFromHost();
                return;
            }
            if (!data.contains("\r\n\r\n")) {
                return;
            }
            disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
            const QList<QByteArray> line = data.left(data.indexOf("\r\n")).split(' ');
            const QByteArray address = QByteArrayLiteral("http://127.0.0.1") + line.value(1);
            const QUrl url = QUrl::fromEncoded(address);
            const QUrlQuery query(url);
            bool valid = m_operation == Operation::SignIn && m_callbackServer.isListening() && line.size() == 3 && line.value(0) == "GET" && line.value(1).startsWith('/') && url.host() == QLatin1String("127.0.0.1") &&
                url.fragment().isEmpty() && url.path() == m_callbackUrl.path() && query.allQueryItemValues(QStringLiteral("state")).size() == 1 && query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded) == m_state;
            for (const QString &key : {QStringLiteral("code"), QStringLiteral("client_id"), QStringLiteral("error")}) {
                valid = valid && query.allQueryItemValues(key).size() <= 1;
            }
            const QByteArray body = valid ? QByteArrayLiteral("Sign-in received. You can return to Okular.") : QByteArrayLiteral("Invalid sign-in callback.");
            const QByteArray response = (valid ? QByteArrayLiteral("HTTP/1.1 200 OK\r\n") : QByteArrayLiteral("HTTP/1.1 400 Bad Request\r\n")) +
                "Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
            socket->write(response);
            socket->disconnectFromHost();
            if (!valid) {
                return;
            }
            m_callbackServer.close();
            m_loginTimer.stop();
            if (query.hasQueryItem(QStringLiteral("error"))) {
                finish(tr("ChatGPT sign-in or plan usage permission was declined."));
                return;
            }
            QString clientId = query.queryItemValue(QStringLiteral("client_id"), QUrl::FullyDecoded);
            if (clientId.isEmpty()) {
                clientId = m_pendingAccountId;
            }
            const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
            if (code.isEmpty() || clientId.isEmpty() || clientId == dynamicClient || (!m_pendingAccountId.isEmpty() && clientId != m_pendingAccountId)) {
                finish(tr("ChatGPT returned an invalid registration."));
                return;
            }
            exchangeCode(code, clientId);
        });
    }
}

void ChatGptConnection::exchangeCode(const QString &code, const QString &clientId)
{
    const QByteArray body = form({{QStringLiteral("grant_type"), QStringLiteral("authorization_code")},
                                  {QStringLiteral("client_id"), clientId},
                                  {QStringLiteral("code"), code},
                                  {QStringLiteral("code_verifier"), m_verifier},
                                  {QStringLiteral("redirect_uri"), m_callbackUrl.toString()},
                                  {QStringLiteral("resource"), m_apiUrl.toString()}});
    requestJson(QUrl(m_discovery.value(QStringLiteral("token_endpoint")).toString()), body, true, {}, [this, clientId](const QJsonObject &tokens) {
        validateTokens(tokens, clientId, false, [this, clientId] {
            finish();
            loadModels(clientId);
        });
    });
}

void ChatGptConnection::validateTokens(const QJsonObject &tokens, const QString &clientId, bool refreshing, const std::function<void()> &callback)
{
    const QStringList scopes = tokens.value(QStringLiteral("scope")).toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (!scopes.contains(planScope) || !scopes.contains(QStringLiteral("resource.invoke")) || !scopes.contains(QStringLiteral("offline_access")) || tokens.value(QStringLiteral("token_type")) != QLatin1String("Bearer") ||
        tokens.value(QStringLiteral("access_token")).toString().isEmpty() || tokens.value(QStringLiteral("refresh_token")).toString().isEmpty() || tokens.value(QStringLiteral("expires_in")).toInt() <= 0) {
        finish(tr("ChatGPT plan usage was not authorized. Sign in and allow use of your plan."));
        return;
    }
    const QJsonObject previous = account(clientId);
    auto store = [this, tokens, clientId, previous, callback](const QJsonObject &claims) {
        QJsonObject record = previous;
        record.insert(QStringLiteral("client_id"), clientId);
        record.insert(QStringLiteral("issuer"), m_issuer.toString());
        if (!claims.isEmpty()) {
            record.insert(QStringLiteral("subject"), claims.value(QStringLiteral("sub")));
            record.insert(QStringLiteral("email"), claims.value(QStringLiteral("email")));
            record.insert(QStringLiteral("id_token"), tokens.value(QStringLiteral("id_token")));
        }
        for (const QString &key : {QStringLiteral("access_token"), QStringLiteral("refresh_token"), QStringLiteral("scope")}) {
            record.insert(key, tokens.value(key));
        }
        record.insert(QStringLiteral("expires_at"), double(QDateTime::currentSecsSinceEpoch() + tokens.value(QStringLiteral("expires_in")).toInt()));
        replaceAccount(record);
        if (!save()) {
            finish(tr("Could not securely save ChatGPT credentials."));
            return;
        }
        Q_EMIT accountsChanged();
        if (previous.isEmpty()) {
            Q_EMIT firstPlanUse();
        }
        callback();
    };
    const QString idToken = tokens.value(QStringLiteral("id_token")).toString();
    if (refreshing && idToken.isEmpty() && !previous.isEmpty()) {
        store({});
        return;
    }
    requestJson(QUrl(m_discovery.value(QStringLiteral("jwks_uri")).toString()), {}, false, {}, [this, idToken, clientId, refreshing, previous, store](const QJsonObject &jwks) {
        const QJsonObject claims = ChatGptToken::validate(idToken, jwks, m_issuer.toString(), clientId, refreshing ? QString() : m_nonce, previous.value(QStringLiteral("subject")).toString());
        if (claims.isEmpty()) {
            finish(tr("Could not verify the ChatGPT account identity. Please sign in again."));
            return;
        }
        store(claims);
    });
}

void ChatGptConnection::ensureAccessToken(const QString &id, const std::function<void(const QString &)> &callback)
{
    const QJsonObject record = account(id);
    if (!isConnected(id)) {
        finish(tr("Sign in to this ChatGPT account first."));
        return;
    }
    if (record.value(QStringLiteral("expires_at")).toDouble() > QDateTime::currentSecsSinceEpoch() + 60) {
        callback(record.value(QStringLiteral("access_token")).toString());
        return;
    }
    discover([this, id, record, callback] {
        const QByteArray body = form({{QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
                                      {QStringLiteral("client_id"), id},
                                      {QStringLiteral("refresh_token"), record.value(QStringLiteral("refresh_token")).toString()},
                                      {QStringLiteral("resource"), m_apiUrl.toString()}});
        requestJson(QUrl(m_discovery.value(QStringLiteral("token_endpoint")).toString()), body, true, {}, [this, id, callback](const QJsonObject &tokens) {
            validateTokens(tokens, id, true, [this, id, callback] { callback(account(id).value(QStringLiteral("access_token")).toString()); });
        });
    });
}

void ChatGptConnection::requestAccessToken(const QString &id, const std::function<void(const QString &)> &onReady, const std::function<void(const QString &)> &onFailed)
{
    if (busy()) {
        onFailed(tr("Another ChatGPT operation is still running."));
        return;
    }
    m_accessReady = onReady;
    m_accessFailed = onFailed;
    if (!begin(Operation::Access, id)) {
        m_accessReady = {};
        m_accessFailed = {};
        onFailed(m_status);
        return;
    }
    ensureAccessToken(id, [this](const QString &token) {
        const auto ready = std::move(m_accessReady);
        finish();
        if (ready) {
            ready(token);
        }
    });
}

void ChatGptConnection::loadModels(const QString &id)
{
    if (!begin(Operation::Models, id)) {
        return;
    }
    m_models.clear();
    m_modelAccountId = id;
    Q_EMIT modelsChanged();
    ensureAccessToken(id, [this](const QString &token) {
        requestJson(QUrl(m_apiUrl.toString() + QStringLiteral("/models")), {}, false, token.toUtf8(), [this](const QJsonObject &json) {
            for (const QJsonValue &value : json.value(QStringLiteral("models")).toArray()) {
                const QJsonObject model = value.toObject();
                const QString slug = model.value(QStringLiteral("slug")).toString();
                if (model.value(QStringLiteral("visibility")) == QLatin1String("list") && !slug.isEmpty()) {
                    m_models.append(QVariantMap {{QStringLiteral("id"), slug}, {QStringLiteral("name"), model.value(QStringLiteral("display_name")).toString(slug)}});
                }
            }
            Q_EMIT modelsChanged();
            finish(m_models.isEmpty() ? tr("No models are available for this ChatGPT account.") : tr("ChatGPT connected. Choose a model to use your plan."));
        });
    });
}

void ChatGptConnection::signOut(const QString &id)
{
    if (account(id).isEmpty() || !begin(Operation::SignOut, id)) {
        return;
    }
    const QString refreshToken = account(id).value(QStringLiteral("refresh_token")).toString();
    if (refreshToken.isEmpty()) {
        finish();
        return;
    }
    discover([this, id, refreshToken] {
        const QByteArray body = form({{QStringLiteral("token"), refreshToken}, {QStringLiteral("token_type_hint"), QStringLiteral("refresh_token")}, {QStringLiteral("client_id"), id}});
        requestJson(QUrl(m_discovery.value(QStringLiteral("revocation_endpoint")).toString()), body, true, {}, [this](const QJsonObject &) { finish(); });
    });
}

void ChatGptConnection::cancel()
{
    if (!busy()) {
        return;
    }
    ++m_generation;
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
    finish(tr("ChatGPT operation canceled."));
}

#include "moc_chatgptconnection.cpp"
