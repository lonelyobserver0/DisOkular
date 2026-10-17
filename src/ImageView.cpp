#include "ImageView.h"

#include <QApplication>
#include <QClipboard>
#include <QFileInfo>
#include <QImageReader>
#include <QLocale>
#include <QMenu>
#include <QMovie>
#include <QPainter>
#include <QScrollBar>
#include <QSvgRenderer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

using namespace Qt::StringLiterals;

static constexpr qreal ZoomStep = 1.25;
static constexpr qreal ZoomMin = 0.02;
static constexpr qreal ZoomMax = 64.0;
static constexpr int FitMargin = 16;
// Sotto questa area la riduzione si fa una volta con buon filtro e si riusa; sopra si disegna al volo.
static constexpr qint64 MaxCachePixels = 40'000'000;
// Il limite predefinito di Qt (256 MB decodificati) rifiuta le foto più grandi.
static constexpr int AllocationLimitMb = 1024;

// ── ImageCanvas ──────────────────────────────────────────────────────────────

ImageCanvas::ImageCanvas(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setContextMenuPolicy(Qt::CustomContextMenu);
    horizontalScrollBar()->setSingleStep(40);
    verticalScrollBar()->setSingleStep(40);
}

bool ImageCanvas::hasContent() const
{
    return m_svg ? m_svg->isValid() : !m_image.isNull();
}

QSize ImageCanvas::naturalSize() const
{
    if (m_svg)
        return m_svg->defaultSize();
    return m_image.size();
}

void ImageCanvas::setImage(const QImage &image, bool keepView)
{
    const bool sameSize = keepView && image.size() == m_lastSize;
    m_svg = nullptr;
    m_image = image;
    m_lastSize = image.size();
    m_scaledCache = QImage();
    if (!sameSize)
        m_fit = true;
    updateScrollBars();
    viewport()->update();
    if (!sameSize)
        emit zoomChanged();
}

void ImageCanvas::setSvg(QSvgRenderer *svg, bool keepView)
{
    const bool sameSize = keepView && svg->defaultSize() == m_lastSize;
    m_image = QImage();
    m_scaledCache = QImage();
    m_svg = svg;
    m_lastSize = svg->defaultSize();
    if (!sameSize)
        m_fit = true;
    updateScrollBars();
    viewport()->update();
    emit zoomChanged();
}

qreal ImageCanvas::scale() const
{
    // Raster: al 100% un pixel dell'immagine per pixel del dispositivo, anche su schermi HiDPI.
    // SVG: le unità sono già logiche.
    return m_svg ? zoom() : zoom() / devicePixelRatioF();
}

qreal ImageCanvas::fitZoom() const
{
    const QSize n = naturalSize();
    if (n.isEmpty())
        return 1.0;
    const qreal unit = m_svg ? 1.0 : 1.0 / devicePixelRatioF();
    const qreal availW = qMax(1, viewport()->width() - 2 * FitMargin);
    const qreal availH = qMax(1, viewport()->height() - 2 * FitMargin);
    const qreal f = qMin(availW / (n.width() * unit), availH / (n.height() * unit));
    // Le foto piccole non si ingrandiscono (diventerebbero sfocate); i vettoriali sì.
    return m_svg ? f : qMin(f, 1.0);
}

qreal ImageCanvas::zoom() const
{
    return m_fit ? fitZoom() : m_zoom;
}

QSizeF ImageCanvas::scaledSize() const
{
    return QSizeF(naturalSize()) * scale();
}

QRectF ImageCanvas::imageRect() const
{
    const QSizeF s = scaledSize();
    const QSize v = viewport()->size();
    // Più piccola della vista: centrata. Più grande: parte dallo scorrimento.
    const qreal x = s.width() < v.width() ? (v.width() - s.width()) / 2 : -horizontalScrollBar()->value();
    const qreal y = s.height() < v.height() ? (v.height() - s.height()) / 2 : -verticalScrollBar()->value();
    return QRectF(QPointF(x, y), s);
}

void ImageCanvas::updateScrollBars()
{
    const QSizeF s = scaledSize();
    const QSize v = viewport()->size();
    horizontalScrollBar()->setRange(0, qMax(0, int(std::ceil(s.width() - v.width()))));
    verticalScrollBar()->setRange(0, qMax(0, int(std::ceil(s.height() - v.height()))));
    horizontalScrollBar()->setPageStep(v.width());
    verticalScrollBar()->setPageStep(v.height());

    const bool scrollable = horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0;
    if (!m_dragging)
        viewport()->setCursor(scrollable ? Qt::OpenHandCursor : Qt::ArrowCursor);
}

void ImageCanvas::setFit(bool fit)
{
    m_fit = fit;
    m_scaledCache = QImage();
    updateScrollBars();
    viewport()->update();
    emit zoomChanged();
}

