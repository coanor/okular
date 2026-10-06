/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "okularcore_export.h"
#include <QString>
#include <QUrl>

class QSqlDatabase;

namespace Okular
{
// Connections belong to callers and stay on their worker thread. This module
// owns the shared schema, legacy import and consistent database exports.
class OKULARCORE_EXPORT ReadingDataStore
{
public:
    static QString defaultPath();
    static QString deviceId();
    static bool isBookHash(const QString &hash);
    static QString fileHash(const QUrl &url, QString *error);
    static bool initialize(QSqlDatabase &database, QString *error);
    static bool ensureBook(QSqlDatabase &database, const QString &hash, const QString &name, QString *error);
    static bool importAiHistory(QSqlDatabase &database, const QString &source, QString *error);
    static bool importAnnotations(QSqlDatabase &database, const QString &directory, QString *error, const QString &onlyHash = {});
    static bool importReadingHistory(QSqlDatabase &database, const QString &source, QString *error);
    enum class LegacyData { ReadingHistory, AiHistory };
    static bool importLegacyData(QSqlDatabase &database, LegacyData kind, QString *error);
    static bool exportDatabase(const QUrl &destination, QString *error);
    static bool snapshot(const QString &source, const QString &destination, QString *error);
};
}
