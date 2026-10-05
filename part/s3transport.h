/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "s3configuration.h"

#include <QByteArray>
#include <QStringList>

struct S3Response {
    long status = 0;
    QByteArray body;
    QString error;

    bool successful() const
    {
        return status >= 200 && status < 300 && error.isEmpty();
    }
};

class BookObjectStore
{
public:
    virtual ~BookObjectStore() = default;
    virtual S3Response getObject(const QString &relativeKey) const = 0;
    virtual S3Response putObjectIfAbsent(const QString &relativeKey, const QByteArray &body) const = 0;
    virtual S3Response putFileIfAbsent(const QString &relativeKey, const QString &sourcePath, const QString &expectedSha256) const = 0;
    virtual S3Response downloadFile(const QString &relativeKey, const QString &destinationPath, const QString &expectedSha256) const = 0;
    virtual bool listObjects(const QString &relativePrefix, QStringList *relativeKeys, QString *error) const = 0;
};

// Blocking transport; callers must run network operations outside the UI
// thread. libcurl supplies AWS Signature V4 for S3-compatible endpoints.
class S3Transport : public BookObjectStore
{
public:
    explicit S3Transport(S3Configuration configuration);
    S3Response getObject(const QString &relativeKey) const override;
    S3Response putObjectIfAbsent(const QString &relativeKey, const QByteArray &body) const override;
    S3Response putFileIfAbsent(const QString &relativeKey, const QString &sourcePath, const QString &expectedSha256) const override;
    S3Response downloadFile(const QString &relativeKey, const QString &destinationPath, const QString &expectedSha256) const override;
    bool listObjects(const QString &relativePrefix, QStringList *relativeKeys, QString *error) const override;

private:
    S3Response request(const QByteArray &method,
                       const QString &relativeKey,
                       const QByteArray *body,
                       bool ifAbsent,
                       bool listRequest = false,
                       const QString &listPrefix = {},
                       const QString &continuationToken = {},
                       const QString &uploadPath = {},
                       const QString &downloadPath = {},
                       const QString &expectedSha256 = {}) const;
    S3Configuration m_configuration;
};
