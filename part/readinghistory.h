/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QList>
#include <QSqlDatabase>
#include <QUrl>
#include <functional>

struct ReadingRecord {
    QUrl url;
    QString title;
    QString bookId;
    int page = 0;
    int pageCount = 0; // Zero for migrated recent files whose position is unknown.
    qint64 updatedAt = 0;
    QString revision;
};

// Create and use each instance on one worker thread. Connections are private;
// SQLite serializes writers from different local reader instances.
class ReadingHistory
{
public:
    using FileNameResolver = std::function<QString(const QUrl &)>;
    explicit ReadingHistory(const QString &path = defaultPath(), FileNameResolver fileNameResolver = {});
    ~ReadingHistory();
    ReadingHistory(const ReadingHistory &) = delete;
    ReadingHistory &operator=(const ReadingHistory &) = delete;

    static QString defaultPath();
    static ReadingRecord record(const QUrl &url, const QString &title, int page, int pageCount);
    bool read(const QUrl &url, ReadingRecord *record, QString *error);
    bool read(const QString &hash, const QUrl &url, ReadingRecord *record, QString *error);
    bool entries(QList<ReadingRecord> *records, QString *error);
    bool save(const ReadingRecord &record, QString *error);
    bool importLegacyLibrary(const QString &root, QString *error);

private:
    bool open(QString *error);
    QString m_path;
    FileNameResolver m_fileNameResolver;
    QSqlDatabase m_database;
};
