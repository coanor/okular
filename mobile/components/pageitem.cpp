/*
    SPDX-FileCopyrightText: 2012 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "pageitem.h"
#include "documentitem.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QTransform>

#include <KLocalizedString>

#include <core/annotations.h>
#include <core/bookmarkmanager.h>
#include <core/generator.h>
#include <core/misc.h>
#include <core/page.h>

#include "gui/pagepainter.h"
#include "gui/priorities.h"
#include "settings.h"

#define REDRAW_TIMEOUT 250

static QTransform pageRotation(Okular::Rotation rotation)
{
    QTransform matrix;
    matrix.rotate(static_cast<int>(rotation) * 90);
    switch (rotation) {
    case Okular::Rotation90:
        matrix.translate(0, -1);
        break;
    case Okular::Rotation180:
        matrix.translate(-1, -1);
        break;
    case Okular::Rotation270:
        matrix.translate(-1, 0);
        break;
    case Okular::Rotation0:
        break;
    }
    return matrix;
}

PageItem::PageItem(QQuickItem *parent)
    : QQuickItem(parent)
    , Okular::View(QStringLiteral("PageView"))
    , m_page(nullptr)
    , m_bookmarked(false)
    , m_isThumbnail(false)
{
    setFlag(QQuickItem::ItemHasContents, true);

    m_viewPort.rePos.enabled = true;

    m_redrawTimer = new QTimer(this);
    m_redrawTimer->setInterval(REDRAW_TIMEOUT);
    m_redrawTimer->setSingleShot(true);
    connect(m_redrawTimer, &QTimer::timeout, this, &PageItem::requestPixmap);
    connect(this, &QQuickItem::windowChanged, m_redrawTimer, [this]() { m_redrawTimer->start(); });
}

PageItem::~PageItem()
{
}

void PageItem::setFlickable(QQuickItem *flickable)
{
    if (m_flickable.data() == flickable) {
        return;
    }

    // check the object can act as a flickable
    if (!flickable->property("contentX").isValid() || !flickable->property("contentY").isValid()) {
        return;
    }

    if (m_flickable) {
        disconnect(m_flickable.data(), nullptr, this, nullptr);
    }

    // check the object can act as a flickable
    if (!flickable->property("contentX").isValid() || !flickable->property("contentY").isValid()) {
        m_flickable.clear();
        return;
    }

    m_flickable = flickable;

    if (flickable) {
        // NOLINTBEGIN(clazy-old-style-connect);
        // QQuickFlickable is not exported so we need the old-style connects here
        connect(flickable, SIGNAL(contentXChanged()), this, SLOT(contentXChanged())); // clazy:exclude=old-style-connect
        connect(flickable, SIGNAL(contentYChanged()), this, SLOT(contentYChanged())); // clazy:exclude=old-style-connect
        // NOLINTEND(clazy-old-style-connect);
    }

    Q_EMIT flickableChanged();
}

QQuickItem *PageItem::flickable() const
{
    return m_flickable.data();
}

DocumentItem *PageItem::document() const
{
    return m_documentItem.data();
}

void PageItem::setDocument(DocumentItem *doc)
{
    if (doc == m_documentItem.data() || !doc) {
        return;
    }

    clearSelection();
    m_page = nullptr;
    disconnect(doc, nullptr, this, nullptr);
    m_documentItem = doc;
    Observer *observer = m_isThumbnail ? m_documentItem.data()->thumbnailObserver() : m_documentItem.data()->pageviewObserver();
    connect(observer, &Observer::pageChanged, this, &PageItem::pageHasChanged);
    connect(doc->document()->bookmarkManager(), &Okular::BookmarkManager::bookmarksChanged, this, &PageItem::checkBookmarksChanged);
    setPageNumber(0);
    Q_EMIT documentChanged();
    m_redrawTimer->start();

    connect(doc, &DocumentItem::urlChanged, this, &PageItem::refreshPage);
}

int PageItem::pageNumber() const
{
    return m_viewPort.pageNumber;
}

void PageItem::setPageNumber(int number)
{
    if ((m_page && m_viewPort.pageNumber == number) || !m_documentItem || !m_documentItem.data()->isOpened() || number < 0) {
        return;
    }

    clearSelection();
    m_viewPort.pageNumber = number;
    refreshPage();
    Q_EMIT pageNumberChanged();
    checkBookmarksChanged();
}

void PageItem::refreshPage()
{
    clearSelection();
    if (uint(m_viewPort.pageNumber) < m_documentItem.data()->document()->pages()) {
        m_page = m_documentItem.data()->document()->page(m_viewPort.pageNumber);
    } else {
        m_page = nullptr;
    }

    Q_EMIT implicitWidthChanged();
    Q_EMIT implicitHeightChanged();

    m_redrawTimer->start();
}

bool PageItem::hasSelection() const
{
    return m_selectedArea && !m_selectedArea->isEmpty();
}

QPointF PageItem::selectionStart() const
{
    return QPointF(m_selectionStart.x() * width(), m_selectionStart.y() * height());
}

QPointF PageItem::selectionEnd() const
{
    return QPointF(m_selectionEnd.x() * width(), m_selectionEnd.y() * height());
}

bool PageItem::canCopySelection() const
{
    return hasSelection() && m_documentItem && m_documentItem->document()->isAllowed(Okular::AllowCopy);
}

bool PageItem::canHighlightSelection() const
{
    return hasSelection() && m_documentItem && m_documentItem->document()->isAllowed(Okular::AllowNotes) && m_documentItem->document()->supportsAnnotationSidecar();
}

bool PageItem::selectWordAt(qreal x, qreal y)
{
    if (!m_page || !m_documentItem || width() <= 0 || height() <= 0 || x < 0 || x > width() || y < 0 || y > height()) {
        return false;
    }

    if (!m_page->hasTextPage()) {
        m_documentItem->document()->requestTextPage(m_viewPort.pageNumber);
    }

    const QTransform rotation = pageRotation(m_page->rotation());
    const QPointF unrotated = rotation.inverted().map(QPointF(x / width(), y / height()));
    auto word = m_page->wordAt(Okular::NormalizedPoint(unrotated.x(), unrotated.y()));
    if (word) {
        word->transform(rotation);
    }
    if (!word || word->isEmpty() || m_page->text(word.get(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour).trimmed().isEmpty()) {
        return false;
    }

    clearSelection();
    const auto &first = word->first();
    const auto &last = word->last();
    m_selectionStart = QPointF(first.left, (first.top + first.bottom) / 2);
    m_selectionEnd = QPointF(last.right, (last.top + last.bottom) / 2);
    m_selectedArea = std::make_unique<Okular::RegularAreaRect>(*word);
    m_documentItem->document()->setPageTextSelection(m_viewPort.pageNumber, std::move(word), QColor(74, 144, 226, 120));
    Q_EMIT selectionChanged();
    return true;
}

void PageItem::moveSelectionHandle(bool start, qreal x, qreal y)
{
    if (!hasSelection() || !m_page || width() <= 0 || height() <= 0) {
        return;
    }

    const QPointF point(qBound(0.0, x / width(), 1.0), qBound(0.0, y / height(), 1.0));
    const QPointF first = start ? point : m_selectionStart;
    const QPointF last = start ? m_selectionEnd : point;
    if (first == last) {
        return;
    }

    const QTransform unrotate = pageRotation(m_page->rotation()).inverted();
    const QPointF unrotatedFirst = unrotate.map(first);
    const QPointF unrotatedLast = unrotate.map(last);
    Okular::TextSelection selection(Okular::NormalizedPoint(unrotatedFirst.x(), unrotatedFirst.y()), Okular::NormalizedPoint(unrotatedLast.x(), unrotatedLast.y()));
    auto area = m_page->textArea(selection);
    if (!area || area->isEmpty()) {
        return;
    }

    m_selectionStart = first;
    m_selectionEnd = last;
    m_selectedArea = std::make_unique<Okular::RegularAreaRect>(*area);
    m_documentItem->document()->setPageTextSelection(m_viewPort.pageNumber, std::move(area), QColor(74, 144, 226, 120));
    Q_EMIT selectionChanged();
}

void PageItem::clearSelection()
{
    if (!hasSelection()) {
        return;
    }

    m_selectedArea.reset();
    if (m_documentItem && m_documentItem->isOpened() && uint(m_viewPort.pageNumber) < m_documentItem->document()->pages()) {
        m_documentItem->document()->setPageTextSelection(m_viewPort.pageNumber, nullptr, QColor());
    }
    Q_EMIT selectionChanged();
}

void PageItem::copySelection()
{
    if (!canCopySelection() || !m_page) {
        return;
    }

    QString text = m_page->text(m_selectedArea.get(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
    if (text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    QGuiApplication::clipboard()->setText(text);
    clearSelection();
}

bool PageItem::highlightSelection()
{
    if (!canHighlightSelection() || !m_page) {
        return false;
    }

    auto *highlight = new Okular::HighlightAnnotation();
    highlight->setHighlightType(Okular::HighlightAnnotation::Highlight);

    Okular::NormalizedRect bounds = m_selectedArea->first();
    for (const Okular::NormalizedRect &rect : std::as_const(*m_selectedArea)) {
        bounds.left = qMin(bounds.left, rect.left);
        bounds.top = qMin(bounds.top, rect.top);
        bounds.right = qMax(bounds.right, rect.right);
        bounds.bottom = qMax(bounds.bottom, rect.bottom);

        Okular::HighlightAnnotation::Quad quad;
        quad.setCapStart(false);
        quad.setCapEnd(false);
        quad.setFeather(1.0);
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.bottom), 0);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.bottom), 1);
        quad.setPoint(Okular::NormalizedPoint(rect.right, rect.top), 2);
        quad.setPoint(Okular::NormalizedPoint(rect.left, rect.top), 3);
        highlight->highlightQuads().append(quad);
    }

    highlight->setBoundingRectangle(bounds);
    QString selectedText = m_page->text(m_selectedArea.get(), Okular::TextPage::CentralPixelTextAreaInclusionBehaviour);
    selectedText.replace(QStringLiteral("-\n"), QString()).replace(QLatin1Char('\n'), QLatin1Char(' '));
    highlight->setContents(selectedText.simplified());
    highlight->style().setColor(QColor(255, 235, 59));
    highlight->style().setOpacity(0.5);

    Okular::Document *document = m_documentItem->document();
    document->addPageAnnotation(m_viewPort.pageNumber, highlight);
    QString error;
    if (!document->saveAnnotationsToSidecar(&error)) {
        document->removePageAnnotation(m_viewPort.pageNumber, highlight);
        Q_EMIT m_documentItem->error(i18n("Could not save highlight: %1", error), -1);
        return false;
    }

    clearSelection();
    return true;
}

int PageItem::customImplicitWidth() const
{
    if (m_page) {
        return m_page->width();
    }
    return 0;
}

int PageItem::customImplicitHeight() const
{
    if (m_page) {
        return m_page->height();
    }
    return 0;
}

bool PageItem::isBookmarked()
{
    return m_bookmarked;
}

void PageItem::setBookmarked(bool bookmarked)
{
    if (!m_documentItem) {
        return;
    }

    if (bookmarked == m_bookmarked) {
        return;
    }

    if (bookmarked) {
        m_documentItem.data()->document()->bookmarkManager()->addBookmark(m_viewPort);
    } else {
        m_documentItem.data()->document()->bookmarkManager()->removeBookmark(m_viewPort.pageNumber);
    }
    m_bookmarked = bookmarked;
    Q_EMIT bookmarkedChanged();
}

QStringList PageItem::bookmarks() const
{
    QStringList list;
    const KBookmark::List pageMarks = m_documentItem.data()->document()->bookmarkManager()->bookmarks(m_viewPort.pageNumber);
    for (const KBookmark &bookmark : pageMarks) {
        list << bookmark.url().toString();
    }
    return list;
}

void PageItem::goToBookmark(const QString &bookmark)
{
    Okular::DocumentViewport viewPort(QUrl::fromUserInput(bookmark).fragment(QUrl::FullyDecoded));
    setPageNumber(viewPort.pageNumber);

    // Are we in a flickable?
    if (m_flickable) {
        // normalizedX is a proportion, so contentX will be the difference between document and viewport times normalizedX
        m_flickable.data()->setProperty("contentX", qMax((qreal)0, width() - m_flickable.data()->width()) * viewPort.rePos.normalizedX);

        m_flickable.data()->setProperty("contentY", qMax((qreal)0, height() - m_flickable.data()->height()) * viewPort.rePos.normalizedY);
    }
}

QPointF PageItem::bookmarkPosition(const QString &bookmark) const
{
    Okular::DocumentViewport viewPort(QUrl::fromUserInput(bookmark).fragment(QUrl::FullyDecoded));

    if (viewPort.pageNumber != m_viewPort.pageNumber) {
        return QPointF(-1, -1);
    }

    return QPointF(qMax((qreal)0, width() - m_flickable.data()->width()) * viewPort.rePos.normalizedX, qMax((qreal)0, height() - m_flickable.data()->height()) * viewPort.rePos.normalizedY);
}

void PageItem::setBookmarkAtPos(qreal x, qreal y)
{
    Okular::DocumentViewport viewPort(m_viewPort);
    viewPort.rePos.normalizedX = x;
    viewPort.rePos.normalizedY = y;

    m_documentItem.data()->document()->bookmarkManager()->addBookmark(viewPort);

    if (!m_bookmarked) {
        m_bookmarked = true;
        Q_EMIT bookmarkedChanged();
    }

    Q_EMIT bookmarksChanged();
}

void PageItem::removeBookmarkAtPos(qreal x, qreal y)
{
    Okular::DocumentViewport viewPort(m_viewPort);
    viewPort.rePos.enabled = true;
    viewPort.rePos.normalizedX = x;
    viewPort.rePos.normalizedY = y;

    m_documentItem.data()->document()->bookmarkManager()->addBookmark(viewPort);

    if (m_bookmarked && m_documentItem.data()->document()->bookmarkManager()->bookmarks(m_viewPort.pageNumber).count() == 0) {
        m_bookmarked = false;
        Q_EMIT bookmarkedChanged();
    }

    Q_EMIT bookmarksChanged();
}

void PageItem::removeBookmark(const QString &bookmark)
{
    m_documentItem.data()->document()->bookmarkManager()->removeBookmark(Okular::DocumentViewport(bookmark));
    Q_EMIT bookmarksChanged();
}

// Reimplemented
void PageItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    if (newGeometry.size().isEmpty()) {
        return;
    }

    bool changed = false;
    if (newGeometry.size() != oldGeometry.size()) {
        changed = true;
        m_redrawTimer->start();
    }

    QQuickItem::geometryChange(newGeometry, oldGeometry);

    if (changed) {
        // Why aren't they automatically emitted?
        Q_EMIT widthChanged();
        Q_EMIT heightChanged();
        if (hasSelection()) {
            Q_EMIT selectionChanged();
        }
    }
}

QSGNode *PageItem::updatePaintNode(QSGNode *node, QQuickItem::UpdatePaintNodeData * /*data*/)
{
    if (!window() || m_buffer.isNull()) {
        delete node;
        return nullptr;
    }
    QSGSimpleTextureNode *n = static_cast<QSGSimpleTextureNode *>(node);
    if (!n) {
        n = new QSGSimpleTextureNode();
        n->setOwnsTexture(true);
    }

    n->setTexture(window()->createTextureFromImage(m_buffer));
    n->setRect(boundingRect());
    return n;
}

