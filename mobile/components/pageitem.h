/*
    SPDX-FileCopyrightText: 2012 Marco Martin <mart@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef QPAGEITEM_H
#define QPAGEITEM_H

#include <QImage>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <qqmlregistration.h>

#include <memory>

#include <core/document.h>
#include <core/textpage.h>
#include <core/view.h>

class QTimer;

class DocumentItem;

namespace Okular
{
class Document;
class Page;
class RegularAreaRect;
}

class PageItem : public QQuickItem, public Okular::View
{
    Q_OBJECT
    QML_ELEMENT

    /**
     * If this page is in a Flickable, assign it in this property, to make goToBookmark work
     */
    Q_PROPERTY(QQuickItem *flickable READ flickable WRITE setFlickable NOTIFY flickableChanged)

    /**
     * The document this page belongs to
     */
    Q_PROPERTY(DocumentItem *document READ document WRITE setDocument NOTIFY documentChanged)

    /**
     * The currently displayed page
     */
    Q_PROPERTY(int pageNumber READ pageNumber WRITE setPageNumber NOTIFY pageNumberChanged)

    /**
     * "Natural" width of the page
     */
    Q_PROPERTY(int implicitWidth READ customImplicitWidth NOTIFY implicitWidthChanged)

    /**
     * "Natural" height of the page
     */
    Q_PROPERTY(int implicitHeight READ customImplicitHeight NOTIFY implicitHeightChanged)

    /**
     * True if the page contains at least a bookmark.
     * Writing true to tis property idds a bookmark at the beginning of the page (if needed).
     * Writing false, all bookmarks for this page will be removed
     */
    Q_PROPERTY(bool bookmarked READ isBookmarked WRITE setBookmarked NOTIFY bookmarkedChanged)

    /**
     * list of bookmarks urls valid on this page
     */
    Q_PROPERTY(QStringList bookmarks READ bookmarks NOTIFY bookmarksChanged)

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(QPointF selectionStart READ selectionStart NOTIFY selectionChanged)
    Q_PROPERTY(QPointF selectionEnd READ selectionEnd NOTIFY selectionChanged)
    Q_PROPERTY(bool canCopySelection READ canCopySelection NOTIFY selectionChanged)
    Q_PROPERTY(bool canHighlightSelection READ canHighlightSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedWord READ selectedWord NOTIFY selectionChanged)

public:
    explicit PageItem(QQuickItem *parent = nullptr);
    ~PageItem() override;

    void setFlickable(QQuickItem *flickable);
    QQuickItem *flickable() const;

    int customImplicitWidth() const;
    int customImplicitHeight() const;

    DocumentItem *document() const;
    void setDocument(DocumentItem *doc);

    int pageNumber() const;
    void setPageNumber(int number);

    bool isBookmarked();
    void setBookmarked(bool bookmarked);

    QStringList bookmarks() const;
    void requestPixmap();

    /**
     * loads a page bookmark and tries to ensure the bookmarked position is visible
     * @param bookmark Url for the bookmark
     */
    Q_INVOKABLE void goToBookmark(const QString &bookmark);

    /**
     * Returns the position in the page for a bookmark
     * QPointF(-1,-1) if doesn't belong to this page
     *
     * @param bookmark Url for the bookmark
     */
    Q_INVOKABLE QPointF bookmarkPosition(const QString &bookmark) const;

    /**
     * Add a new bookmark ar a given position of the current page
     */
    Q_INVOKABLE void setBookmarkAtPos(qreal x, qreal y);

    /**
     * Remove a bookmark ar a given position of the current page (if present)
     */
    Q_INVOKABLE void removeBookmarkAtPos(qreal x, qreal y);

    /**
     * Remove a bookmark at a given position, if any
     */
    Q_INVOKABLE void removeBookmark(const QString &bookmark);

    bool hasSelection() const;
    QPointF selectionStart() const;
    QPointF selectionEnd() const;
    bool canCopySelection() const;
    bool canHighlightSelection() const;
    QString selectedWord() const;

    Q_INVOKABLE bool selectWordAt(qreal x, qreal y);
    Q_INVOKABLE void moveSelectionHandle(bool start, qreal x, qreal y);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void copySelection();
    Q_INVOKABLE bool highlightSelection();

    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

    QSGNode *updatePaintNode(QSGNode *, QQuickItem::UpdatePaintNodeData *) override;

Q_SIGNALS:
    void flickableChanged();
    void documentChanged();
    void pageNumberChanged();
    void bookmarkedChanged();
    void bookmarksChanged();
    void selectionChanged();

protected:
    void setIsThumbnail(bool thumbnail);

private Q_SLOTS:
    void pageHasChanged(int page, int flags);
    void checkBookmarksChanged();
    void contentXChanged();
    void contentYChanged();

private:
    void paint();
    void refreshPage();
    std::unique_ptr<Okular::RegularAreaRect> wordNear(const QPointF &point, const Okular::TextEntity::List &entities) const;

    const Okular::Page *m_page;
    bool m_bookmarked;
    bool m_isThumbnail;
    QPointer<DocumentItem> m_documentItem;
    QTimer *m_redrawTimer;
    QPointer<QQuickItem> m_flickable;
    Okular::DocumentViewport m_viewPort;
    QImage m_buffer;
    std::unique_ptr<Okular::RegularAreaRect> m_selectedArea;
    Okular::TextEntity::List m_selectionText;
    QPointF m_selectionStart;
    QPointF m_selectionEnd;
};

#endif
