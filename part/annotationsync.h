/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "s3transport.h"

#include <QString>

struct AnnotationSyncResult {
    int uploaded = 0;
    int applied = 0;
    int conflicts = 0;
    int deferred = 0;
    int snapshotsUploaded = 0;
    QString error;

    bool successful() const
    {
        return error.isEmpty();
    }
};

// Synchronizes one PDF's sidecar by publishing immutable per-annotation events.
// The caller must hold the managed library's .sync.lock and run off the UI thread.
class AnnotationSync
{
public:
    static AnnotationSyncResult synchronize(const BookObjectStore &store, const QString &libraryRoot, const QString &pdfHash, bool deferRemoteApply = false);
};
