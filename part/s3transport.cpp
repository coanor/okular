/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "s3transport.h"

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#endif

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <curl/curl.h>

#include <cstring>
#include <utility>

namespace
{
struct Upload {
    const QByteArray *bytes = nullptr;
    QFile *file = nullptr;
    qsizetype offset = 0;
};

struct Download {
    QByteArray *bytes = nullptr;
    QSaveFile *file = nullptr;
    QCryptographicHash *hash = nullptr;
};

size_t receive(char *data, size_t size, size_t count, void *context)
{
    auto *download = static_cast<Download *>(context);
    const size_t received = size * count;
    if (download->file) {
        if (download->file->write(data, static_cast<qint64>(received)) != static_cast<qint64>(received)) {
            return 0;
        }
        download->hash->addData(QByteArrayView(data, static_cast<qsizetype>(received)));
        return received;
    }
    auto *bytes = download->bytes;
    constexpr qsizetype maxMetadataBytes = 16 * 1024 * 1024;
    if (received > static_cast<size_t>(maxMetadataBytes - bytes->size())) {
        return 0;
    }
    bytes->append(data, static_cast<qsizetype>(received));
    return received;
}

size_t sendBody(char *data, size_t size, size_t count, void *context)
{
    auto *upload = static_cast<Upload *>(context);
    if (upload->file) {
        const qint64 amount = upload->file->read(data, static_cast<qint64>(size * count));
        return amount < 0 ? CURL_READFUNC_ABORT : static_cast<size_t>(amount);
    }
    const qsizetype remaining = upload->bytes->size() - upload->offset;
    const qsizetype amount = qMin(static_cast<qsizetype>(size * count), remaining);
    if (amount > 0) {
        memcpy(data, upload->bytes->constData() + upload->offset, static_cast<size_t>(amount));
        upload->offset += amount;
    }
    return static_cast<size_t>(amount);
}

QString elementText(QXmlStreamReader &reader)
{
    return reader.readElementText(QXmlStreamReader::SkipChildElements);
}
}

S3Transport::S3Transport(S3Configuration configuration)
    : m_configuration(std::move(configuration))
{
}

S3Response S3Transport::getObject(const QString &relativeKey) const
{
    return request("GET", relativeKey, nullptr, false);
}

S3Response S3Transport::putObjectIfAbsent(const QString &relativeKey, const QByteArray &body) const
{
    return request("PUT", relativeKey, &body, true);
}

S3Response S3Transport::putFileIfAbsent(const QString &relativeKey, const QString &sourcePath, const QString &expectedSha256) const
{
    return request("PUT", relativeKey, nullptr, true, false, {}, {}, sourcePath, {}, expectedSha256);
}

S3Response S3Transport::downloadFile(const QString &relativeKey, const QString &destinationPath, const QString &expectedSha256) const
{
    return request("GET", relativeKey, nullptr, false, false, {}, {}, {}, destinationPath, expectedSha256);
}

S3Response S3Transport::request(const QByteArray &method,
                                const QString &relativeKey,
                                const QByteArray *body,
                                bool ifAbsent,
                                bool listRequest,
                                const QString &listPrefix,
                                const QString &continuationToken,
                                const QString &uploadPath,
                                const QString &downloadPath,
                                const QString &expectedSha256) const
{
    QFile uploadFile(uploadPath);
    QByteArray uploadDigest;
    qint64 uploadSize = 0;
    if (!uploadPath.isEmpty()) {
        if (!uploadFile.open(QIODevice::ReadOnly)) {
            return {0, {}, QStringLiteral("Could not read the source file for S3 upload")};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&uploadFile) || !uploadFile.seek(0)) {
            return {0, {}, QStringLiteral("Could not hash the source file for S3 upload")};
        }
        uploadDigest = hash.result().toHex();
        if (uploadDigest != expectedSha256.toLatin1()) {
            return {0, {}, QStringLiteral("The source file changed before S3 upload")};
        }
        uploadSize = uploadFile.size();
    } else if (body) {
        uploadDigest = QCryptographicHash::hash(*body, QCryptographicHash::Sha256).toHex();
        uploadSize = body->size();
    }
    QSaveFile downloadFile(downloadPath);
    QCryptographicHash downloadHash(QCryptographicHash::Sha256);
    if (!downloadPath.isEmpty() && !downloadFile.open(QIODevice::WriteOnly)) {
        return {0, {}, QStringLiteral("Could not create the local download file")};
    }

    static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) {
        return {0, {}, QStringLiteral("Could not initialize S3 transport")};
    }
    CURL *handle = curl_easy_init();
    if (!handle) {
        return {0, {}, QStringLiteral("Could not create S3 request")};
    }

    QUrl url = m_configuration.endpoint;
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }
    path += m_configuration.bucket;
    if (!relativeKey.isEmpty()) {
        path += QLatin1Char('/') + m_configuration.objectKey(relativeKey);
    }
    url.setPath(path);
    if (listRequest) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("list-type"), QStringLiteral("2"));
        query.addQueryItem(QStringLiteral("prefix"), m_configuration.objectKey(listPrefix));
        if (!continuationToken.isEmpty()) {
            query.addQueryItem(QStringLiteral("continuation-token"), continuationToken);
        }
        url.setQuery(query);
    }

    const QByteArray urlBytes = url.toString(QUrl::FullyEncoded).toUtf8();
    const QByteArray access = m_configuration.accessKeyId.toUtf8();
    const QByteArray secret = m_configuration.secretAccessKey.toUtf8();
    const QByteArray signing = QStringLiteral("aws:amz:%1:s3").arg(m_configuration.region).toUtf8();
    char curlError[CURL_ERROR_SIZE] = {};
    QByteArray responseBody;
    Upload upload {body, uploadPath.isEmpty() ? nullptr : &uploadFile, 0};
    Download download {&responseBody, downloadPath.isEmpty() ? nullptr : &downloadFile, &downloadHash};
    curl_slist *headers = nullptr;
    const auto appendHeader = [&headers](const QByteArray &header) {
        curl_slist *next = curl_slist_append(headers, header.constData());
        if (!next) {
            return false;
        }
        headers = next;
        return true;
    };
    if (!m_configuration.sessionToken.isEmpty()) {
        const QByteArray tokenHeader = "x-amz-security-token: " + m_configuration.sessionToken.toUtf8();
        if (!appendHeader(tokenHeader)) {
            curl_easy_cleanup(handle);
            return {0, {}, QStringLiteral("Could not prepare S3 request headers")};
        }
    }
    if (body || !uploadPath.isEmpty()) {
        const QByteArray hashHeader = QByteArray("x-amz-content-sha256: ") + uploadDigest;
        if (!appendHeader(hashHeader) || !appendHeader("Expect:") || (ifAbsent && !appendHeader("If-None-Match: *"))) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(handle);
            return {0, {}, QStringLiteral("Could not prepare S3 request headers")};
        }
    }
