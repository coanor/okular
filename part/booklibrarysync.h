/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "s3transport.h"

#include <QString>

struct BookSyncResult {
    int uploaded = 0;
    int downloaded = 0;
    int annotationsUploaded = 0;
    int annotationsApplied = 0;
    int annotationConflicts = 0;
    int annotationsDeferred = 0;
    int snapshotsUploaded = 0;
    QString error;

    bool successful() const
    {
        return error.isEmpty();
    }
};

// Call from a worker thread. Model profile synchronization is a later slice.
class BookLibrarySync
{
public:
    BookLibrarySync(const BookObjectStore &store, QString libraryRoot);
    BookSyncResult synchronizeSources() const;
    BookSyncResult synchronizeAnnotations(const QString &deferHash = {}) const;
    BookSyncResult synchronizeAll(const QString &activePdfPath = {}) const;

private:
    bool ensureLibraryIdentity(QString *error) const;
    const BookObjectStore &m_store;
    QString m_libraryRoot;
};