void PageItem::requestPixmap()
{
    if (!m_documentItem || !m_page || !window() || width() <= 0 || height() < 0) {
        if (!m_buffer.isNull()) {
            m_buffer = QImage();
            update();
        }
        return;
    }

    Observer *observer = m_isThumbnail ? m_documentItem.data()->thumbnailObserver() : m_documentItem.data()->pageviewObserver();
    const int priority = m_isThumbnail ? THUMBNAILS_PRIO : PAGEVIEW_PRIO;

    const qreal dpr = window()->devicePixelRatio();

    // Here we want to request the pixmap for the page, but it may happen that the page
    // already has the pixmap, thus requestPixmaps would not trigger pageHasChanged
    // and we would not call paint. Always call paint, if we don't have a pixmap
    // it's a noop. Requesting a page that already has a pixmap is also
    // almost a noop.
    // Ideally we would do one or the other but for now this is good enough
    paint();
    {
        auto request = new Okular::PixmapRequest(observer, m_viewPort.pageNumber, width(), height(), dpr, priority, Okular::PixmapRequest::Asynchronous);
        request->setNormalizedRect(Okular::NormalizedRect(0, 0, 1, 1));
        const Okular::Document::PixmapRequestFlag prf = Okular::Document::NoOption;
        m_documentItem.data()->document()->requestPixmaps({request}, prf);
    }
}

