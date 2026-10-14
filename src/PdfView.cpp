#include "PdfView.h"

#include <QPainter>
#include <QPdfBookmarkModel>
#include <QVariantAnimation>
#include <QPdfDocument>
#include <QPdfPageNavigator>
#include <QPdfSearchModel>
#include <QPdfView>
#include <QScreen>
#include <QScrollBar>
#include <QVBoxLayout>

#include <functional>

static constexpr qreal ZoomStep = 1.2;
static constexpr qreal ZoomMin = 0.1;
static constexpr qreal ZoomMax = 10.0;

static_assert(DocumentView::OutlinePageRole == int(QPdfBookmarkModel::Role::Page),
              "il delegato dell'indice legge la pagina da OutlinePageRole");

PdfView::PdfView(const QString &path, QWidget *parent)
    : DocumentView(path, parent)
    , m_layout(new QVBoxLayout(this))
    , m_doc(new QPdfDocument(this))
    , m_view(new QPdfView(this))
    , m_search(new QPdfSearchModel(this))
    , m_bookmarks(new QPdfBookmarkModel(this))
{
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(m_view);

    m_view->setDocument(m_doc);
    m_view->setPageMode(QPdfView::PageMode::MultiPage);
    m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    m_view->setPageSpacing(12);
    m_view->setDocumentMargins(QMargins(12, 12, 12, 12));

    m_search->setDocument(m_doc);
    m_bookmarks->setDocument(m_doc);
    m_view->setSearchModel(m_search);

    connect(m_view->pageNavigator(), &QPdfPageNavigator::currentPageChanged, this, &DocumentView::statusChanged);
    connect(m_view, &QPdfView::zoomFactorChanged, this, &DocumentView::statusChanged);
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, this, &DocumentView::positionChanged);
    connect(m_view, &QPdfView::zoomModeChanged, this, &DocumentView::statusChanged);

    // I risultati arrivano in modo asincrono: appena c'è il primo, mostralo.
    connect(m_search, &QPdfSearchModel::countChanged, this, [this] {
        if (m_searchIndex < 0 && m_search->count() > 0)
            showSearchResult(0);
    });
}

bool PdfView::load()
{
    return loadPdf(m_path);
}

bool PdfView::loadPdf(const QString &file)
{
    const bool reloading = m_doc->status() == QPdfDocument::Status::Ready;
    const int page = currentPage();
    const int vscroll = m_view->verticalScrollBar()->value();
    const int hscroll = m_view->horizontalScrollBar()->value();

    m_doc->close();
    const QPdfDocument::Error err = m_doc->load(file);
    if (err != QPdfDocument::Error::None) {
        switch (err) {
        case QPdfDocument::Error::FileNotFound:        m_error = tr("File non trovato"); break;
        case QPdfDocument::Error::InvalidFileFormat:   m_error = tr("Formato PDF non valido"); break;
        case QPdfDocument::Error::IncorrectPassword:   m_error = tr("PDF protetto da password"); break;
        case QPdfDocument::Error::UnsupportedSecurityScheme: m_error = tr("Schema di protezione non supportato"); break;
        default:                                       m_error = tr("Impossibile aprire il PDF"); break;
        }
        emit statusChanged();
        return false;
    }

    m_error.clear();
    if (reloading) {
        // Il layout delle pagine viene ricalcolato al prossimo giro dell'event loop.
        QTimer::singleShot(0, this, [this, page, vscroll, hscroll] {
            if (page < pageCount()) {
                m_view->verticalScrollBar()->setValue(vscroll);
                m_view->horizontalScrollBar()->setValue(hscroll);
            }
        });
    }
    emit statusChanged();
    return true;
}

qreal PdfView::effectiveZoom() const
{
    if (m_view->zoomMode() == QPdfView::ZoomMode::Custom || m_doc->pageCount() == 0)
        return m_view->zoomFactor();

    // In modalità "adatta" QPdfView non espone il fattore reale: lo ricaviamo.
    const QSizeF pt = m_doc->pagePointSize(currentPage());
    const qreal dpiScale = m_view->logicalDpiX() / 72.0;
    const QMargins mg = m_view->documentMargins();
    const qreal availW = m_view->viewport()->width() - mg.left() - mg.right();
    const qreal byWidth = availW / (pt.width() * dpiScale);
    if (m_view->zoomMode() == QPdfView::ZoomMode::FitToWidth)
        return byWidth;
    const qreal availH = m_view->viewport()->height() - mg.top() - mg.bottom();
    return qMin(byWidth, availH / (pt.height() * dpiScale));
}

void PdfView::setZoom(qreal factor)
{
    m_view->setZoomMode(QPdfView::ZoomMode::Custom);
    m_view->setZoomFactor(qBound(ZoomMin, factor, ZoomMax));
}

