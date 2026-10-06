/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "readingsession.h"
#include "readinghistory.h"

#include "core/document.h"
#include "core/readingdatastore_p.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QtConcurrentRun>

ReadingSession::ReadingSession(Okular::Document *document, QObject *parent)
    : QObject(parent)
    , m_document(document)
{
    m_pool.setMaxThreadCount(1);
    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(1000);
    connect(&m_saveTimer, &QTimer::timeout, this, &ReadingSession::save);
    m_document->addObserver(this);
    connect(qApp, &QCoreApplication::aboutToQuit, this, &ReadingSession::flush);
    if (auto *application = qobject_cast<QGuiApplication *>(qApp)) {
        connect(application, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) {
                flush();
            }
        });
    }
}

ReadingSession::~ReadingSession()
{
    end();
    m_pool.waitForDone();
    m_document->removeObserver(this);
}

void ReadingSession::begin(const QUrl &url, const QString &title, bool resume)
{
    m_url = url.adjusted(QUrl::RemoveFragment);
    m_title = title;
    m_hash = std::make_shared<QString>(m_document->contentHash());
    ++m_generation;
    if (resume && !url.hasFragment()) {
        reload();
    } else {
        save();
    }
}

void ReadingSession::end()
{
    if (m_saveTimer.isActive()) {
        save();
    }
    m_saveTimer.stop();
    m_url = {};
    m_pendingResume = false;
    ++m_generation;
}

void ReadingSession::flush()
{
    if (m_saveTimer.isActive()) {
        save();
        m_saveTimer.stop();
    }
    // Lifecycle boundaries must finish pending writes before the document
    // is closed or the process is suspended. Navigation itself never waits.
    m_pool.waitForDone();
}

void ReadingSession::reload()
{
    if (m_url.isEmpty() || m_url.scheme() == QLatin1String("fd") || !m_document->isOpened()) {
        return;
    }
    m_pendingResume = true;
    const quint64 generation = ++m_generation;
    struct Outcome {
        ReadingRecord record;
        QString error;
    };
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, generation] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        if (generation != m_generation || !m_pendingResume || !m_document->isOpened()) {
            return;
        }
        m_pendingResume = false;
        if (!outcome.error.isEmpty()) {
            Q_EMIT error(outcome.error);
            return;
        }
        if (!outcome.record.url.isEmpty() && outcome.record.pageCount > 0) {
            m_restoring = true;
            m_document->setViewportPage(qBound(0, outcome.record.page, int(m_document->pages()) - 1));
            m_restoring = false;
            Q_EMIT resumed();
        }
        save();
    });
    const int page = m_document->currentPage();
    const int pageCount = m_document->pages();
    const qint64 openedAt = QDateTime::currentMSecsSinceEpoch();
    watcher->setFuture(QtConcurrent::run(&m_pool, [url = m_url, title = m_title, hash = m_hash, page, pageCount, openedAt] {
        Outcome outcome;
        if (hash->isEmpty()) {
            *hash = Okular::ReadingDataStore::fileHash(url, &outcome.error);
        }
        if (!hash->isEmpty()) {
            ReadingHistory history;
            if (history.read(*hash, url, &outcome.record, &outcome.error) && outcome.record.url.isEmpty()) {
                // Register first opens even if the view closes before the
                // queued resume callback can run. Existing progress is kept.
                auto initial = ReadingHistory::record(url, title, page, pageCount);
                initial.bookId = *hash;
                initial.updatedAt = openedAt;
                history.save(initial, &outcome.error);
                outcome.record = initial;
            }
        }
        return outcome;
    }));
}

void ReadingSession::notifyViewportChanged(bool)
{
    if (m_restoring || m_url.isEmpty() || !m_document->isOpened()) {
        return;
    }
    // Explicit navigation while the history read is pending wins over resume.
    m_pendingResume = false;
    m_saveTimer.start();
}

void ReadingSession::save()
{
    if (m_url.isEmpty() || m_url.scheme() == QLatin1String("fd") || !m_document->isOpened()) {
        return;
    }
    const QUrl url = m_url;
    const QString title = m_title;
    const int page = m_document->currentPage();
    const int pageCount = m_document->pages();
    // Capture the reading time before queueing, so older work cannot supersede
    // a newer position just because it finishes later.
    const qint64 updatedAt = QDateTime::currentMSecsSinceEpoch();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString message = watcher->result();
        watcher->deleteLater();
        if (message.isEmpty()) {
            Q_EMIT historyChanged();
        } else {
            Q_EMIT error(message);
        }
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [url, title, page, pageCount, updatedAt, hash = m_hash] {
        QString error;
        if (hash->isEmpty()) {
            *hash = Okular::ReadingDataStore::fileHash(url, &error);
        }
        if (hash->isEmpty()) {
            return error;
        }
        ReadingRecord record = ReadingHistory::record(url, title, page, pageCount);
        record.bookId = *hash;
        record.updatedAt = updatedAt;
        ReadingHistory().save(record, &error);
        return error;
    }));
}

#include "moc_readingsession.cpp"