void ImageCanvas::setZoom(qreal z, QPointF anchor)
{
    if (!hasContent())
        return;
    if (anchor.x() < 0)
        anchor = QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0);

    // Il punto dell'immagine sotto `anchor` deve restare lì dopo lo zoom.
    const QPointF imagePoint = (anchor - imageRect().topLeft()) / scale();

    m_fit = false;
    m_zoom = qBound(ZoomMin, z, ZoomMax);
    m_scaledCache = QImage();
    updateScrollBars();

    horizontalScrollBar()->setValue(qRound(imagePoint.x() * scale() - anchor.x()));
    verticalScrollBar()->setValue(qRound(imagePoint.y() * scale() - anchor.y()));
    viewport()->update();
    emit zoomChanged();
}

QImage ImageCanvas::currentImage() const
{
    if (!m_svg)
        return m_image;
    QImage img(m_svg->defaultSize(), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    m_svg->render(&p);
    return img;
}

void ImageCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    const QPalette &pal = palette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    p.fillRect(viewport()->rect(), dark ? pal.color(QPalette::Window).darker(130) : pal.color(QPalette::Window).darker(108));
    if (!hasContent())
        return;

    const QRectF r = imageRect();

    // Scacchiera sotto le zone trasparenti, ancorata all'immagine così non "scivola" scorrendo.
    if (m_svg || m_image.hasAlphaChannel()) {
        static const QPixmap checker = [] {
            QPixmap pm(16, 16);
            pm.fill(QColor(0xcc, 0xcc, 0xcc));
            QPainter cp(&pm);
            cp.fillRect(0, 0, 8, 8, QColor(0xf2, 0xf2, 0xf2));
            cp.fillRect(8, 8, 8, 8, QColor(0xf2, 0xf2, 0xf2));
            return pm;
        }();
        QBrush brush(checker);
        brush.setTransform(QTransform::fromTranslate(r.x(), r.y()));
        p.fillRect(r, brush);
    }

    if (m_svg) {
        p.setRenderHint(QPainter::Antialiasing);
        m_svg->render(&p, r);
        return;
    }

    const qreal s = scale();
    const QSize target = r.size().toSize();
    if (s < 1.0 && qint64(target.width()) * target.height() <= MaxCachePixels) {
        // Riduzione: filtro di qualità calcolato una volta per livello di zoom.
        if (m_scaledCache.size() != target)
            m_scaledCache = m_image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.drawImage(r.topLeft(), m_scaledCache);
        return;
    }

    // Ingrandimento: si disegna solo la parte visibile. Oltre il 300% pixel netti, utili per la pixel art.
    const QRectF visible = r.intersected(QRectF(viewport()->rect()));
    if (visible.isEmpty())
        return;
    const QRectF source((visible.x() - r.x()) / s, (visible.y() - r.y()) / s, visible.width() / s, visible.height() / s);
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom() < 3.0);
    p.drawImage(visible, m_image, source);
}

void ImageCanvas::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollBars();
    if (m_fit) {
        m_scaledCache = QImage();
        emit zoomChanged();
    }
}

void ImageCanvas::scrollContentsBy(int, int)
{
    viewport()->update();
}

