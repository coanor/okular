/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_ANNOTATIONSIDECAR_P_H
#define OKULAR_ANNOTATIONSIDECAR_P_H

#include <QList>
#include <QString>

#include "okularcore_export.h"

namespace Okular
{

struct SidecarAnnotation {
    QString id;
    int page = -1;
    int subtype = 0;
    QString xml;
    QString contents = {};
    QString author = {};
    QString color = {};
    bool hiddenNative = false;
};

/** Stores annotation changes in one SQLite file per exact PDF byte hash. */
class OKULARCORE_EXPORT AnnotationSidecar
{
public:
    static QString pdfHash(const QString &pdfPath, QString *error);
    static QString pathForHash(const QString &hash);
    static bool load(const QString &hash, QList<SidecarAnnotation> *annotations, QString *error, qint64 *revision = nullptr);
    static bool save(const QString &hash, const QList<SidecarAnnotation> &annotations, QString *error, qint64 expectedRevision = -1, qint64 *newRevision = nullptr);
    // Produces a consistent, standalone SQLite file at a new path. The caller
    // owns the snapshot and should delete it after uploading or inspecting it.
    static bool snapshot(const QString &hash, const QString &destination, QString *error, qint64 *revision = nullptr);
};

}

#endif
