/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "chatgpttoken.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <memory>

#if HAVE_CHATGPT_AUTH
#include <openssl/evp.h>
#include <openssl/x509.h>

namespace
{
QByteArray decode(const QByteArray &value)
{
    return QByteArray::fromBase64(value, QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
}

QByteArray der(unsigned char tag, const QByteArray &value)
{
    QByteArray result(1, char(tag));
    if (value.size() < 128) {
        result.append(char(value.size()));
    } else {
        QByteArray length;
        for (qsizetype size = value.size(); size; size >>= 8) {
            length.prepend(char(size & 255));
        }
        result.append(char(0x80 | length.size()));
        result.append(length);
    }
    return result + value;
}

QByteArray integer(QByteArray value)
{
    if (!value.isEmpty() && (static_cast<unsigned char>(value[0]) & 0x80)) {
        value.prepend(char(0));
    }
    return der(2, value);
}

bool verify(const QByteArray &signedData, const QByteArray &signature, const QJsonObject &key)
{
    const QByteArray modulus = decode(key.value(QStringLiteral("n")).toString().toLatin1());
    const QByteArray exponent = decode(key.value(QStringLiteral("e")).toString().toLatin1());
    if (modulus.size() < 256 || modulus.size() > 1024 || exponent.isEmpty() || exponent.size() > 8) {
        return false;
    }
    const QByteArray rsa = der(0x30, integer(modulus) + integer(exponent));
    const QByteArray algorithm = QByteArray::fromHex("300d06092a864886f70d0101010500");
    const QByteArray encoded = der(0x30, algorithm + der(3, QByteArray(1, char(0)) + rsa));
    const auto *cursor = reinterpret_cast<const unsigned char *>(encoded.constData());
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> publicKey(d2i_PUBKEY(nullptr, &cursor, encoded.size()), EVP_PKEY_free);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return publicKey && context && EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, publicKey.get()) == 1 &&
        EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char *>(signature.constData()), signature.size(), reinterpret_cast<const unsigned char *>(signedData.constData()), signedData.size()) == 1;
}
}
#endif

QJsonObject ChatGptToken::validate(const QString &token, const QJsonObject &jwks, const QString &issuer, const QString &clientId, const QString &nonce, const QString &subject)
{
#if HAVE_CHATGPT_AUTH
    const QList<QByteArray> parts = token.toLatin1().split('.');
    if (parts.size() != 3 || token.size() > 65536) {
        return {};
    }
    const QJsonObject header = QJsonDocument::fromJson(decode(parts[0])).object();
    if (header.value(QStringLiteral("alg")) != QLatin1String("RS256") || header.value(QStringLiteral("kid")).toString().isEmpty() || header.contains(QStringLiteral("crit"))) {
        return {};
    }
    bool verified = false;
    for (const QJsonValue &value : jwks.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject key = value.toObject();
        if (key.value(QStringLiteral("kid")) == header.value(QStringLiteral("kid")) && key.value(QStringLiteral("kty")) == QLatin1String("RSA") &&
            (!key.contains(QStringLiteral("alg")) || key.value(QStringLiteral("alg")) == QLatin1String("RS256")) && (!key.contains(QStringLiteral("use")) || key.value(QStringLiteral("use")) == QLatin1String("sig"))) {
            verified = verify(parts[0] + '.' + parts[1], decode(parts[2]), key);
            break;
        }
    }
    if (!verified) {
        return {};
    }
    const QJsonObject claims = QJsonDocument::fromJson(decode(parts[1])).object();
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QJsonValue audience = claims.value(QStringLiteral("aud"));
    const bool validAudience = audience.isString() ? audience.toString() == clientId : audience.toArray().contains(clientId);
    if (claims.value(QStringLiteral("iss")) != issuer || !validAudience || claims.value(QStringLiteral("exp")).toDouble() <= now || (claims.contains(QStringLiteral("nbf")) && claims.value(QStringLiteral("nbf")).toDouble() > now) ||
        (audience.isArray() && audience.toArray().size() > 1 && claims.value(QStringLiteral("azp")) != clientId) || (claims.contains(QStringLiteral("azp")) && claims.value(QStringLiteral("azp")) != clientId) ||
        (!nonce.isEmpty() && claims.value(QStringLiteral("nonce")) != nonce) || claims.value(QStringLiteral("sub")).toString().isEmpty() || (!subject.isEmpty() && claims.value(QStringLiteral("sub")) != subject)) {
        return {};
    }
    return claims;
#else
    Q_UNUSED(token)
    Q_UNUSED(jwks)
    Q_UNUSED(issuer)
    Q_UNUSED(clientId)
    Q_UNUSED(nonce)
    Q_UNUSED(subject)
    return {};
#endif
}
