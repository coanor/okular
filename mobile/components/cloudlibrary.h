/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "part/s3configuration.h"

#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <qqmlintegration.h>

#include <functional>

class CloudLibrary : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool credentialStorageAvailable READ credentialStorageAvailable CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool configured READ configured NOTIFY changed)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY changed)
    Q_PROPERTY(QVariantList books READ books NOTIFY changed)
    Q_PROPERTY(QVariantList conflicts READ conflicts NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    explicit CloudLibrary(QObject *parent = nullptr);
    // A private directory per instance also lets tests represent separate devices.
    explicit CloudLibrary(QString root, QObject *parent = nullptr);
    bool available() const;
    bool credentialStorageAvailable() const;
    bool busy() const;
    bool configured() const;
    QVariantMap settings() const;
    QVariantList books() const;
    QVariantList conflicts() const;
    QString status() const;
    QString error() const;

    Q_INVOKABLE void configure(const QVariantMap &settings, const QString &secret, const QString &sessionToken, bool remember);
    Q_INVOKABLE void forgetCredentials();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void importBook(const QUrl &url);
    // The UI saves and closes the open document before applying remote annotations.
    Q_INVOKABLE void synchronize();
    Q_INVOKABLE void resolveConflict(int conflictIndex, int variantIndex);

Q_SIGNALS:
    void changed();
    void finished();
    void bookImported(const QUrl &url);

private:
    struct Outcome {
        QVariantList books;
        QVariantList conflicts;
        QString status;
        QString error;
        QUrl importedBook;
        bool configurationChanged = false;
        S3Configuration configuration;
        bool remember = false;
    };
    static Outcome scan(const QString &root);
    void run(const QString &status, std::function<Outcome()> task);
    void fail(const QString &error);
    const QString m_root;
    S3Configuration m_configuration;
    bool m_remember = false;
    bool m_busy = false;
    QVariantList m_books;
    QVariantList m_conflicts;
    QString m_status;
    QString m_error;
};
