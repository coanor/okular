/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "textselectionutils.h"

std::optional<QPointF> TextSelectionUtils::nearestTextPoint(const Okular::TextEntity::List &entities, const QPointF &point, const QSizeF &size, qreal radius)
{
    if (size.width() <= 0 || size.height() <= 0 || radius < 0) {
        return std::nullopt;
    }
    std::optional<QPointF> nearest;
    qreal bestDistance = radius * radius;
    for (const auto &entity : entities) {
        if (entity.text().trimmed().isEmpty()) {
            continue;
        }
        const auto rect = entity.area();
        const qreal dx = point.x() - qBound(rect.left * size.width(), point.x(), rect.right * size.width());
        const qreal dy = point.y() - qBound(rect.top * size.height(), point.y(), rect.bottom * size.height());
        const qreal distance = dx * dx + dy * dy;
        if (distance <= bestDistance && (!nearest || distance < bestDistance)) {
            const auto center = rect.center();
            nearest = QPointF(center.x, center.y);
            bestDistance = distance;
            if (distance == 0) {
                break;
            }
        }
    }
    return nearest;
}

QString TextSelectionUtils::selectionText(const Okular::TextEntity::List &entities, const QTransform &unrotate)
{
    QString text;
    std::optional<Okular::NormalizedRect> previous;
    bool lineBreak = false;
    for (const auto &entity : entities) {
        const QString part = entity.text();
        if (!part.trimmed().isEmpty()) {
            const auto area = entity.transformedArea(unrotate);
            if (previous && !lineBreak && (area.top >= previous->bottom || area.bottom <= previous->top)) {
                text += QLatin1Char('\n');
            }
            previous = area;
            lineBreak = false;
        }
        text += part;
        lineBreak |= part.contains(QLatin1Char('\n')) || part.contains(QLatin1Char('\r'));
    }
    return text;
}
