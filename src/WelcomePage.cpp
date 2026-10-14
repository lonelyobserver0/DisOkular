#include "WelcomePage.h"

#include "DocumentView.h"
#include "History.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {

enum Role {
    PathRole = Qt::UserRole + 1,
    OpenedRole,
    ExistsRole,
    FilterRole,   // nome e cartella: il filtro cerca in entrambi
};

// "adesso", "5 minuti fa", "ieri", "3 giorni fa", poi la data.
QString relativeTime(const QDateTime &when)
{
    if (!when.isValid())
        return {};
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 secs = when.secsTo(now);
    if (secs < 60)
        return WelcomePage::tr("adesso");
    if (secs < 3600)
        return WelcomePage::tr("%n minuti fa", "", int(secs / 60));
    const qint64 days = when.date().daysTo(now.date());
    if (days == 0)
        return WelcomePage::tr("%n ore fa", "", int(secs / 3600));
    if (days == 1)
        return WelcomePage::tr("ieri alle %1").arg(when.toString(u"HH:mm"_s));
    if (days < 7)
        return WelcomePage::tr("%n giorni fa", "", int(days));
    return QLocale(QLocale::Italian).toString(when.date(), u"d MMMM yyyy"_s);
}

struct Badge {
    QString text;
    QColor color;
};

Badge badgeFor(const QString &path)
{
    switch (kindOf(path)) {
    case Kind::Pdf:      return {u"PDF"_s, QColor(0xd9, 0x48, 0x3b)};
    case Kind::Markdown: return {u"MD"_s,  QColor(0x35, 0x74, 0xf0)};
    case Kind::Latex:    return {u"TEX"_s, QColor(0x2e, 0x8b, 0x57)};
    case Kind::Unknown:  break;
    }
    return {u"?"_s, QColor(0x88, 0x88, 0x88)};
}

// Riga a due livelli: etichetta del tipo, nome in evidenza, cartella e data attenuate.
class HistoryDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(200, option.fontMetrics.height() * 2 + 22);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        opt.icon = QIcon();
        const QWidget *w = opt.widget;
        QStyle *style = w ? w->style() : QApplication::style();
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, p, w);

        const QString path = index.data(PathRole).toString();
        const bool exists = index.data(ExistsRole).toBool();
        const bool selected = opt.state & QStyle::State_Selected;
        const QPalette &pal = opt.palette;
        QColor text = pal.color(selected ? QPalette::HighlightedText : QPalette::Text);
        QColor muted = text;
        muted.setAlphaF(0.6);
        if (!exists)
            text = muted;

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect.adjusted(12, 0, -12, 0);

        // Etichetta del tipo
        const Badge badge = badgeFor(path);
        QFont badgeFont = opt.font;
        badgeFont.setBold(true);
        badgeFont.setPointSizeF(badgeFont.pointSizeF() * 0.75);
        const QRect badgeRect(r.left(), r.center().y() - 14, 40, 28);
        QColor fill = badge.color;
        if (!exists)
            fill.setAlphaF(0.4);
        QPainterPath rounded;
        rounded.addRoundedRect(badgeRect, 6, 6);
        p->fillPath(rounded, fill);
        p->setFont(badgeFont);
        p->setPen(Qt::white);
        p->drawText(badgeRect, Qt::AlignCenter, badge.text);

        // Nome e, sotto, cartella
        const QFileInfo fi(path);
        const int textLeft = badgeRect.right() + 14;
        const QString when = relativeTime(index.data(OpenedRole).toDateTime());
        QFont small = opt.font;
        small.setPointSizeF(small.pointSizeF() * 0.9);
        const int whenWidth = QFontMetrics(small).horizontalAdvance(when) + 12;
        const int lineH = opt.fontMetrics.height();
        const int top = r.center().y() - lineH - 1;

        QFont nameFont = opt.font;
        nameFont.setWeight(QFont::DemiBold);
        p->setFont(nameFont);
        p->setPen(text);
        const QRect nameRect(textLeft, top, r.right() - textLeft - whenWidth, lineH);
        QString name = fi.fileName();
        if (!exists)
            name += tr(" · non trovato");
        p->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(nameFont).elidedText(name, Qt::ElideRight, nameRect.width()));

        p->setFont(small);
        p->setPen(muted);
        QString folder = fi.absolutePath();
        const QString home = QDir::homePath();
        if (folder.startsWith(home))
            folder = u"~"_s + folder.mid(home.size());
        const QRect folderRect(textLeft, top + lineH + 2, r.right() - textLeft, lineH);
        // La parte significativa di un percorso è la fine: si tronca a sinistra.
        p->drawText(folderRect, Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(small).elidedText(folder, Qt::ElideLeft, folderRect.width()));
        p->drawText(QRect(r.right() - whenWidth, top, whenWidth, lineH), Qt::AlignRight | Qt::AlignVCenter, when);
        p->restore();
    }

