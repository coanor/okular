/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cloudlibrary.h"
#include "cloudcredentials.h"
#include "part/annotationsync.h"
#include "part/booklibrary.h"
#include "part/booklibrarysync.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtConcurrent>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#endif

#include <KLocalizedString>

#include <utility>

namespace
{
QVariantMap publicSettings(const S3Configuration &configuration, bool remember)
{
    return {{QStringLiteral("bucket"), configuration.bucket},
            {QStringLiteral("prefix"), configuration.prefix},
            {QStringLiteral("region"), configuration.region},
            {QStringLiteral("endpoint"), configuration.endpoint.toString()},
            {QStringLiteral("accessKeyId"), configuration.accessKeyId},
            {QStringLiteral("remember"), remember}};
}

bool validate(const S3Configuration &input, const QString &root, S3Configuration *output, QString *error)
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("OKULAR_S3_BUCKET"), input.bucket);
    environment.insert(QStringLiteral("OKULAR_S3_PREFIX"), input.prefix);
    environment.insert(QStringLiteral("AWS_REGION"), input.region);
    environment.insert(QStringLiteral("AWS_ENDPOINT_URL_S3"), input.endpoint.toString());
    environment.insert(QStringLiteral("AWS_ACCESS_KEY_ID"), input.accessKeyId);
    environment.insert(QStringLiteral("AWS_SECRET_ACCESS_KEY"), input.secretAccessKey);
    environment.insert(QStringLiteral("AWS_SESSION_TOKEN"), input.sessionToken);
    // Mobile configuration must never fall back to unrelated AWS profiles.
    environment.insert(QStringLiteral("AWS_SHARED_CREDENTIALS_FILE"), QDir(root).filePath(QStringLiteral("unused-aws-credentials")));
    environment.insert(QStringLiteral("AWS_CONFIG_FILE"), QDir(root).filePath(QStringLiteral("unused-aws-config")));
    return S3Configuration::fromEnvironment(environment, output, error);
}

QJsonObject readObject(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}
}

CloudLibrary::CloudLibrary(QObject *parent)
    : CloudLibrary(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/cloud-library"), parent)
{
}

CloudLibrary::CloudLibrary(QString root, QObject *parent)
    : QObject(parent)
    , m_root(std::move(root))
{
    run(i18n("Opening cloud library…"), [root = m_root] {
        Outcome outcome = scan(root);
        QSettings settings(QDir(root).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        outcome.configuration.bucket = settings.value(QStringLiteral("bucket")).toString();
        outcome.configuration.prefix = settings.value(QStringLiteral("prefix")).toString();
        outcome.configuration.region = settings.value(QStringLiteral("region")).toString();
        outcome.configuration.endpoint = QUrl(settings.value(QStringLiteral("endpoint")).toString());
        outcome.remember = settings.value(QStringLiteral("remember"), false).toBool();
        if (outcome.remember) {
            QString payload;
            if (CloudCredentials::load(&payload, &outcome.error)) {
                const QJsonObject credentials = QJsonDocument::fromJson(payload.toUtf8()).object();
                outcome.configuration.accessKeyId = credentials.value(QStringLiteral("accessKeyId")).toString();
                outcome.configuration.secretAccessKey = credentials.value(QStringLiteral("secretAccessKey")).toString();
                outcome.configuration.sessionToken = credentials.value(QStringLiteral("sessionToken")).toString();
            }
        }
        outcome.configurationChanged = true;
        return outcome;
    });
}

bool CloudLibrary::available() const
{
#ifdef OKULAR_HAVE_S3_CURL
    return true;
#else
    return false;
#endif
}

bool CloudLibrary::credentialStorageAvailable() const
{
    return CloudCredentials::available();
}

bool CloudLibrary::busy() const
{
    return m_busy;
}
bool CloudLibrary::configured() const
{
    return !m_configuration.bucket.isEmpty() && !m_configuration.region.isEmpty() && !m_configuration.accessKeyId.isEmpty() && !m_configuration.secretAccessKey.isEmpty();
}
QVariantMap CloudLibrary::settings() const
{
    return publicSettings(m_configuration, m_remember);
}
QVariantList CloudLibrary::books() const
{
    return m_books;
}
QVariantList CloudLibrary::conflicts() const
{
    return m_conflicts;
}
QString CloudLibrary::status() const
{
    return m_status;
}
QString CloudLibrary::error() const
{
    return m_error;
}

void CloudLibrary::fail(const QString &error)
{
    m_error = error;
    Q_EMIT changed();
}

void CloudLibrary::run(const QString &status, std::function<Outcome()> task)
{
    if (m_busy) {
        return;
    }
    m_busy = true;
    m_error.clear();
    m_status = status;
    Q_EMIT changed();
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        if (outcome.configurationChanged) {
            m_configuration = outcome.configuration;
            m_remember = outcome.remember;
        }
        m_books = outcome.books;
        m_conflicts = outcome.conflicts;
        m_status = outcome.status;
        m_error = outcome.error;
        m_busy = false;
        Q_EMIT changed();
        Q_EMIT finished();
        if (!outcome.importedBook.isEmpty()) {
            Q_EMIT bookImported(outcome.importedBook);
        }
    });
    watcher->setFuture(QtConcurrent::run(std::move(task)));
}

