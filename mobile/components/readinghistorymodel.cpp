/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "readinghistorymodel.h"
#include "core/readingdatastore_p.h"
#include "part/readinghistory.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QDateTime>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QUuid>
#include <QtConcurrentRun>

ReadingHistoryModel::ReadingHistoryModel(QObject *parent)
    : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
    // Preserve old mobile recent files, including Android content URIs.
    const KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("Recent Files"));
    QList<ReadingRecord> legacy;
    for (const QString &key : group.keyList()) {
        if (!key.startsWith(QLatin1String("File"))) {
            continue;
        }
        bool valid;
        const int index = key.mid(4).toInt(&valid);
        if (!valid || index < 1) {
            continue;
        }
        const QUrl url = QUrl::fromUserInput(group.readPathEntry(key, QString()));
        if (url.isEmpty() || url.scheme() == QLatin1String("fd")) {
            continue;
        }
        ReadingRecord record {url, group.readEntry(QStringLiteral("Name%1").arg(index), url.fileName()), {}, 0, 0, index, QUuid::createUuid().toString(QUuid::WithoutBraces)};
        legacy.append(record);
    }
    (void)QtConcurrent::run(&m_pool, [legacy] {
        ReadingHistory history;
        for (const ReadingRecord &record : legacy) {
            ReadingRecord existing;
            QString error;
            if (history.read(record.url, &existing, &error) && existing.url.isEmpty()) {
                history.save(record, &error);
            }
        }
    });
    m_legacyLibrary = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/cloud-library");
    refresh();
}

ReadingHistoryModel::~ReadingHistoryModel()
{
    m_pool.waitForDone();
}

QVariantList ReadingHistoryModel::books() const
{
    return m_books;
}

QString ReadingHistoryModel::error() const
{
    return m_error;
}

void ReadingHistoryModel::refresh()
{
    if (m_busy) {
        m_refreshPending = true;
        return;
    }
    m_busy = true;
    struct Outcome {
        QVariantList books;
        QString error;
    };
    auto *watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher] {
        const Outcome outcome = watcher->result();
        watcher->deleteLater();
        m_books = outcome.books;
        m_error = outcome.error;
        if (m_error.isEmpty()) {
            m_legacyLibrary.clear();
        }
        m_busy = false;
        Q_EMIT changed();
        if (m_refreshPending) {
            m_refreshPending = false;
            refresh();
        }
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [legacyLibrary = m_legacyLibrary] {
        Outcome outcome;
        QList<ReadingRecord> records;
        ReadingHistory history;
        if (!legacyLibrary.isEmpty() && !history.importLegacyLibrary(legacyLibrary, &outcome.error)) {
            return outcome;
        }
        history.entries(&records, &outcome.error);
        for (const ReadingRecord &record : records) {
            QVariantMap book {{QStringLiteral("url"), record.url}, {QStringLiteral("title"), record.title}, {QStringLiteral("bookId"), record.bookId}, {QStringLiteral("page"), record.page}, {QStringLiteral("pageCount"), record.pageCount}};
            if (record.pageCount > 0) {
                book.insert(QStringLiteral("lastRead"), QDateTime::fromMSecsSinceEpoch(record.updatedAt));
            }
            outcome.books.append(book);
        }
        return outcome;
    }));
}

void ReadingHistoryModel::exportDatabase(const QUrl &destination)
{
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, destination] {
        m_error = watcher->result();
        watcher->deleteLater();
        Q_EMIT changed();
        if (m_error.isEmpty()) {
            Q_EMIT exported(destination);
        }
    });
    Q_EMIT exportRequested();
    watcher->setFuture(QtConcurrent::run(&m_pool, [destination] {
        QString error;
        Okular::ReadingDataStore::exportDatabase(destination, &error);
        return error;
    }));
}

#include "moc_readinghistorymodel.cpp"
