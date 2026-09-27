/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../part/s3configuration.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class S3ConfigurationTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void readsEnvironmentWithoutLeakingSecrets()
    {
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("OKULAR_S3_BUCKET"), QStringLiteral("my-books"));
        environment.insert(QStringLiteral("OKULAR_S3_PREFIX"), QStringLiteral("/reader/library/"));
        environment.insert(QStringLiteral("AWS_REGION"), QStringLiteral("eu-west-1"));
        environment.insert(QStringLiteral("AWS_ENDPOINT_URL_S3"), QStringLiteral("https://storage.example:9000"));
        environment.insert(QStringLiteral("AWS_ACCESS_KEY_ID"), QStringLiteral("access-id"));
        environment.insert(QStringLiteral("AWS_SECRET_ACCESS_KEY"), QStringLiteral("private-secret"));
        S3Configuration config;
        QString error;
        QVERIFY(S3Configuration::fromEnvironment(environment, &config, &error));
        QCOMPARE(config.bucket, QStringLiteral("my-books"));
        QCOMPARE(config.objectKey(QStringLiteral("books/a/source.pdf")), QStringLiteral("reader/library/books/a/source.pdf"));
        QCOMPARE(config.endpoint.host(), QStringLiteral("storage.example"));
        QCOMPARE(config.region, QStringLiteral("eu-west-1"));
        QCOMPARE(config.secretAccessKey, QStringLiteral("private-secret"));
        QVERIFY(error.isEmpty());

        environment.insert(QStringLiteral("AWS_ENDPOINT_URL_S3"), QStringLiteral("https://name:private-secret@storage.example"));
        QVERIFY(!S3Configuration::fromEnvironment(environment, &config, &error));
        QVERIFY(!error.contains(QStringLiteral("private-secret")));
    }

    void readsStaticProfile()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile credentials(temp.filePath(QStringLiteral("credentials")));
        QVERIFY(credentials.open(QIODevice::WriteOnly));
        credentials.write("[reading]\naws_access_key_id = profile-id\naws_secret_access_key = profile-secret\naws_session_token = token\n");
        credentials.close();
        QFile configFile(temp.filePath(QStringLiteral("config")));
        QVERIFY(configFile.open(QIODevice::WriteOnly));
        configFile.write("[profile reading]\nregion = us-east-2\n");
        configFile.close();

        QProcessEnvironment environment;
        environment.insert(QStringLiteral("OKULAR_S3_BUCKET"), QStringLiteral("books"));
        environment.insert(QStringLiteral("AWS_PROFILE"), QStringLiteral("reading"));
        environment.insert(QStringLiteral("AWS_SHARED_CREDENTIALS_FILE"), credentials.fileName());
        environment.insert(QStringLiteral("AWS_CONFIG_FILE"), configFile.fileName());
        S3Configuration result;
        QString error;
        QVERIFY2(S3Configuration::fromEnvironment(environment, &result, &error), qPrintable(error));
        QCOMPARE(result.accessKeyId, QStringLiteral("profile-id"));
        QCOMPARE(result.secretAccessKey, QStringLiteral("profile-secret"));
        QCOMPARE(result.sessionToken, QStringLiteral("token"));
        QCOMPARE(result.region, QStringLiteral("us-east-2"));
    }

    void rejectsIncompleteConfiguration()
    {
        QProcessEnvironment environment;
        S3Configuration result;
        QString error;
        QVERIFY(!S3Configuration::fromEnvironment(environment, &result, &error));
        QVERIFY(error.contains(QStringLiteral("OKULAR_S3_BUCKET")));

        environment.insert(QStringLiteral("OKULAR_S3_BUCKET"), QStringLiteral("books"));
        environment.insert(QStringLiteral("AWS_REGION"), QStringLiteral("us-east-1"));
        environment.insert(QStringLiteral("AWS_ACCESS_KEY_ID"), QStringLiteral("unique-access-id-value"));
        QVERIFY(!S3Configuration::fromEnvironment(environment, &result, &error));
        QVERIFY(!error.contains(QStringLiteral("unique-access-id-value")));

        environment.insert(QStringLiteral("AWS_SECRET_ACCESS_KEY"), QStringLiteral("secret"));
        environment.insert(QStringLiteral("AWS_REGION"), QStringLiteral("us-east-1:s3"));
        QVERIFY(!S3Configuration::fromEnvironment(environment, &result, &error));
        QVERIFY(!error.contains(QStringLiteral("secret")));
    }
};

QTEST_MAIN(S3ConfigurationTest)
#include "s3configurationtest.moc"
