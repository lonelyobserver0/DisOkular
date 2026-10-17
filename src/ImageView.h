#pragma once

#include "DocumentView.h"

#include <QAbstractScrollArea>
#include <QImage>
#include <QPointer>

class QMovie;
class QSvgRenderer;

// Superficie che mostra un'immagine raster o un SVG con zoom, scorrimento e trascinamento.
class ImageCanvas : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit ImageCanvas(QWidget *parent = nullptr);

    // keepView: stessa dimensione di prima (ricarica, fotogramma successivo): zoom e posizione restano.
    void setImage(const QImage &image, bool keepView);
    void setSvg(QSvgRenderer *svg, bool keepView);

    // Dimensione naturale, in pixel dell'immagine (o unità SVG).
    QSize naturalSize() const;
    qreal zoom() const;
    bool isFit() const { return m_fit; }

    void setFit(bool fit);
    // Zoom con il punto `anchor` (coordinate del viewport) fermo sotto il cursore.
    void setZoom(qreal zoom, QPointF anchor = QPointF(-1, -1));

    // L'immagine com'è ora, per la clipboard (gli SVG alla dimensione naturale).
    QImage currentImage() const;

signals:
    void zoomChanged();
    void togglePlayRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    // Pixel del viewport per unità dell'immagine: lo zoom al 100% è un pixel per pixel del dispositivo.
    qreal scale() const;
    qreal fitZoom() const;
    QSizeF scaledSize() const;
    QRectF imageRect() const;
    void updateScrollBars();
    bool hasContent() const;

    QImage m_image;
    QImage m_scaledCache;       // riduzione di alta qualità, ricalcolata solo al cambio di zoom
    QPointer<QSvgRenderer> m_svg;
    QSize m_lastSize;           // dimensione dell'ultimo contenuto: stessa dimensione = stessa vista
    qreal m_zoom = 1.0;
    bool m_fit = true;
    bool m_dragging = false;
    QPoint m_dragOrigin;
    QPoint m_dragScroll;
};

// Visualizzatore di immagini: raster (anche animate) e SVG.
class ImageView : public DocumentView
{
    Q_OBJECT

public:
    explicit ImageView(const QString &path, QWidget *parent = nullptr);

    bool load() override;

    void zoomIn() override;
    void zoomOut() override;
    void zoomReset() override;
    void fitWidth() override;

    bool canSearch() const override { return false; }
    bool find(const QString &, bool) override { return false; }

    QString statusText() const override;

private:
    void togglePlay();
    void showContextMenu(const QPoint &pos);
    void resetAnimation();

    ImageCanvas *m_canvas;
    QSvgRenderer *m_svg = nullptr;
    QMovie *m_movie = nullptr;
    QString m_format;
    QSize m_size;
};
