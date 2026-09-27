/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "s3configuration.h"

#include <QDir>
#include <QRegularExpression>
#include <QSettings>

namespace
{
bool fail(const QString &reason, QString *error)
{
    if (error) {
        *error = reason;
    }
    return false;
}

QString profileValue(const QString &path, const QString &group, const QString &key)
{
    QSettings settings(path, QSettings::IniFormat);
    settings.beginGroup(group);
    return settings.value(key).toString().trimmed();
}
}

bool S3Configuration::fromEnvironment(const QProcessEnvironment &environment, S3Configuration *configuration, QString *error)
{
    if (!configuration) {
        return fail(QStringLiteral("S3 configuration output is missing"), error);
    }
    S3Configuration result;
    result.bucket = environment.value(QStringLiteral("OKULAR_S3_BUCKET")).trimmed();
    const QRegularExpression bucketName(QStringLiteral("^[A-Za-z0-9._-]+$"));
    if (!bucketName.match(result.bucket).hasMatch()) {
        return fail(QStringLiteral("Set a valid OKULAR_S3_BUCKET"), error);
    }
    result.prefix = environment.value(QStringLiteral("OKULAR_S3_PREFIX")).trimmed();
    while (result.prefix.startsWith(QLatin1Char('/'))) {
        result.prefix.remove(0, 1);
    }
    while (result.prefix.endsWith(QLatin1Char('/'))) {
        result.prefix.chop(1);
    }

    const QString profile = environment.value(QStringLiteral("AWS_PROFILE"), QStringLiteral("default"));
    const QString credentialsFile = environment.value(QStringLiteral("AWS_SHARED_CREDENTIALS_FILE"), QDir::homePath() + QStringLiteral("/.aws/credentials"));
    const QString configFile = environment.value(QStringLiteral("AWS_CONFIG_FILE"), QDir::homePath() + QStringLiteral("/.aws/config"));
    const QString configGroup = profile == QLatin1String("default") ? profile : QStringLiteral("profile ") + profile;

    result.region = environment.value(QStringLiteral("AWS_REGION"));
    if (result.region.isEmpty()) {
        result.region = environment.value(QStringLiteral("AWS_DEFAULT_REGION"));
    }
    if (result.region.isEmpty()) {
        result.region = profileValue(configFile, configGroup, QStringLiteral("region"));
    }
    if (result.region.isEmpty()) {
        return fail(QStringLiteral("Set AWS_REGION or a region in the selected AWS profile"), error);
    }
    const QRegularExpression regionName(QStringLiteral("^[A-Za-z0-9-]+$"));
    if (!regionName.match(result.region).hasMatch()) {
        return fail(QStringLiteral("Set a valid AWS_REGION"), error);
    }

    QString endpointText = environment.value(QStringLiteral("AWS_ENDPOINT_URL_S3"));
    if (endpointText.isEmpty()) {
        endpointText = environment.value(QStringLiteral("AWS_ENDPOINT_URL"));
    }
    if (endpointText.isEmpty()) {
        endpointText = profileValue(configFile, configGroup, QStringLiteral("endpoint_url"));
    }
    if (endpointText.isEmpty()) {
        endpointText = QStringLiteral("https://s3.%1.amazonaws.com").arg(result.region);
    }
    result.endpoint = QUrl(endpointText);
    if (!result.endpoint.isValid() || result.endpoint.host().isEmpty() || (result.endpoint.scheme() != QLatin1String("https") && result.endpoint.scheme() != QLatin1String("http"))
        || !result.endpoint.userInfo().isEmpty() || result.endpoint.hasQuery() || result.endpoint.hasFragment()) {
        return fail(QStringLiteral("Set a valid AWS_ENDPOINT_URL_S3"), error);
    }

    result.accessKeyId = environment.value(QStringLiteral("AWS_ACCESS_KEY_ID"));
    result.secretAccessKey = environment.value(QStringLiteral("AWS_SECRET_ACCESS_KEY"));
    result.sessionToken = environment.value(QStringLiteral("AWS_SESSION_TOKEN"));
    if (result.accessKeyId.isEmpty() && result.secretAccessKey.isEmpty()) {
        result.accessKeyId = profileValue(credentialsFile, profile, QStringLiteral("aws_access_key_id"));
        result.secretAccessKey = profileValue(credentialsFile, profile, QStringLiteral("aws_secret_access_key"));
        result.sessionToken = profileValue(credentialsFile, profile, QStringLiteral("aws_session_token"));
    }
    if (result.accessKeyId.isEmpty() || result.secretAccessKey.isEmpty()) {
        return fail(QStringLiteral("Set AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY or select a static AWS credentials profile"), error);
    }

    *configuration = result;
    if (error) {
        error->clear();
    }
    return true;
}

QString S3Configuration::objectKey(const QString &relativeKey) const
{
    return prefix.isEmpty() ? relativeKey : prefix + QLatin1Char('/') + relativeKey;
}