void PdfView::zoomIn()    { setZoom(effectiveZoom() * ZoomStep); }
void PdfView::zoomOut()   { setZoom(effectiveZoom() / ZoomStep); }
void PdfView::zoomReset() { setZoom(1.0); }
void PdfView::fitWidth()  { m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth); }

int PdfView::pageCount() const
{
    return m_doc->pageCount();
}

int PdfView::currentPage() const
{
    return m_view->pageNavigator()->currentPage();
}

void PdfView::goToPage(int page)
{
    if (page >= 0 && page < pageCount())
        m_view->pageNavigator()->jump(page, QPointF(), 0);
}

bool PdfView::find(const QString &text, bool backward)
{
    if (text != m_search->searchString()) {
        m_searchIndex = -1;
        m_search->setSearchString(text);
        // Se i risultati non sono ancora pronti ci pensa countChanged.
        if (m_search->count() > 0)
            showSearchResult(0);
        return !text.isEmpty();
    }

    const int n = m_search->count();
    if (n == 0)
        return false;
    showSearchResult(backward ? (m_searchIndex - 1 + n) % n : (m_searchIndex + 1) % n);
    return true;
}

QAbstractItemModel *PdfView::outlineModel() const
{
    return m_bookmarks;
}

void PdfView::activateOutline(const QModelIndex &index)
{
    const int page = index.data(int(QPdfBookmarkModel::Role::Page)).toInt();
    const QPointF location = index.data(int(QPdfBookmarkModel::Role::Location)).toPointF();
    // Il titolo finisce poco sotto il bordo, non incollato. Lo zoom scelto non cambia.
    scrollToLocation(page, location.y(), 16);
}

// QPdfPageNavigator aggiorna la posizione solo nei salti (non nello scroll) e QPdfView,
// saltando, ignora la y e mostra l'inizio della pagina. Per entrambe le cose ricostruiamo
// il layout di QPdfView: margine, pagine impilate, spaziatura.

qreal PdfView::pixelsPerPoint() const
{
    return effectiveZoom() * m_view->logicalDpiY() / 72.0;
}

qreal PdfView::pageTop(int page) const
{
    const qreal ppp = pixelsPerPoint();
    qreal top = m_view->documentMargins().top();
    for (int i = 0; i < page; ++i)
        top += m_doc->pagePointSize(i).height() * ppp + m_view->pageSpacing();
    return top;
}

QPair<int, qreal> PdfView::locationAt(int viewportY) const
{
    const qreal ppp = pixelsPerPoint();
    qreal y = m_view->verticalScrollBar()->value() + viewportY - m_view->documentMargins().top();
    const int n = m_doc->pageCount();
    for (int page = 0; page < n; ++page) {
        const qreal h = m_doc->pagePointSize(page).height() * ppp;
        if (y < h + m_view->pageSpacing() || page == n - 1)
            return {page, qMax<qreal>(0, y) / ppp};
        y -= h + m_view->pageSpacing();
    }
    return {0, 0};
}

qreal PdfView::pageLeft(int page) const
{
    // Misurato: le pagine più strette della vista sono centrate; quelle più larghe
    // partono dal margine sinistro, centrate rispetto alla pagina più larga.
    const qreal ppp = pixelsPerPoint();
    qreal maxWidth = 0;
    for (int i = 0; i < m_doc->pageCount(); ++i)
        maxWidth = qMax(maxWidth, m_doc->pagePointSize(i).width() * ppp);
    const qreal width = m_doc->pagePointSize(page).width() * ppp;
    const QMargins mg = m_view->documentMargins();
    const int viewportWidth = m_view->viewport()->width();
    if (maxWidth + mg.left() + mg.right() <= viewportWidth)
        return (viewportWidth - width) / 2;
    return mg.left() + (maxWidth - width) / 2 - m_view->horizontalScrollBar()->value();
}

QRectF PdfView::toViewport(int page, const QRectF &rect) const
{
    const qreal ppp = pixelsPerPoint();
    return QRectF(pageLeft(page) + rect.x() * ppp,
                  pageTop(page) - m_view->verticalScrollBar()->value() + rect.y() * ppp,
                  rect.width() * ppp, rect.height() * ppp);
}

std::optional<QPair<int, QPointF>> PdfView::pagePointAt(QPoint pos) const
{
    if (m_doc->pageCount() == 0)
        return std::nullopt;
    const auto [page, y] = locationAt(pos.y());
    const QSizeF size = m_doc->pagePointSize(page);
    const qreal x = (pos.x() - pageLeft(page)) / pixelsPerPoint();
    if (x < 0 || x > size.width() || y > size.height())
        return std::nullopt;   // sul margine o nello spazio tra le pagine
    return QPair<int, QPointF>(page, QPointF(x, y));
}

