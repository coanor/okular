/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../mobile/components/chatgptconnection.h"
#include "../mobile/components/chatgpttoken.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include <memory>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

namespace
{
QByteArray base64(const QByteArray &value)
{
    return value.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray number(const BIGNUM *value)
{
    QByteArray bytes(BN_num_bytes(value), Qt::Uninitialized);
    BN_bn2bin(value, reinterpret_cast<unsigned char *>(bytes.data()));
    return base64(bytes);
}
}

class ChatGptServer : public QTcpServer
{
    Q_OBJECT
public:
    ChatGptServer()
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY *key = nullptr;
        if (!context || EVP_PKEY_keygen_init(context.get()) != 1 || EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1 || EVP_PKEY_keygen(context.get(), &key) != 1 || !listen(QHostAddress::LocalHost)) {
            qFatal("Could not prepare the ChatGPT OAuth fixture");
        }
        m_key.reset(key);
        QByteArray modulus;
        QByteArray exponent;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        BIGNUM *n = nullptr;
        BIGNUM *e = nullptr;
        EVP_PKEY_get_bn_param(key, "n", &n);
        EVP_PKEY_get_bn_param(key, "e", &e);
        modulus = number(n);
        exponent = number(e);
        BN_free(n);
        BN_free(e);
#else
        const BIGNUM *n = nullptr;
        const BIGNUM *e = nullptr;
        RSA_get0_key(EVP_PKEY_get0_RSA(key), &n, &e, nullptr);
        modulus = number(n);
        exponent = number(e);
#endif
        jwks = QJsonObject {{QStringLiteral("keys"),
                             QJsonArray {QJsonObject {{QStringLiteral("kty"), QStringLiteral("RSA")},
                                                      {QStringLiteral("kid"), QStringLiteral("fixture")},
                                                      {QStringLiteral("alg"), QStringLiteral("RS256")},
                                                      {QStringLiteral("n"), QString::fromLatin1(modulus)},
                                                      {QStringLiteral("e"), QString::fromLatin1(exponent)}}}}};
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket, data = QByteArray(), handled = false]() mutable {
                if (handled) {
                    return;
                }
                data += socket->readAll();
                const qsizetype end = data.indexOf("\r\n\r\n");
                if (end < 0) {
                    return;
                }
                qsizetype length = 0;
                for (const QByteArray &line : data.left(end).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        length = line.mid(15).trimmed().toLongLong();
                    }
                }
                if (data.size() < end + 4 + length) {
                    return;
                }
                handled = true;
                const QByteArray path = data.left(end).split(' ').value(1);
                QJsonObject result;
                int status = 200;
                if (path == "/.well-known/openid-configuration") {
                    const QString authorizationUrl = issuer() + QStringLiteral("/authorize");
                    const QString tokenUrl = issuer() + QStringLiteral("/token");
                    const QString jwksUrl = issuer() + QStringLiteral("/jwks");
                    const QString revocationUrl = issuer() + QStringLiteral("/revoke");
                    result = {{QStringLiteral("issuer"), issuer()},
                              {QStringLiteral("authorization_endpoint"), authorizationUrl},
                              {QStringLiteral("token_endpoint"), tokenUrl},
                              {QStringLiteral("jwks_uri"), jwksUrl},
                              {QStringLiteral("revocation_endpoint"), revocationUrl}};
                    if (untrustedDiscovery) {
                        result.insert(QStringLiteral("token_endpoint"), QStringLiteral("https://untrusted.invalid/token"));
                    }
                } else if (path == "/jwks") {
                    result = jwks;
                } else if (path == "/token") {
                    lastTokenForm = QUrlQuery(QString::fromUtf8(data.mid(end + 4, length)));
                    ++tokenRequests;
                    const bool refreshing = lastTokenForm.queryItemValue(QStringLiteral("grant_type")) == QLatin1String("refresh_token");
                    if (!refreshing) {
                        authorizationCodeForm = lastTokenForm;
                    }
                    if (rejectRefresh && refreshing) {
                        status = 400;
                        result.insert(QStringLiteral("error"), QStringLiteral("invalid_grant"));
                    } else {
                        result = {{QStringLiteral("token_type"), QStringLiteral("Bearer")},
                                  {QStringLiteral("access_token"), QStringLiteral("test-access-%1").arg(tokenRequests)},
                                  {QStringLiteral("refresh_token"), QStringLiteral("test-refresh-%1").arg(tokenRequests)},
                                  {QStringLiteral("expires_in"), expiresIn},
                                  {QStringLiteral("scope"), missingScope ? QStringLiteral("openid email") : QStringLiteral("offline_access resource.invoke chatgpt.tokens.use.direct")}};
                        if (!refreshing) {
                            QJsonObject identity = claims(lastTokenForm.queryItemValue(QStringLiteral("client_id")), authorizeQuery.queryItemValue(QStringLiteral("nonce")));
                            if (badIdentity) {
                                identity.insert(QStringLiteral("nonce"), QStringLiteral("wrong-nonce"));
                            }
                            result.insert(QStringLiteral("id_token"), sign(identity));
                        }
                    }
                } else if (path == "/v1/models") {
                    ++modelRequests;
                    modelHeaders = data.left(end);
                    result.insert(
                        QStringLiteral("models"),
                        QJsonArray {QJsonObject {{QStringLiteral("visibility"), QStringLiteral("list")}, {QStringLiteral("slug"), QStringLiteral("account-model")}, {QStringLiteral("display_name"), QStringLiteral("Account model")}},
                                    QJsonObject {{QStringLiteral("visibility"), QStringLiteral("hidden")}, {QStringLiteral("slug"), QStringLiteral("hidden-model")}}});
                } else if (path == "/revoke") {
                    lastRevocationForm = QUrlQuery(QString::fromUtf8(data.mid(end + 4, length)));
                    ++revocations;
                    status = failRevocation ? 500 : 200;
                } else {
                    status = 404;
                }
                const QByteArray body = path == "/revoke" ? QByteArray() : QJsonDocument(result).toJson(QJsonDocument::Compact);
                socket->write("HTTP/1.1 " + QByteArray::number(status) + " Result\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
    }