CloudLibrary::Outcome CloudLibrary::scan(const QString &root)
{
    Outcome outcome;
    const QDir books(QDir(root).filePath(QStringLiteral("books")));
    const QRegularExpression hash(QStringLiteral("^[a-f0-9]{64}$"));
    for (const QFileInfo &entry : books.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (entry.isSymLink() || !hash.match(entry.fileName()).hasMatch()) {
            continue;
        }
        const QDir project(entry.filePath());
        const QJsonObject manifest = readObject(project.filePath(QStringLiteral("manifest.json")));
        const QString storedName = manifest.value(QStringLiteral("storedName")).toString();
        const QFileInfo source(project.filePath(storedName));
        if (manifest.value(QStringLiteral("schemaVersion")).toInt() != 1 || manifest.value(QStringLiteral("sha256")).toString() != entry.fileName() || storedName.isEmpty() || QFileInfo(storedName).fileName() != storedName ||
            storedName.contains(QLatin1Char('\\')) || source.isSymLink() || !source.isFile()) {
            continue;
        }
        const QString title = manifest.value(QStringLiteral("originalName")).toString();
        outcome.books.append(QVariantMap {{QStringLiteral("id"), entry.fileName()}, {QStringLiteral("title"), title}, {QStringLiteral("url"), QUrl::fromLocalFile(source.filePath())}});
        const QJsonObject conflicts = readObject(project.filePath(QStringLiteral("annotation-conflicts.json")));
        if (conflicts.value(QStringLiteral("pdfSha256")).toString() != entry.fileName()) {
            continue;
        }
        for (const QJsonValue &value : conflicts.value(QStringLiteral("conflicts")).toArray()) {
            QVariantMap conflict = value.toObject().toVariantMap();
            if (conflict.value(QStringLiteral("page"), -1).toInt() < 0 || conflict.value(QStringLiteral("id")).toString().isEmpty() || conflict.value(QStringLiteral("variants")).toList().size() < 2) {
                continue;
            }
            conflict.insert(QStringLiteral("bookId"), entry.fileName());
            conflict.insert(QStringLiteral("title"), title);
            outcome.conflicts.append(conflict);
        }
    }
    return outcome;
}

