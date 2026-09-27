/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QString>

namespace MdxDictionary
{
QString word(QString text);
QString summary(QString definition);
QString lookup(const QString &filePath, const QString &word);
void invalidate(const QString &filePath);
}