    QString issuer() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }
    QUrl apiUrl() const
    {
        return QUrl(issuer() + QStringLiteral("/v1"));
    }

    QJsonObject claims(const QString &clientId = QStringLiteral("oaiapp_test"), const QString &nonce = QStringLiteral("nonce")) const
    {
        return {{QStringLiteral("iss"), issuer()},
                {QStringLiteral("aud"), clientId},
                {QStringLiteral("sub"), subject},
                {QStringLiteral("email"), QStringLiteral("reader@example.test")},
                {QStringLiteral("exp"), double(QDateTime::currentSecsSinceEpoch() + 3600)},
                {QStringLiteral("nonce"), nonce}};
    }

    QString sign(const QJsonObject &claims) const
    {
        const QByteArray header = base64(QByteArrayLiteral("{\"alg\":\"RS256\",\"kid\":\"fixture\"}"));
        const QByteArray payload = header + '.' + base64(QJsonDocument(claims).toJson(QJsonDocument::Compact));
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, m_key.get());
        size_t length = 0;
        EVP_DigestSign(context.get(), nullptr, &length, reinterpret_cast<const unsigned char *>(payload.constData()), payload.size());
        QByteArray signature(length, Qt::Uninitialized);
        EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(signature.data()), &length, reinterpret_cast<const unsigned char *>(payload.constData()), payload.size());
        signature.resize(length);
        return QString::fromLatin1(payload + '.' + base64(signature));
    }

    QUrl callback(const QUrl &authorization, const QString &clientId = QStringLiteral("oaiapp_test"))
    {
        authorizeQuery = QUrlQuery(authorization);
        QUrl url(authorizeQuery.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("state"), authorizeQuery.queryItemValue(QStringLiteral("state")));
        query.addQueryItem(QStringLiteral("code"), QStringLiteral("test-code"));
        query.addQueryItem(QStringLiteral("client_id"), clientId);
        url.setQuery(query);
        return url;
    }

    QJsonObject jwks;
    QUrlQuery authorizeQuery;
    QUrlQuery lastTokenForm;
    QUrlQuery authorizationCodeForm;
    QUrlQuery lastRevocationForm;
    QByteArray modelHeaders;
    int tokenRequests = 0;
    int modelRequests = 0;
    int revocations = 0;
    int expiresIn = 3600;
    QString subject = QStringLiteral("reader");
    bool missingScope = false;
    bool badIdentity = false;
    bool rejectRefresh = false;
    bool untrustedDiscovery = false;
    bool failRevocation = false;