QWidget *PdfView::viewport() const
{
    return m_view->viewport();
}

namespace {

// Riquadri disegnati sopra le pagine; seguono lo scroll e svaniscono da soli.
class FlashOverlay : public QWidget
{
public:
    FlashOverlay(PdfView *view, QWidget *viewport, int page, const QList<QRectF> &rects,
                 std::function<QRectF(int, const QRectF &)> map)
        : QWidget(viewport), m_page(page), m_rects(rects), m_map(std::move(map))
    {
        Q_UNUSED(view)
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(viewport->rect());
        show();
        raise();

        auto *anim = new QVariantAnimation(this);
        anim->setStartValue(1.0);
        anim->setEndValue(0.0);
        anim->setDuration(1800);
        anim->setEasingCurve(QEasingCurve::InQuad);   // resta visibile, poi sfuma
        connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_opacity = v.toReal();
            update();
        });
        connect(anim, &QVariantAnimation::finished, this, &QObject::deleteLater);
        anim->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (QWidget *p = parentWidget(); p && geometry() != p->rect())
            setGeometry(p->rect());
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor accent = palette().color(QPalette::Highlight);
        QColor fill = accent;
        fill.setAlphaF(0.25 * m_opacity);
        accent.setAlphaF(0.9 * m_opacity);
        painter.setPen(QPen(accent, 2));
        painter.setBrush(fill);
        for (const QRectF &r : std::as_const(m_rects))
            painter.drawRoundedRect(m_map(m_page, r).adjusted(-4, -3, 4, 3), 4, 4);
    }

private:
    int m_page;
    QList<QRectF> m_rects;
    std::function<QRectF(int, const QRectF &)> m_map;
    qreal m_opacity = 1.0;
};

} // namespace

void PdfView::flashRects(int page, const QList<QRectF> &rects)
{
    auto *overlay = new FlashOverlay(this, m_view->viewport(), page, rects,
                                     [this](int p, const QRectF &r) { return toViewport(p, r); });
    // Lo scroll sposta le pagine sotto il riquadro: ridisegna.
    connect(m_view->verticalScrollBar(), &QScrollBar::valueChanged, overlay, qOverload<>(&QWidget::update));
    connect(m_view->horizontalScrollBar(), &QScrollBar::valueChanged, overlay, qOverload<>(&QWidget::update));
}

void PdfView::scrollToLocation(int page, qreal y, int viewportY)
{
    const qreal target = pageTop(page) + y * pixelsPerPoint() - viewportY;
    m_view->verticalScrollBar()->setValue(qRound(target));
}

QModelIndex PdfView::currentOutlineIndex() const
{
    // La sezione corrente è l'ultima il cui titolo sta sopra il primo terzo della vista:
    // un titolo appena comparso in alto conta già come "dentro".
    // In fondo al documento l'ultima sezione non sale fino in cima: conta il bordo inferiore.
    const QScrollBar *vbar = m_view->verticalScrollBar();
    const bool atEnd = vbar->maximum() > 0 && vbar->value() >= vbar->maximum();
    const int vh = m_view->viewport()->height();
    const auto [curPage, curY] = locationAt(atEnd ? vh - 1 : vh / 3);
    QModelIndex best;
    std::function<void(const QModelIndex &)> visit = [&](const QModelIndex &parent) {
        for (int row = 0; row < m_bookmarks->rowCount(parent); ++row) {
            const QModelIndex idx = m_bookmarks->index(row, 0, parent);
            const int page = idx.data(int(QPdfBookmarkModel::Role::Page)).toInt();
            const qreal y = idx.data(int(QPdfBookmarkModel::Role::Location)).toPointF().y();
            if (page > curPage || (page == curPage && y > curY))
                return;
            best = idx;
            visit(idx);
        }
    };
    visit({});
    return best;
}

void PdfView::showSearchResult(int index)
{
    m_searchIndex = index;
    m_view->setCurrentSearchResultIndex(index);
    const QPdfLink link = m_search->resultAtIndex(index);
    // Il risultato a un terzo della vista, dove l'occhio lo cerca.
    scrollToLocation(link.page(), link.location().y(), m_view->viewport()->height() / 3);
    emit statusChanged();
}

QString PdfView::statusText() const
{
    if (!m_error.isEmpty())
        return m_error;
    QString s = tr("Pagina %1 di %2 — zoom %3%")
                    .arg(currentPage() + 1)
                    .arg(pageCount())
                    .arg(qRound(effectiveZoom() * 100));
    if (m_view->zoomMode() == QPdfView::ZoomMode::FitToWidth)
        s += tr(" (adatta larghezza)");
    if (m_searchIndex >= 0)
        s += tr(" — risultato %1 di %2").arg(m_searchIndex + 1).arg(m_search->count());
    return s;
}
