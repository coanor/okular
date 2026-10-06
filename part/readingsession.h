/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/observer.h"

#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <memory>

namespace Okular
{
class Document;
}

// Tracks one open document. Database work is ordered on a private worker pool.
class ReadingSession : public QObject, public Okular::DocumentObserver
{
    Q_OBJECT
public:
    explicit ReadingSession(Okular::Document *document, QObject *parent = nullptr);
    ~ReadingSession() override;
    void begin(const QUrl &url, const QString &title, bool resume = true);
    void end();
    void flush();
    void reload();
    void notifyViewportChanged(bool smoothMove) override;

Q_SIGNALS:
    void resumed();
    void historyChanged();
    void error(const QString &message);

private:
    void save();
    Okular::Document *m_document;
    QUrl m_url;
    QString m_title;
    std::shared_ptr<QString> m_hash; // Accessed only by ordered worker jobs.
    QThreadPool m_pool;
    QTimer m_saveTimer;
    quint64 m_generation = 0;
    bool m_restoring = false;
    bool m_pendingResume = false;
};
