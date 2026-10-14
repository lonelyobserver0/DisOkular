#include "OutlinePanel.h"

#include "DocumentView.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

// Titolo a sinistra, numero di pagina allineato a destra e attenuato.
class OutlineDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QVariant page = index.data(DocumentView::OutlinePageRole);
        if (!page.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        const QString title = opt.text;
        opt.text.clear();
        QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette::ColorGroup group = opt.state & QStyle::State_Enabled ? QPalette::Normal : QPalette::Disabled;
        QColor text = opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
        QColor muted = text;
        muted.setAlphaF(0.55);

        QRect r = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget).adjusted(2, 0, -6, 0);
        const QString pageText = QString::number(page.toInt() + 1);
        const int pageWidth = opt.fontMetrics.horizontalAdvance(pageText);

        painter->save();
        painter->setFont(opt.font);
        painter->setPen(muted);
        painter->drawText(r, Qt::AlignRight | Qt::AlignVCenter, pageText);
        r.setRight(r.right() - pageWidth - 10);
        painter->setPen(text);
        painter->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, opt.fontMetrics.elidedText(title, Qt::ElideRight, r.width()));
        painter->restore();
    }
};

} // namespace

OutlinePanel::OutlinePanel(QWidget *parent)
    : QWidget(parent)
    , m_stack(new QStackedWidget(this))
    , m_tree(new QTreeView(this))
    , m_placeholder(new QLabel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_stack);

    m_tree->setHeaderHidden(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setItemDelegate(new OutlineDelegate(m_tree));
    m_tree->setFrameShape(QFrame::NoFrame);
    m_tree->setTextElideMode(Qt::ElideRight);
    m_tree->header()->setStretchLastSection(true);

    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setMargin(16);
    m_placeholder->setForegroundRole(QPalette::PlaceholderText);

    m_stack->addWidget(m_tree);
    m_stack->addWidget(m_placeholder);

    // Solo il gesto dell'utente sposta il documento: la selezione fatta da
    // syncToPosition() passa per il modello di selezione e non emette questi segnali.
    auto activate = [this](const QModelIndex &index) {
        if (m_view)
            m_view->activateOutline(index);
    };
    connect(m_tree, &QTreeView::clicked, this, activate);
    connect(m_tree, &QTreeView::activated, this, activate);

    updatePlaceholder();
}

void OutlinePanel::setView(DocumentView *view)
{
    disconnect(m_resetConn);
    disconnect(m_changedConn);
    m_view = view;
    if (view)
        m_changedConn = connect(view, &DocumentView::outlineChanged, this, &OutlinePanel::onModelReset);

    QAbstractItemModel *model = view ? view->outlineModel() : nullptr;
    m_tree->setModel(model);
    if (model) {
        m_resetConn = connect(model, &QAbstractItemModel::modelReset, this, &OutlinePanel::onModelReset);
        onModelReset();
    } else {
        updatePlaceholder();
    }
}

void OutlinePanel::onModelReset()
{
    // Il primo livello aperto basta a orientarsi senza annegare nelle sottosezioni.
    m_tree->expandToDepth(0);
    updatePlaceholder();
    syncToPosition();
}

void OutlinePanel::updatePlaceholder()
{
    QAbstractItemModel *model = m_tree->model();
    if (model && model->rowCount() > 0) {
        m_stack->setCurrentWidget(m_tree);
        return;
    }
    if (!m_view)
        m_placeholder->setText(tr("Nessun documento aperto"));
    else if (!model)
        m_placeholder->setText(tr("Indice non disponibile per questo formato"));
    else
        m_placeholder->setText(tr("Questo documento non ha un indice"));
    m_stack->setCurrentWidget(m_placeholder);
}

void OutlinePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    syncToPosition();
}

void OutlinePanel::syncToPosition()
{
    if (!m_view || !m_tree->model() || !isVisible())
        return;
    const QModelIndex idx = m_view->currentOutlineIndex();
    if (idx == m_tree->currentIndex())
        return;
    if (!idx.isValid()) {
        m_tree->selectionModel()->clear();
        return;
    }
    m_tree->selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect);
    m_tree->scrollTo(idx);
}
