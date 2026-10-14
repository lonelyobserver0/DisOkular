#pragma once

#include "DocumentView.h"

#include <QRectF>

#include <optional>

class QPdfBookmarkModel;
class QPdfDocument;
class QPdfSearchModel;
class QPdfView;
class QVBoxLayout;

class PdfView : public DocumentView
{
    Q_OBJECT

public:
    explicit PdfView(const QString &path, QWidget *parent = nullptr);

    bool load() override;

    void zoomIn() override;
    void zoomOut() override;
    void zoomReset() override;
    void fitWidth() override;

    int pageCount() const override;
    int currentPage() const override;
    void goToPage(int page) override;

    bool find(const QString &text, bool backward) override;

    QAbstractItemModel *outlineModel() const override;
    void activateOutline(const QModelIndex &index) override;
    QModelIndex currentOutlineIndex() const override;

    QString statusText() const override;

protected:
    // Carica un PDF qualsiasi mantenendo pagina e posizione correnti.
    bool loadPdf(const QString &file);

    // Il punto della pagina sotto `pos` (coordinate del viewport), se cade su una pagina.
    std::optional<QPair<int, QPointF>> pagePointAt(QPoint pos) const;
    // Evidenzia per un attimo dei riquadri (in punti) di una pagina.
    void flashRects(int page, const QList<QRectF> &rects);
    // Porta il punto (page, y in punti) a quota `viewportY` nella vista.
    void scrollToLocation(int page, qreal y, int viewportY);
    // Rettangolo in punti di una pagina → coordinate del viewport, allo scroll corrente.
    QRectF toViewport(int page, const QRectF &rect) const;

    QWidget *viewport() const;

    QVBoxLayout *m_layout;

private:
    qreal effectiveZoom() const;
    qreal pixelsPerPoint() const;
    // Ordinata in pixel dell'inizio di `page` nel layout di QPdfView.
    qreal pageTop(int page) const;
    // Pagina e ordinata (in punti, dall'alto) del punto del documento a quota `viewportY`.
    QPair<int, qreal> locationAt(int viewportY) const;
    // Ascissa in pixel (nel viewport) del bordo sinistro di `page`.
    qreal pageLeft(int page) const;
    void setZoom(qreal factor);
    void showSearchResult(int index);

    QPdfDocument *m_doc;
    QPdfView *m_view;
    QPdfSearchModel *m_search;
    QPdfBookmarkModel *m_bookmarks;
    int m_searchIndex = -1;
};