private:
    static QString tr(const char *s) { return WelcomePage::tr(s); }
};

} // namespace

WelcomePage::WelcomePage(History *history, QWidget *parent)
    : QWidget(parent)
    , m_history(history)
    , m_filter(new QLineEdit(this))
    , m_list(new QListView(this))
    , m_empty(new QLabel(this))
    , m_clear(new QPushButton(tr("Svuota"), this))
    , m_model(new QStandardItemModel(this))
    , m_proxy(new QSortFilterProxyModel(this))
{
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);

    // Colonna centrale a larghezza di lettura, centrata nella finestra.
    auto *column = new QWidget(this);
    column->setMaximumWidth(720);
    auto *outer = new QHBoxLayout(this);
    outer->addStretch(1);
    outer->addWidget(column, 4);
    outer->addStretch(1);

    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(24, 56, 24, 24);
    layout->setSpacing(0);

    // Intestazione
    auto *title = new QLabel(QStringLiteral("DisOkular"), column);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 2.6);
    titleFont.setWeight(QFont::Bold);
    titleFont.setLetterSpacing(QFont::PercentageSpacing, 97);
    title->setFont(titleFont);

    auto *subtitle = new QLabel(tr("Lettore di documenti PDF, Markdown e LaTeX"), column);
    QFont subFont = subtitle->font();
    subFont.setPointSizeF(subFont.pointSizeF() * 1.1);
    subtitle->setFont(subFont);
    subtitle->setForegroundRole(QPalette::PlaceholderText);

    auto *open = new QPushButton(QIcon::fromTheme("document-open"), tr("Apri documento…"), column);
    open->setDefault(true);
    open->setToolTip(QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText));
    connect(open, &QPushButton::clicked, this, &WelcomePage::openDialogRequested);
    auto *hint = new QLabel(tr("oppure trascina qui un file"), column);
    hint->setForegroundRole(QPalette::PlaceholderText);
    auto *actions = new QHBoxLayout;
    actions->addWidget(open);
    actions->addSpacing(12);
    actions->addWidget(hint);
    actions->addStretch(1);

    layout->addWidget(title);
    layout->addSpacing(2);
    layout->addWidget(subtitle);
    layout->addSpacing(24);
    layout->addLayout(actions);
    layout->addSpacing(40);

    // Cronologia
    auto *recentTitle = new QLabel(tr("Recenti"), column);
    QFont sectionFont = recentTitle->font();
    sectionFont.setPointSizeF(sectionFont.pointSizeF() * 1.25);
    sectionFont.setWeight(QFont::DemiBold);
    recentTitle->setFont(sectionFont);

    m_filter->setPlaceholderText(tr("Filtra"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setMaximumWidth(220);
    m_clear->setFlat(true);
    m_clear->setToolTip(tr("Svuota la cronologia"));

    auto *recentHeader = new QHBoxLayout;
    recentHeader->addWidget(recentTitle);
    recentHeader->addStretch(1);
    recentHeader->addWidget(m_filter);
    recentHeader->addWidget(m_clear);
    layout->addLayout(recentHeader);
    layout->addSpacing(8);

    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterRole(FilterRole);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);

    m_list->setModel(m_proxy);
    m_list->setItemDelegate(new HistoryDelegate(m_list));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(true);
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->viewport()->setAutoFillBackground(false);
    layout->addWidget(m_list, 1);

    m_empty->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_empty->setForegroundRole(QPalette::PlaceholderText);
    m_empty->setContentsMargins(0, 24, 0, 0);
    layout->addWidget(m_empty, 1);

    connect(m_list, &QListView::clicked, this, &WelcomePage::activate);
    connect(m_list, &QListView::activated, this, &WelcomePage::activate);
    connect(m_list, &QWidget::customContextMenuRequested, this, &WelcomePage::showContextMenu);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_proxy->setFilterFixedString(text);
        updateEmptyState();
    });
    // Invio nel filtro apre il primo risultato.
    connect(m_filter, &QLineEdit::returnPressed, this, [this] {
        if (m_proxy->rowCount() > 0)
            activate(m_proxy->index(0, 0));
    });
    connect(m_clear, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Svuota cronologia"),
                                  tr("Togliere tutti i documenti dalla cronologia?\nI file non vengono toccati."))
            == QMessageBox::Yes)
            m_history->clear();
    });
    connect(m_history, &History::changed, this, &WelcomePage::rebuild);

    rebuild();
}