void PageItem::paint()
{
    Observer *observer = m_isThumbnail ? m_documentItem.data()->thumbnailObserver() : m_documentItem.data()->pageviewObserver();
    const int flags = PagePainter::Accessibility | PagePainter::Highlights | PagePainter::TextSelection | PagePainter::Annotations;

    const qreal dpr = window()->devicePixelRatio();
    const QRect limits(QPoint(0, 0), QSize(width() * dpr, height() * dpr));
    QPixmap pix(limits.size());
    pix.setDevicePixelRatio(dpr);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, false);
    PagePainter::paintPageOnPainter(&p, m_page, observer, flags, width(), height(), limits);
    p.end();

    m_buffer = pix.toImage();

    update();
}

// Protected slots
void PageItem::pageHasChanged(int page, int flags)
{
    if (m_viewPort.pageNumber == page) {
        if (flags == Okular::DocumentObserver::BoundingBox) {
            // skip bounding box updates
            // kDebug() << "32" << m_page->boundingBox();
        } else if (flags == Okular::DocumentObserver::Pixmap) {
            // if pixmaps have updated, just repaint .. don't bother updating pixmaps AGAIN
            paint();
        } else {
            m_redrawTimer->start();
        }
    }
}

void PageItem::checkBookmarksChanged()
{
    if (!m_documentItem) {
        return;
    }

    bool newBookmarked = m_documentItem.data()->document()->bookmarkManager()->isBookmarked(m_viewPort.pageNumber);
    if (m_bookmarked != newBookmarked) {
        m_bookmarked = newBookmarked;
        Q_EMIT bookmarkedChanged();
    }

    // TODO: check the page
    Q_EMIT bookmarksChanged();
}

void PageItem::contentXChanged()
{
    if (!m_flickable || !m_flickable.data()->property("contentX").isValid()) {
        return;
    }

    m_viewPort.rePos.normalizedX = m_flickable.data()->property("contentX").toReal() / (width() - m_flickable.data()->width());
}

void PageItem::contentYChanged()
{
    if (!m_flickable || !m_flickable.data()->property("contentY").isValid()) {
        return;
    }

    m_viewPort.rePos.normalizedY = m_flickable.data()->property("contentY").toReal() / (height() - m_flickable.data()->height());
}

void PageItem::setIsThumbnail(bool thumbnail)
{
    if (thumbnail == m_isThumbnail) {
        return;
    }

    m_isThumbnail = thumbnail;

    /*
    m_redrawTimer->setInterval(thumbnail ? 0 : REDRAW_TIMEOUT);
    m_redrawTimer->setSingleShot(true);
    */
}

#include "moc_pageitem.cpp"