#ifdef Q_OS_ANDROID
    static const QByteArray certificates = [] {
        const QJniObject pem = QJniObject::callStaticObjectMethod("org/kde/okular/CloudPlatform", "systemCertificates", "()Ljava/lang/String;");
        QJniEnvironment environment;
        environment.checkAndClearExceptions(QJniEnvironment::OutputMode::Silent);
        return pem.toString().toUtf8();
    }();
    if (m_configuration.endpoint.scheme() == QLatin1String("https") && certificates.isEmpty()) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        return {0, {}, QStringLiteral("Could not load Android's trusted certificates")};
    }
    curl_blob certificateBundle {const_cast<char *>(certificates.constData()), static_cast<size_t>(certificates.size()), CURL_BLOB_COPY};
    curl_easy_setopt(handle, CURLOPT_CAINFO_BLOB, &certificateBundle);
#endif
    curl_easy_setopt(handle, CURLOPT_URL, urlBytes.constData());
    curl_easy_setopt(handle, CURLOPT_USERNAME, access.constData());
    curl_easy_setopt(handle, CURLOPT_PASSWORD, secret.constData());
    curl_easy_setopt(handle, CURLOPT_AWS_SIGV4, signing.constData());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, curlError);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &download);
    if (method == "PUT" && (body || !uploadPath.isEmpty())) {
        curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, sendBody);
        curl_easy_setopt(handle, CURLOPT_READDATA, &upload);
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(uploadSize));
    }
    const CURLcode result = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    if (result != CURLE_OK) {
        // curl's error text can include a URL but must never include the
        // access key or secret. Keep the user-facing error generic.
        return {status, {}, QStringLiteral("S3 request failed (%1)").arg(static_cast<int>(result))};
    }
    if (!downloadPath.isEmpty()) {
        if (status < 200 || status >= 300 || downloadHash.result().toHex() != expectedSha256.toLatin1() || !downloadFile.commit()) {
            downloadFile.cancelWriting();
            return {status, {}, QStringLiteral("S3 download failed or did not match the expected SHA-256")};
        }
    }
    return {status, responseBody, {}};
}

bool S3Transport::listObjects(const QString &relativePrefix, QStringList *relativeKeys, QString *error) const
{
    if (!relativeKeys) {
        if (error) {
            *error = QStringLiteral("S3 listing output is missing");
        }
        return false;
    }
    relativeKeys->clear();
    QString continuationToken;
    QSet<QString> seenTokens;
    do {
        const S3Response response = request("GET", {}, nullptr, false, true, relativePrefix, continuationToken);
        if (!response.successful()) {
            if (error) {
                *error = response.error.isEmpty() ? QStringLiteral("S3 listing failed (HTTP %1)").arg(response.status) : response.error;
            }
            return false;
        }
        QXmlStreamReader reader(response.body);
        bool truncated = false;
        bool listingRoot = false;
        QString nextToken;
        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement()) {
                continue;
            }
            if (!listingRoot) {
                listingRoot = reader.name() == QLatin1String("ListBucketResult");
                if (!listingRoot) {
                    break;
                }
            }
            if (reader.name() == QLatin1String("Key")) {
                const QString key = elementText(reader);
                const QString rootPrefix = m_configuration.prefix.isEmpty() ? QString() : m_configuration.prefix + QLatin1Char('/');
                if (key.startsWith(rootPrefix)) {
                    relativeKeys->append(key.mid(rootPrefix.size()));
                }
            } else if (reader.name() == QLatin1String("IsTruncated")) {
                truncated = elementText(reader) == QLatin1String("true");
            } else if (reader.name() == QLatin1String("NextContinuationToken")) {
                nextToken = elementText(reader);
            }
        }
        if (reader.hasError() || !listingRoot || (truncated && (nextToken.isEmpty() || seenTokens.contains(nextToken)))) {
            if (error) {
                *error = QStringLiteral("S3 returned an invalid object listing");
            }
            return false;
        }
        seenTokens.insert(nextToken);
        continuationToken = truncated ? nextToken : QString();
    } while (!continuationToken.isEmpty());
    if (error) {
        error->clear();
    }
    return true;
}
