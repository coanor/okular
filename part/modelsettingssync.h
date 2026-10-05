/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "aistore.h"
#include "s3transport.h"

#include <QByteArray>

struct ModelSettingsSyncResult {
    QList<AiProfile> profiles;
    int uploaded = 0;
    int applied = 0;
    int conflicts = 0;
    QByteArray checkpoint;
    QString error;

    bool successful() const
    {
        return error.isEmpty();
    }
};

// The caller holds the managed library lock. Public profile fields and book
// preferences are synced; credentials and conversations never enter events.
class ModelSettingsSync
{
public:
    static ModelSettingsSyncResult synchronize(const BookObjectStore &store, const QString &root, const QList<AiProfile> &localProfiles);
    static ModelSettingsSyncResult resolveConflict(const BookObjectStore &store, const QString &root, const QList<AiProfile> &localProfiles, const QString &key, const QStringList &expectedHeads, const QString &chosenHead);
    static bool commit(const QString &root, const ModelSettingsSyncResult &result, QString *error);
};