void WelcomePage::rebuild()
{
    m_model->clear();
    for (const History::Entry &e : m_history->entries()) {
        const QFileInfo fi(e.path);
        auto *item = new QStandardItem(fi.fileName());
        item->setData(e.path, PathRole);
        item->setData(e.opened, OpenedRole);
        item->setData(fi.exists(), ExistsRole);
        item->setData(fi.fileName() + u' ' + fi.absolutePath(), FilterRole);
        item->setToolTip(e.path);
        m_model->appendRow(item);
    }
    updateEmptyState();
}

void WelcomePage::updateEmptyState()
{
    const bool hasHistory = m_model->rowCount() > 0;
    const bool hasResults = m_proxy->rowCount() > 0;
    m_list->setVisible(hasResults);
    m_empty->setVisible(!hasResults);
    m_empty->setText(hasHistory ? tr("Nessun documento corrisponde al filtro")
                                : tr("I documenti che apri compariranno qui"));
    m_filter->setVisible(hasHistory);
    m_clear->setVisible(hasHistory);
}

void WelcomePage::showEvent(QShowEvent *event)
{
    // Le date relative ("5 minuti fa") invecchiano e i file possono sparire: si ricalcola.
    rebuild();
    QWidget::showEvent(event);
}

void WelcomePage::activate(const QModelIndex &index)
{
    const QString path = index.data(PathRole).toString();
    if (!QFileInfo::exists(path)) {
        if (QMessageBox::question(this, tr("File non trovato"),
                                  tr("\"%1\" non esiste più.\nToglierlo dalla cronologia?").arg(path))
            == QMessageBox::Yes)
            m_history->remove(path);
        return;
    }
    emit openRequested(path);
}

void WelcomePage::showContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_list->indexAt(pos);
    if (!index.isValid())
        return;
    const QString path = index.data(PathRole).toString();
    const bool exists = index.data(ExistsRole).toBool();

    QMenu menu(this);
    QAction *open = menu.addAction(QIcon::fromTheme("document-open"), tr("Apri"));
    open->setEnabled(exists);
    QAction *folder = menu.addAction(QIcon::fromTheme("folder-open"), tr("Apri la cartella"));
    folder->setEnabled(QFileInfo(path).absoluteDir().exists());
    menu.addSeparator();
    QAction *remove = menu.addAction(QIcon::fromTheme("edit-delete"), tr("Rimuovi dalla cronologia"));

    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (chosen == open)
        emit openRequested(path);
    else if (chosen == folder)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    else if (chosen == remove)
        m_history->remove(path);
}
