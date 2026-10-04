/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cloudcredentials.h"

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif

bool CloudCredentials::available()
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

bool CloudCredentials::load(QString *payload, QString *error)
{
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject result = QJniObject::callStaticObjectMethod("org/kde/okular/CloudPlatform", "load", "(Landroid/content/Context;)Ljava/lang/String;", context.object());
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions(QJniEnvironment::OutputMode::Silent) || !result.isValid()) {
        *error = QStringLiteral("Could not unlock stored S3 credentials. Enter the credentials again.");
        return false;
    }
    *payload = result.toString();
#else
    Q_UNUSED(error)
    payload->clear();
#endif
    return true;
}

bool CloudCredentials::save(const QString &payload, QString *error)
{
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const bool result = QJniObject::callStaticMethod<jboolean>("org/kde/okular/CloudPlatform", "save", "(Landroid/content/Context;Ljava/lang/String;)Z", context.object(), QJniObject::fromString(payload).object<jstring>());
    QJniEnvironment environment;
    if (environment.checkAndClearExceptions(QJniEnvironment::OutputMode::Silent) || !result) {
        *error = QStringLiteral("Could not save S3 credentials securely.");
        return false;
    }
#else
    Q_UNUSED(payload)
    Q_UNUSED(error)
#endif
    return true;
}
