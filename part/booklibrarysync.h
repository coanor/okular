/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "s3transport.h"

#include <QString>

struct BookSyncResult {
    int uploaded = 0;
    int downloaded = 0;
    QString error;

    bool successful() const
    {
        return error.isEmpty();
    }
};

// Source-file synchronization. Call from a worker thread; annotation and
// profile synchronization are separate later slices.
class BookLibrarySync
{
public:
    BookLibrarySync(const BookObjectStore &store, QString libraryRoot);
    BookSyncResult synchronizeSources() const;

private:
    bool ensureLibraryIdentity(QString *error) const;
    const BookObjectStore &m_store;
    QString m_libraryRoot;
};