private:
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> m_key {nullptr, EVP_PKEY_free};
};

class ChatGptConnectionTest : public QObject
{
    Q_OBJECT
private:
    QNetworkAccessManager m_browser;

    void visit(const QUrl &url)
    {
        auto *reply = m_browser.get(QNetworkRequest(url));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }

private Q_SLOTS:
    void registrationRefreshAndSignOut()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("credentials.json"));
        ChatGptServer server;
        server.expiresIn = 1;
        ChatGptConnection connection(QUrl(server.issuer()), server.apiUrl(), path);
        QSignalSpy authorize(&connection, &ChatGptConnection::authorizationRequested);
        QSignalSpy welcome(&connection, &ChatGptConnection::firstPlanUse);
        connection.signIn();
        QVERIFY(authorize.wait());
        const QUrl authorization = authorize.takeFirst().at(0).toUrl();
        const QUrl callback = server.callback(authorization);
        QCOMPARE(server.authorizeQuery.queryItemValue(QStringLiteral("client_id")), QStringLiteral("dynamic_agent_client"));
        QCOMPARE(server.authorizeQuery.queryItemValue(QStringLiteral("agent_name_hint")), QStringLiteral("Okular"));
        const QString host = server.authorizeQuery.queryItemValue(QStringLiteral("ext_agent_host_id"));
        QVERIFY(host.startsWith(QLatin1String("urn:uuid:")));
        QCOMPARE(server.authorizeQuery.queryItemValue(QStringLiteral("code_challenge_method")), QStringLiteral("S256"));
        QVERIFY(!server.authorizeQuery.queryItemValue(QStringLiteral("code_challenge")).isEmpty());
        QUrl wrongCallback = callback;
        QUrlQuery wrongQuery(wrongCallback);
        wrongQuery.removeAllQueryItems(QStringLiteral("state"));
        wrongQuery.addQueryItem(QStringLiteral("state"), QStringLiteral("untrusted"));
        wrongCallback.setQuery(wrongQuery);
        visit(wrongCallback);
        QTest::qWait(50);
        QVERIFY(connection.busy());
        QCOMPARE(server.tokenRequests, 0);
        visit(callback);
        QTRY_VERIFY(!connection.busy());
        QCOMPARE(connection.accounts().size(), 1);
        QVERIFY(connection.isConnected(QStringLiteral("oaiapp_test")));
        QCOMPARE(connection.models().size(), 1);
        QCOMPARE(connection.models().first().toMap().value(QStringLiteral("id")).toString(), QStringLiteral("account-model"));
        QCOMPARE(welcome.size(), 1);
        QCOMPARE(server.authorizationCodeForm.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded), QUrlQuery(authorization).queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
        const QByteArray verifier = server.authorizationCodeForm.queryItemValue(QStringLiteral("code_verifier")).toLatin1();
        QCOMPARE(QString::fromLatin1(base64(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256))), server.authorizeQuery.queryItemValue(QStringLiteral("code_challenge")));
        QVERIFY(!connection.accounts().first().toMap().contains(QStringLiteral("access_token")));
        QVERIFY(server.modelHeaders.contains("Authorization: Bearer test-access-2"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        const QJsonObject record = saved.value(QStringLiteral("accounts")).toArray().first().toObject();
        QCOMPARE(record.value(QStringLiteral("refresh_token")).toString(), QStringLiteral("test-refresh-2"));
#ifndef Q_OS_WIN
        QVERIFY(!(file.permissions() & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));
#endif
        server.expiresIn = 3600;
        ChatGptConnection restored(QUrl(server.issuer()), server.apiUrl(), path);
        QString access;
        QString accessError;
        restored.requestAccessToken(QStringLiteral("oaiapp_test"), [&access](const QString &token) { access = token; }, [&accessError](const QString &error) { accessError = error; });
        QTRY_VERIFY(!access.isEmpty());
        QVERIFY(accessError.isEmpty());
        QCOMPARE(access, QStringLiteral("test-access-3"));
        QCOMPARE(restored.metaObject()->indexOfSignal("accessTokenReady(QString,QString)"), -1);
        QCOMPARE(restored.metaObject()->indexOfMethod("requestAccessToken(QString)"), -1);
        QCOMPARE(server.lastTokenForm.queryItemValue(QStringLiteral("grant_type")), QStringLiteral("refresh_token"));
        QCOMPARE(server.lastTokenForm.queryItemValue(QStringLiteral("client_id")), QStringLiteral("oaiapp_test"));
        QCOMPARE(server.lastTokenForm.queryItemValue(QStringLiteral("refresh_token")), QStringLiteral("test-refresh-2"));
        restored.signOut(QStringLiteral("oaiapp_test"));
        QTRY_VERIFY(!restored.busy());
        QVERIFY(!restored.isConnected(QStringLiteral("oaiapp_test")));
        QCOMPARE(server.revocations, 1);
        QCOMPARE(server.lastRevocationForm.queryItemValue(QStringLiteral("token")), QStringLiteral("test-refresh-3"));
        QSignalSpy reauthorize(&restored, &ChatGptConnection::authorizationRequested);
        restored.signIn(QStringLiteral("oaiapp_test"));
        if (reauthorize.isEmpty()) {
            QVERIFY(reauthorize.wait());
        }
        const QUrl secondAuthorization = reauthorize.first().at(0).toUrl();
        QCOMPARE(QUrlQuery(secondAuthorization).queryItemValue(QStringLiteral("client_id")), QStringLiteral("oaiapp_test"));
        QCOMPARE(QUrlQuery(secondAuthorization).queryItemValue(QStringLiteral("ext_agent_host_id")), host);
        QVERIFY(!QUrlQuery(secondAuthorization).hasQueryItem(QStringLiteral("agent_name_hint")));
        restored.cancel();
        QVERIFY(!restored.busy());
    }

    void invalidAuthorization_data()
    {
        QTest::addColumn<bool>("missingScope");
        QTest::addColumn<bool>("badIdentity");
        QTest::newRow("declined-plan-use") << true << false;
        QTest::newRow("invalid-account-identity") << false << true;
    }

    void invalidAuthorization()
    {
        QFETCH(bool, missingScope);
        QFETCH(bool, badIdentity);
        QTemporaryDir directory;
        ChatGptServer server;
        server.missingScope = missingScope;
        server.badIdentity = badIdentity;
        ChatGptConnection connection(QUrl(server.issuer()), server.apiUrl(), directory.filePath(QStringLiteral("credentials.json")));
        QSignalSpy authorize(&connection, &ChatGptConnection::authorizationRequested);
        connection.signIn();
        QVERIFY(authorize.wait());
        visit(server.callback(authorize.first().at(0).toUrl()));
        QTRY_VERIFY(!connection.busy());
        QVERIFY(connection.accounts().isEmpty());
        QVERIFY(!connection.status().isEmpty());
        QCOMPARE(server.modelRequests, 0);
    }

    void refusesUntrustedDiscovery()
    {
        QTemporaryDir directory;
        ChatGptServer server;
        server.untrustedDiscovery = true;
        ChatGptConnection connection(QUrl(server.issuer()), server.apiUrl(), directory.filePath(QStringLiteral("credentials.json")));
        QSignalSpy authorize(&connection, &ChatGptConnection::authorizationRequested);
        connection.signIn();
        QTRY_VERIFY(!connection.busy());
        QVERIFY(authorize.isEmpty());
        QCOMPARE(server.tokenRequests, 0);
    }

    void accountIsolationAndRevokedRefresh()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("credentials.json"));
        ChatGptServer server;
        ChatGptConnection connection(QUrl(server.issuer()), server.apiUrl(), path);
        QSignalSpy authorize(&connection, &ChatGptConnection::authorizationRequested);
        auto signIn = [&](const QString &requestedId, const QString &returnedId) {
            authorize.clear();
            connection.signIn(requestedId);
            QTRY_COMPARE(authorize.size(), 1);
            visit(server.callback(authorize.first().at(0).toUrl(), returnedId));
            QTRY_VERIFY(!connection.busy());
        };
        signIn({}, QStringLiteral("oaiapp_first"));
        server.subject = QStringLiteral("other-reader");
        signIn({}, QStringLiteral("oaiapp_second"));
        QCOMPARE(connection.accounts().size(), 2);
        QVERIFY(connection.accounts().first().toMap().value(QStringLiteral("label")) != connection.accounts().last().toMap().value(QStringLiteral("label")));
        QVERIFY(connection.isConnected(QStringLiteral("oaiapp_first")));
        QVERIFY(connection.isConnected(QStringLiteral("oaiapp_second")));
        // Reauthorization cannot replace an account with a different identity.
        signIn(QStringLiteral("oaiapp_first"), QStringLiteral("oaiapp_first"));
        QVERIFY(connection.status().contains(QStringLiteral("identity")));
        const int requests = server.tokenRequests;
        signIn(QStringLiteral("oaiapp_first"), QStringLiteral("oaiapp_second"));
        QCOMPARE(server.tokenRequests, requests);
        server.failRevocation = true;
        connection.signOut(QStringLiteral("oaiapp_first"));
        QTRY_VERIFY(!connection.busy());
        QVERIFY(!connection.isConnected(QStringLiteral("oaiapp_first")));
        QVERIFY(connection.isConnected(QStringLiteral("oaiapp_second")));
        QVERIFY(connection.status().contains(QStringLiteral("not confirmed")));

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonArray accounts = saved.value(QStringLiteral("accounts")).toArray();
        QJsonObject second = accounts[1].toObject();
        second.insert(QStringLiteral("expires_at"), 0);
        accounts[1] = second;
        saved.insert(QStringLiteral("accounts"), accounts);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(saved).toJson());
        file.close();
        server.rejectRefresh = true;
        ChatGptConnection restored(QUrl(server.issuer()), server.apiUrl(), path);
        QString failed;
        restored.requestAccessToken(QStringLiteral("oaiapp_second"), [](const QString &) { QFAIL("Revoked credentials must not be returned"); }, [&failed](const QString &error) { failed = error; });
        QTRY_VERIFY(!failed.isEmpty());
        QVERIFY(!restored.isConnected(QStringLiteral("oaiapp_second")));
        QVERIFY(!restored.busy());
        QVERIFY(restored.status().contains(QStringLiteral("revoked")));
    }

    void signedTokenValidation()
    {
        ChatGptServer server;
        const QJsonObject original = server.claims();
        auto validate = [&server](const QJsonObject &claims) { return ChatGptToken::validate(server.sign(claims), server.jwks, server.issuer(), QStringLiteral("oaiapp_test"), QStringLiteral("nonce"), QStringLiteral("reader")); };
        QVERIFY(!validate(original).isEmpty());
        for (const QString &field : {QStringLiteral("iss"), QStringLiteral("aud"), QStringLiteral("nonce"), QStringLiteral("sub")}) {
            QJsonObject claims = original;
            claims.insert(field, QStringLiteral("different"));
            QVERIFY(validate(claims).isEmpty());
        }
        QJsonObject expired = original;
        expired.insert(QStringLiteral("exp"), double(QDateTime::currentSecsSinceEpoch() - 1));
        QVERIFY(validate(expired).isEmpty());
        QJsonObject future = original;
        future.insert(QStringLiteral("nbf"), double(QDateTime::currentSecsSinceEpoch() + 3600));
        QVERIFY(validate(future).isEmpty());
        QJsonObject multiple = original;
        multiple.insert(QStringLiteral("aud"), QJsonArray {QStringLiteral("oaiapp_test"), QStringLiteral("other")});
        QVERIFY(validate(multiple).isEmpty());
        multiple.insert(QStringLiteral("azp"), QStringLiteral("oaiapp_test"));
        QVERIFY(!validate(multiple).isEmpty());
        QString tampered = server.sign(original);
        const qsizetype signature = tampered.lastIndexOf(QLatin1Char('.')) + 1;
        tampered[signature] = tampered[signature] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
        QVERIFY(ChatGptToken::validate(tampered, server.jwks, server.issuer(), QStringLiteral("oaiapp_test"), QStringLiteral("nonce"), {}).isEmpty());
    }
};

QTEST_GUILESS_MAIN(ChatGptConnectionTest)
#include "chatgptconnectiontest.moc"
