/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QString>

struct BookProject {
    QString id; // SHA-256 of the exact source file bytes.
    QString sourcePath;
    QString originalName;
};

class BookLibrary
{
public:
    // Imports a local file into the chosen managed library. Once the managed
    // copy and its metadata are committed, the original file is removed.
    static bool importFile(const QString &sourcePath, const QString &libraryRoot, BookProject *project, QString *error);
};
