#pragma once

#include "DocumentView.h"
#include "MarkdownRenderer.h"

#include <QPointF>
#include <QTimer>
#include <QWebEngineFindTextResult>

class QStandardItem;
class QStandardItemModel;
class QUrl;
class QWebEngineView;

class MarkdownView : public DocumentView
{
    Q_OBJECT

public:
    explicit MarkdownView(const QString &path, QWidget *parent = nullptr);

    // Va chiamata prima di creare QApplication.
    static void registerUrlScheme();

    bool load() override;

    void zoomIn() override;
    void zoomOut() override;
    void zoomReset() override;

    bool find(const QString &text, bool backward) override;
    void goToAnchor(const QString &anchor) override;

    QAbstractItemModel *outlineModel() const override;
    void activateOutline(const QModelIndex &index) override;
    QModelIndex currentOutlineIndex() const override;

    QString statusText() const override;

protected:
    void changeEvent(QEvent *event) override;

private:
    QString pageHtml(const QString &body) const;
    void render();
    void onLinkActivated(const QUrl &url);
    void showContextMenu(const QPoint &pos);
    void refreshOutline();
    void applyOutline(const QVariant &headings);

    QWebEngineView *m_view;
    MarkdownRenderer m_renderer;
    QByteArray m_source;
    QPointF m_restoreScroll;
    QString m_pendingAnchor;
    bool m_loading = false;
    QWebEngineFindTextResult m_findResult;

    QStandardItemModel *m_outline;
    QList<QStandardItem *> m_outlineOrder;   // in ordine di documento
    QList<qreal> m_outlineTops;               // posizione di ogni titolo, pixel CSS
    QTimer m_outlineTimer;                    // raccoglie le variazioni d'altezza della pagina
};
