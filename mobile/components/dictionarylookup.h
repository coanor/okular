/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QObject>
#include <QUrl>
#include <qqmlregistration.h>

class DictionaryLookup : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool autoLookupEnabled READ autoLookupEnabled WRITE setAutoLookupEnabled NOTIFY autoLookupEnabledChanged)
    Q_PROPERTY(QString dictionaryFile READ dictionaryFile WRITE setDictionaryFile NOTIFY dictionaryFileChanged)
    Q_PROPERTY(QString word READ word NOTIFY resultChanged)
    Q_PROPERTY(QString definition READ definition NOTIFY resultChanged)
    Q_PROPERTY(QString error READ error NOTIFY resultChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY resultChanged)
    Q_PROPERTY(bool importing READ importing NOTIFY importChanged)
    Q_PROPERTY(QString importError READ importError NOTIFY importChanged)
    Q_PROPERTY(bool mdxAvailable READ mdxAvailable CONSTANT)

public:
    explicit DictionaryLookup(QObject *parent = nullptr);

    bool autoLookupEnabled() const;
    void setAutoLookupEnabled(bool enabled);
    QString dictionaryFile() const;
    void setDictionaryFile(const QString &file);

    QString word() const;
    QString definition() const;
    QString error() const;
    bool loading() const;
    bool importing() const;
    QString importError() const;
    bool mdxAvailable() const;

    Q_INVOKABLE void lookup(const QString &selectedText);
    Q_INVOKABLE void retry(const QString &selectedText);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void importFile(const QUrl &source);

Q_SIGNALS:
    void autoLookupEnabledChanged();
    void dictionaryFileChanged();
    void resultChanged();
    void importChanged();

private:
    QString m_word;
    QString m_lookupFile;
    QString m_definition;
    QString m_error;
    QString m_importError;
    int m_request = 0;
    bool m_loading = false;
    bool m_importing = false;
};