void CloudLibrary::configure(const QVariantMap &settings, const QString &secret, const QString &sessionToken, bool remember)
{
    if (m_busy) {
        return;
    }
    S3Configuration input;
    input.bucket = settings.value(QStringLiteral("bucket")).toString().trimmed();
    input.prefix = settings.value(QStringLiteral("prefix")).toString().trimmed();
    input.region = settings.value(QStringLiteral("region")).toString().trimmed();
    input.endpoint = QUrl(settings.value(QStringLiteral("endpoint")).toString().trimmed());
    input.accessKeyId = settings.value(QStringLiteral("accessKeyId")).toString().trimmed();
    input.secretAccessKey = secret.isEmpty() && input.accessKeyId == m_configuration.accessKeyId ? m_configuration.secretAccessKey : secret;
    input.sessionToken = sessionToken;
    S3Configuration configuration;
    QString error;
    if (!validate(input, m_root, &configuration, &error)) {
        fail(error);
        return;
    }
    remember = remember && credentialStorageAvailable();
    run(i18n("Saving cloud settings…"), [root = m_root, configuration, remember] {
        Outcome outcome = scan(root);
        if (!QDir().mkpath(root)) {
            outcome.error = QStringLiteral("Could not create the private cloud library.");
            return outcome;
        }
        const QJsonObject credentials {{QStringLiteral("accessKeyId"), configuration.accessKeyId}, {QStringLiteral("secretAccessKey"), configuration.secretAccessKey}, {QStringLiteral("sessionToken"), configuration.sessionToken}};
        const QString payload = remember ? QString::fromUtf8(QJsonDocument(credentials).toJson(QJsonDocument::Compact)) : QString();
        if (!CloudCredentials::save(payload, &outcome.error)) {
            return outcome;
        }
        QSettings settings(QDir(root).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        QVariantMap values = publicSettings(configuration, remember);
        values.remove(QStringLiteral("accessKeyId"));
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            settings.setValue(it.key(), it.value());
        }
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            outcome.error = QStringLiteral("Could not save cloud settings.");
            return outcome;
        }
        outcome.configurationChanged = true;
        outcome.configuration = configuration;
        outcome.remember = remember;
        outcome.status = i18n("Cloud settings saved.");
        return outcome;
    });
}

void CloudLibrary::forgetCredentials()
{
    if (m_busy) {
        return;
    }
    run(i18n("Removing saved credentials…"), [root = m_root, configuration = m_configuration] {
        Outcome outcome = scan(root);
        if (!CloudCredentials::save({}, &outcome.error)) {
            return outcome;
        }
        QSettings settings(QDir(root).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
        settings.setValue(QStringLiteral("remember"), false);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            outcome.error = QStringLiteral("Could not save cloud settings.");
        }
        outcome.configuration = configuration;
        outcome.configuration.accessKeyId.clear();
        outcome.configuration.secretAccessKey.clear();
        outcome.configuration.sessionToken.clear();
        outcome.configurationChanged = true;
        outcome.status = i18n("Saved S3 credentials removed.");
        return outcome;
    });
}

void CloudLibrary::refresh()
{
    run(i18n("Opening cloud library…"), [root = m_root] { return scan(root); });
}

