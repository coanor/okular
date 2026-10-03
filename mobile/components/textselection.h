/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_MOBILE_TEXTSELECTION_H
#define OKULAR_MOBILE_TEXTSELECTION_H

#include "core/textpage.h"

#include <QPointF>
#include <QSizeF>
#include <optional>

namespace MobileTextSelection
{
// Return the center of the nearest text entity within radius (in item coordinates).
std::optional<QPointF> nearestTextPoint(const Okular::TextEntity::List &entities, const QPointF &point, const QSizeF &size, qreal radius);
// Restore line breaks omitted by the generator before joining hyphenated words.
QString selectionText(const Okular::TextEntity::List &entities, const QTransform &unrotate = QTransform());
}

#endif
