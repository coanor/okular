/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QUrl>

struct S3Configuration {
    QString bucket;
    QString prefix;
    QString region;
    QUrl endpoint;
    QString accessKeyId;
    QString secretAccessKey;
    QString sessionToken;

    // Reads the app's bucket/prefix and the standard AWS environment or a
    // static AWS shared-credentials profile. Never includes secrets in errors.
    static bool fromEnvironment(const QProcessEnvironment &environment, S3Configuration *configuration, QString *error);
    QString objectKey(const QString &relativeKey) const;
};