void ImageCanvas::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        // 120 = uno scatto di rotella; i touchpad mandano valori più piccoli e frequenti.
        const qreal factor = std::pow(ZoomStep, event->angleDelta().y() / 120.0);
        setZoom(zoom() * factor, event->position());
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void ImageCanvas::mousePressEvent(QMouseEvent *event)
{
    const bool scrollable = horizontalScrollBar()->maximum() > 0 || verticalScrollBar()->maximum() > 0;
    if (event->button() == Qt::LeftButton && scrollable) {
        m_dragging = true;
        m_dragOrigin = event->position().toPoint();
        m_dragScroll = QPoint(horizontalScrollBar()->value(), verticalScrollBar()->value());
        viewport()->setCursor(Qt::ClosedHandCursor);
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void ImageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPoint delta = event->position().toPoint() - m_dragOrigin;
        horizontalScrollBar()->setValue(m_dragScroll.x() - delta.x());
        verticalScrollBar()->setValue(m_dragScroll.y() - delta.y());
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        updateScrollBars();   // ripristina il cursore giusto
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Doppio clic: dalla vista adattata al 100% nel punto cliccato, e ritorno.
    if (event->button() == Qt::LeftButton) {
        if (m_fit)
            setZoom(1.0, event->position());
        else
            setFit(true);
        return;
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void ImageCanvas::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::NoModifier) {
        emit togglePlayRequested();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

// ── ImageView ────────────────────────────────────────────────────────────────

ImageView::ImageView(const QString &path, QWidget *parent)
    : DocumentView(path, parent)
    , m_canvas(new ImageCanvas(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_canvas);
    setFocusProxy(m_canvas);

    connect(m_canvas, &ImageCanvas::zoomChanged, this, &DocumentView::statusChanged);
    connect(m_canvas, &ImageCanvas::togglePlayRequested, this, &ImageView::togglePlay);
    connect(m_canvas, &QWidget::customContextMenuRequested, this, &ImageView::showContextMenu);
}

void ImageView::resetAnimation()
{
    // La superficie non si svuota: deve ricordare la dimensione per conservare zoom e posizione.
    delete m_movie;
    m_movie = nullptr;
    delete m_svg;
    m_svg = nullptr;
}

bool ImageView::load()
{
    QImageReader::setAllocationLimit(AllocationLimitMb);

    const QSize previousSize = m_size;
    const QString suffix = QFileInfo(m_path).suffix().toLower();

    if (suffix == u"svg" || suffix == u"svgz") {
        auto *svg = new QSvgRenderer(m_path, this);
        if (!svg->isValid()) {
            delete svg;
            m_error = tr("File SVG non valido");
            return false;
        }
        resetAnimation();
        m_svg = svg;
        m_format = u"SVG"_s;
        m_size = svg->defaultSize();
        // Gli SVG animati (SMIL) chiedono di essere ridisegnati.
        connect(svg, &QSvgRenderer::repaintNeeded, m_canvas->viewport(), qOverload<>(&QWidget::update));
        m_canvas->setSvg(svg, m_size == previousSize);
    } else {
        QImageReader reader(m_path);
        reader.setAutoTransform(true);   // orientamento EXIF delle foto
        if (!reader.canRead()) {
            m_error = reader.errorString();
            return false;
        }
        m_format = QString::fromLatin1(reader.format()).toUpper();

        // GIF e WebP possono avere un solo fotogramma: allora sono immagini come le altre.
        if (reader.supportsAnimation() && reader.imageCount() != 1) {
            auto *movie = new QMovie(m_path, QByteArray(), this);
            if (!movie->isValid() || !movie->jumpToFrame(0)) {
                delete movie;
                m_error = tr("Animazione non leggibile");
                return false;
            }
            resetAnimation();
            m_movie = movie;
            m_size = movie->currentImage().size();
            m_canvas->setImage(movie->currentImage(), m_size == previousSize);
            connect(movie, &QMovie::frameChanged, this, [this] {
                m_canvas->setImage(m_movie->currentImage(), true);
                emit statusChanged();
            });
            movie->start();
        } else {
            const QImage image = reader.read();
            if (image.isNull()) {
                m_error = reader.errorString();
                return false;
            }
            resetAnimation();
            m_size = image.size();
            m_canvas->setImage(image, m_size == previousSize);
        }
    }

    m_error.clear();
    emit statusChanged();
    return true;
}

void ImageView::zoomIn()    { m_canvas->setZoom(m_canvas->zoom() * ZoomStep); }
void ImageView::zoomOut()   { m_canvas->setZoom(m_canvas->zoom() / ZoomStep); }
void ImageView::zoomReset() { m_canvas->setZoom(1.0); }
void ImageView::fitWidth()  { m_canvas->setFit(true); }

void ImageView::togglePlay()
{
    if (!m_movie)
        return;
    m_movie->setPaused(m_movie->state() == QMovie::Running);
    emit statusChanged();
}

void ImageView::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme("edit-copy"), tr("Copia immagine"), this, [this] {
        QApplication::clipboard()->setImage(m_canvas->currentImage());
        emit notice(tr("Immagine copiata negli appunti"));
    });
    menu.addSeparator();
    QAction *fit = menu.addAction(QIcon::fromTheme("zoom-fit-best"), tr("Adatta alla finestra"), this, &ImageView::fitWidth);
    fit->setEnabled(!m_canvas->isFit());
    menu.addAction(QIcon::fromTheme("zoom-original"), tr("Dimensione reale"), this, &ImageView::zoomReset);
    if (m_movie) {
        menu.addSeparator();
        const bool running = m_movie->state() == QMovie::Running;
        menu.addAction(QIcon::fromTheme(running ? "media-playback-pause" : "media-playback-start"),
                       running ? tr("Pausa (Spazio)") : tr("Riproduci (Spazio)"), this, &ImageView::togglePlay);
    }
    menu.exec(m_canvas->viewport()->mapToGlobal(pos));
}

QString ImageView::statusText() const
{
    if (!m_error.isEmpty())
        return m_error;
    const QLocale locale(QLocale::Italian);
    QString s = u"%1 · %2×%3 · %4 · zoom %5%"_s
                    .arg(m_format)
                    .arg(m_size.width())
                    .arg(m_size.height())
                    .arg(locale.formattedDataSize(QFileInfo(m_path).size()))
                    .arg(qRound(m_canvas->zoom() * 100));
    if (m_canvas->isFit())
        s += tr(" (adatta)");
    if (m_movie) {
        const int frames = m_movie->frameCount();
        s += frames > 0 ? tr(" · fotogramma %1 di %2").arg(m_movie->currentFrameNumber() + 1).arg(frames)
                        : tr(" · fotogramma %1").arg(m_movie->currentFrameNumber() + 1);
        if (m_movie->state() == QMovie::Paused)
            s += tr(" (in pausa — Spazio per riprendere)");
    }
    return s;
}
