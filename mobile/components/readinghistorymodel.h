/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QObject>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <qqmlintegration.h>

class ReadingHistoryModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList books READ books NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit ReadingHistoryModel(QObject *parent = nullptr);
    ~ReadingHistoryModel() override;
    QVariantList books() const;
    QString error() const;
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void exportDatabase(const QUrl &destination);

Q_SIGNALS:
    void changed();
    void exported(const QUrl &destination);
    void exportRequested();

private:
    QThreadPool m_pool;
    QVariantList m_books;
    QString m_error;
    QString m_legacyLibrary;
    bool m_busy = false;
    bool m_refreshPending = false;
};