void CloudLibrary::importBook(const QUrl &url)
{
    if (m_busy) {
        return;
    }
    if (!url.isLocalFile() && url.scheme() != QLatin1String("content")) {
        fail(i18n("Choose a local document to add to the cloud library."));
        return;
    }
    run(i18n("Adding book to cloud library…"), [root = m_root, url] {
        Outcome outcome = scan(root);
        // Always stage a copy: Android providers may not support rename/remove.
        const QString sourcePath = url.isLocalFile() ? url.toLocalFile() : url.toString(QUrl::FullyEncoded);
        QFile source(sourcePath);
        QTemporaryDir staging;
        QString name = QFileInfo(sourcePath).fileName();
#ifdef Q_OS_ANDROID
        if (url.scheme() == QLatin1String("content")) {
            const QJniObject context = QNativeInterface::QAndroidApplication::context();
            const QJniObject displayName =
                QJniObject::callStaticObjectMethod("org/kde/okular/CloudPlatform", "documentName", "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;", context.object(), QJniObject::fromString(sourcePath).object<jstring>());
            QJniEnvironment environment;
            environment.checkAndClearExceptions(QJniEnvironment::OutputMode::Silent);
            name = displayName.toString();
        }
#endif
        if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..") || QFileInfo(name).fileName() != name || name.contains(QLatin1Char('\\'))) {
            name = QStringLiteral("document");
        }
        QFile copy(staging.filePath(name));
        if (!staging.isValid() || !source.open(QIODevice::ReadOnly) || !copy.open(QIODevice::WriteOnly)) {
            outcome.error = i18n("Could not read the selected document.");
            return outcome;
        }
        while (!source.atEnd()) {
            const QByteArray data = source.read(1024 * 1024);
            if (data.isEmpty() || copy.write(data) != data.size()) {
                outcome.error = i18n("Could not copy the selected document.");
                return outcome;
            }
        }
        if (source.error() != QFileDevice::NoError || !copy.flush()) {
            outcome.error = i18n("Could not copy the selected document.");
            return outcome;
        }
        copy.close();
        BookProject project;
        if (!BookLibrary::importFile(copy.fileName(), root, &project, &outcome.error)) {
            return outcome;
        }
        outcome = scan(root);
        outcome.importedBook = QUrl::fromLocalFile(project.sourcePath);
        outcome.status = i18n("Book added. The original document was kept.");
        return outcome;
    });
}

void CloudLibrary::synchronize()
{
    if (m_busy) {
        return;
    }
    if (!available() || !configured()) {
        fail(i18n("Configure S3 before synchronizing the cloud library."));
        return;
    }
#ifdef OKULAR_HAVE_S3_CURL
    run(i18n("Synchronizing books and annotations…"), [root = m_root, configuration = m_configuration] {
        S3Transport store(configuration);
        const BookSyncResult result = BookLibrarySync(store, root).synchronizeAll();
        Outcome outcome = scan(root);
        outcome.error = result.error;
        if (result.successful()) {
            outcome.status = i18n("Uploaded %1 books and %2 annotations; downloaded %3 books and applied %4 annotations. %5 conflicts need a choice.",
                                  result.uploaded,
                                  result.annotationsUploaded,
                                  result.downloaded,
                                  result.annotationsApplied,
                                  result.annotationConflicts);
        }
        return outcome;
    });
#endif
}

void CloudLibrary::resolveConflict(int conflictIndex, int variantIndex)
{
    if (m_busy) {
        return;
    }
    if (!available() || !configured() || conflictIndex < 0 || conflictIndex >= m_conflicts.size()) {
        fail(i18n("Choose an annotation conflict and configure S3 first."));
        return;
    }
    const QVariantMap choice = m_conflicts.at(conflictIndex).toMap();
    const QVariantList variants = choice.value(QStringLiteral("variants")).toList();
    if (variantIndex < 0 || variantIndex >= variants.size()) {
        fail(i18n("Choose an annotation version."));
        return;
    }
    QStringList heads;
    for (const QVariant &variant : variants) {
        heads.append(variant.toMap().value(QStringLiteral("eventId")).toString());
    }
#ifdef OKULAR_HAVE_S3_CURL
    run(i18n("Resolving annotation conflict…"), [root = m_root, configuration = m_configuration, choice, heads, variantIndex] {
        AnnotationSyncResult result;
        QLockFile lock(QDir(root).filePath(QStringLiteral(".sync.lock")));
        if (!lock.tryLock(0)) {
            result.error = QStringLiteral("This managed library is already syncing");
        } else {
            S3Transport store(configuration);
            result =
                AnnotationSync::resolveConflict(store, root, choice.value(QStringLiteral("bookId")).toString(), choice.value(QStringLiteral("page")).toInt(), choice.value(QStringLiteral("id")).toString(), heads, heads.at(variantIndex));
        }
        Outcome outcome = scan(root);
        outcome.error = result.error;
        outcome.status = result.successful() ? i18n("Annotation version saved. %1 conflicts remain in this book.", result.conflicts) : QString();
        return outcome;
    });
#endif
}
