#include "MainWindow.h"

#include "LatexView.h"
#include "MarkdownView.h"
#include "History.h"
#include "OutlinePanel.h"
#include "WelcomePage.h"
#include "PdfView.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>

using namespace Qt::StringLiterals;

static constexpr int MaxRecentMenu = 10;
static const QString NotFoundStyle = QStringLiteral("QLineEdit { background: #f4c7c3; color: black; }");

bool MainWindow::isSupported(const QString &path)
{
    return kindOf(path) != Kind::Unknown;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_history(new History(this))
    , m_central(new QStackedWidget(this))
    , m_welcome(new WelcomePage(m_history, this))
    , m_tabs(new QTabWidget(this))
    , m_outline(new OutlinePanel(this))
    , m_outlineDock(new QDockWidget(tr("Indice"), this))
{
    setWindowTitle(QStringLiteral("DisOkular"));
    setAcceptDrops(true);

    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    // Senza documenti aperti si vede la pagina iniziale.
    m_central->addWidget(m_welcome);
    m_central->addWidget(m_tabs);
    setCentralWidget(m_central);
    connect(m_welcome, &WelcomePage::openRequested, this, [this](const QString &p) { openFile(p); });
    connect(m_welcome, &WelcomePage::openDialogRequested, this, &MainWindow::openDialog);
    connect(m_history, &History::changed, this, &MainWindow::rebuildRecentMenu);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::updateUi);

    m_outlineDock->setObjectName("outlineDock");
    m_outlineDock->setWidget(m_outline);
    m_outlineDock->setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable);
    m_outlineDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, m_outlineDock);
    resizeDocks({m_outlineDock}, {260}, Qt::Horizontal);

    createActions();

    // Etichetta permanente: i messaggi passeggeri (showMessage) le passano sopra e poi spariscono.
    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel, 1);

    QSettings s;
    if (!restoreGeometry(s.value("window/geometry").toByteArray()))
        resize(1000, 800);
    restoreState(s.value("window/state").toByteArray());
    m_outlineWanted = s.value("window/outlineVisible", true).toBool();
    m_findBar->hide();

    rebuildRecentMenu();
    updateUi();
}

MainWindow::~MainWindow()
{
    // Le viste muoiono dopo questo distruttore, e mentre vengono smontate le loro scrollbar
    // emettono ancora segnali: staccali prima che raggiungano m_tabs già distrutto.
    for (int i = 0; i < m_tabs->count(); ++i)
        m_tabs->widget(i)->disconnect(this);
    m_outline->setView(nullptr);
}

void MainWindow::createActions()
{
    auto add = [this](QMenu *menu, const QString &text, const char *icon,
                      QKeySequence key, auto slot) {
        auto *a = menu->addAction(QIcon::fromTheme(QLatin1String(icon)), text);
        a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };

    // File
    QMenu *file = menuBar()->addMenu(tr("&File"));
    QAction *open = add(file, tr("&Apri…"), "document-open", QKeySequence::Open, &MainWindow::openDialog);
    m_recentMenu = file->addMenu(QIcon::fromTheme("document-open-recent"), tr("Apri &recenti"));
    file->addSeparator();
    QAction *reload = add(file, tr("&Ricarica"), "view-refresh", QKeySequence::Refresh, &MainWindow::reloadCurrent);
    QAction *close = add(file, tr("&Chiudi"), "document-close", QKeySequence::Close,
                         [this] { closeTab(m_tabs->currentIndex()); });
    file->addSeparator();
    add(file, tr("&Esci"), "application-exit", QKeySequence::Quit, &QWidget::close);

    // Visualizza
    QMenu *view = menuBar()->addMenu(tr("&Visualizza"));
    QAction *zin = add(view, tr("Ingrandisci"), "zoom-in", QKeySequence::ZoomIn, [this] { current()->zoomIn(); });
    QAction *zout = add(view, tr("Riduci"), "zoom-out", QKeySequence::ZoomOut, [this] { current()->zoomOut(); });
    QAction *zreset = add(view, tr("Dimensione reale"), "zoom-original", QKeySequence(Qt::CTRL | Qt::Key_0),
                          [this] { current()->zoomReset(); });
    QAction *zfit = add(view, tr("Adatta alla larghezza"), "zoom-fit-width", QKeySequence(Qt::CTRL | Qt::Key_9),
                        [this] { current()->fitWidth(); });
    view->addSeparator();
    QAction *outline = m_outlineDock->toggleViewAction();
    // Solo il gesto dell'utente cambia la preferenza; il nascondimento sulla pagina iniziale no.
    connect(outline, &QAction::triggered, this, [this](bool on) { m_outlineWanted = on; });
    connect(m_outlineDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        // Chiusura dalla × del pannello: anche questa è una scelta dell'utente.
        if (!visible && m_outlineDock->isHidden() && m_tabs->count() > 0)
            m_outlineWanted = false;
    });
    outline->setText(tr("Pannello indice"));
    outline->setIcon(QIcon::fromTheme("view-table-of-contents-ltr"));
    outline->setShortcut(Qt::Key_F7);
    view->addAction(outline);
    add(view, tr("Schermo intero"), "view-fullscreen", QKeySequence::FullScreen, &MainWindow::toggleFullScreen);

    // Vai
    QMenu *go = menuBar()->addMenu(tr("&Vai"));
    QAction *prev = add(go, tr("Pagina precedente"), "go-previous", QKeySequence(Qt::Key_PageUp | Qt::CTRL),
                        [this] { current()->goToPage(current()->currentPage() - 1); });
    QAction *next = add(go, tr("Pagina successiva"), "go-next", QKeySequence(Qt::Key_PageDown | Qt::CTRL),
                        [this] { current()->goToPage(current()->currentPage() + 1); });
    QAction *first = add(go, tr("Prima pagina"), "go-first", QKeySequence(Qt::CTRL | Qt::Key_Home),
                         [this] { current()->goToPage(0); });
    QAction *last = add(go, tr("Ultima pagina"), "go-last", QKeySequence(Qt::CTRL | Qt::Key_End),
                        [this] { current()->goToPage(current()->pageCount() - 1); });
    go->addSeparator();
    QAction *find = add(go, tr("&Trova…"), "edit-find", QKeySequence::Find, &MainWindow::showFindBar);

    // Impostazioni
    QMenu *settings = menuBar()->addMenu(tr("&Impostazioni"));
    add(settings, tr("Editor per SyncTeX…"), "document-edit", QKeySequence(), &MainWindow::chooseEditor);

    m_docActions = {reload, close, zin, zout, zreset, find};
    m_pageActions = {zfit, prev, next, first, last};

    // Barra principale
    QToolBar *tb = addToolBar(tr("Principale"));
    tb->setObjectName("mainToolBar");
    tb->setMovable(false);
    tb->addAction(outline);
    tb->addAction(open);
    tb->addSeparator();
    tb->addAction(prev);
    m_pageSpin = new QSpinBox(this);
    m_pageSpin->setKeyboardTracking(false);
    m_pageSpin->setToolTip(tr("Pagina"));
    m_pageSpinAction = tb->addWidget(m_pageSpin);
    m_pageTotal = new QLabel(this);
    m_pageTotal->setContentsMargins(4, 0, 4, 0);
    m_pageTotalAction = tb->addWidget(m_pageTotal);
    tb->addAction(next);
    tb->addSeparator();
    tb->addAction(zout);
    tb->addAction(zin);
    tb->addAction(zfit);
    tb->addSeparator();
    tb->addAction(find);
    connect(m_pageSpin, &QSpinBox::valueChanged, this, [this](int v) {
        if (current())
            current()->goToPage(v - 1);
    });

    // Barra di ricerca, in basso
    m_findBar = new QToolBar(tr("Trova"), this);
    m_findBar->setObjectName("findBar");
    m_findBar->setMovable(false);
    addToolBar(Qt::BottomToolBarArea, m_findBar);
    m_findEdit = new QLineEdit(this);
    m_findEdit->setPlaceholderText(tr("Cerca nel documento (Invio: successivo, Maiusc+Invio: precedente)"));
    m_findEdit->setClearButtonEnabled(true);
    m_findBar->addWidget(m_findEdit);
    m_findBar->addAction(QIcon::fromTheme("go-up"), tr("Precedente"), this, [this] { findNext(true); });
    m_findBar->addAction(QIcon::fromTheme("go-down"), tr("Successivo"), this, [this] { findNext(false); });
    QAction *hideFind = m_findBar->addAction(QIcon::fromTheme("window-close"), tr("Chiudi"));
    hideFind->setShortcut(Qt::Key_Escape);
    hideFind->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(hideFind, &QAction::triggered, m_findBar, &QWidget::hide);
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] {
        findNext(QApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(m_findEdit, &QLineEdit::textChanged, this, [this] { m_findEdit->setStyleSheet({}); });
}

DocumentView *MainWindow::current() const
{
    return qobject_cast<DocumentView *>(m_tabs->currentWidget());
}

bool MainWindow::openFile(const QString &path, const QString &anchor)
{
    const QFileInfo fi(path);
    const QString abs = fi.absoluteFilePath();

    for (int i = 0; i < m_tabs->count(); ++i) {
        auto *v = qobject_cast<DocumentView *>(m_tabs->widget(i));
        if (v && v->path() == abs) {
            m_tabs->setCurrentIndex(i);
            if (!anchor.isEmpty())
                v->goToAnchor(anchor);
            return true;
        }
    }

    DocumentView *view = nullptr;
    switch (kindOf(abs)) {
    case Kind::Pdf:      view = new PdfView(abs); break;
    case Kind::Markdown: view = new MarkdownView(abs); break;
    case Kind::Latex:    view = new LatexView(abs); break;
    case Kind::Unknown:
        QMessageBox::warning(this, tr("Formato non supportato"),
                             tr("Non so aprire \"%1\".\nFormati supportati: PDF, Markdown, LaTeX.").arg(fi.fileName()));
        return false;
    }

    if (!view->load()) {
        QMessageBox::warning(this, tr("Errore"), tr("Impossibile aprire \"%1\":\n%2").arg(fi.fileName(), view->errorString()));
        delete view;
        return false;
    }

    connect(view, &DocumentView::statusChanged, this, [this, view] {
        if (view == current())
            updateUi();
    });
    connect(view, &DocumentView::positionChanged, this, [this, view] {
        if (view == current())
            m_outline->syncToPosition();
    });
    connect(view, &DocumentView::notice, this, [this](const QString &text) {
        statusBar()->showMessage(text, 5000);
    });
    connect(view, &DocumentView::openRequested, this, [this](const QString &p, const QString &a) {
        if (isSupported(p))
            openFile(p, a);
    });
    connect(view, &DocumentView::searchFailed, this, [this, view] {
        if (view == current())
            m_findEdit->setStyleSheet(NotFoundStyle);
    });

    const int idx = m_tabs->addTab(view, fi.fileName());
    m_tabs->setTabToolTip(idx, abs);
    m_tabs->setCurrentIndex(idx);
    m_history->add(abs);
    if (!anchor.isEmpty())
        view->goToAnchor(anchor);
    return true;
}

void MainWindow::handleRequest(const QJsonObject &request)
{
    for (const QJsonValue &f : request.value(u"open").toArray())
        openFile(f.toString());

    const QJsonObject fw = request.value(u"forward").toObject();
    if (!fw.isEmpty())
        forwardSearch(fw.value(u"file").toString(), fw.value(u"line").toInt(),
                      fw.value(u"column").toInt(), fw.value(u"main").toString());

    // Su Wayland il compositore può ignorarlo: la finestra non si ruba il fuoco da sola.
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
}

void MainWindow::forwardSearch(const QString &file, int line, int column, const QString &main)
{
    LatexView *target = nullptr;

    QString mainTex = main;
    if (mainTex.endsWith(u".pdf"_s, Qt::CaseInsensitive))
        mainTex = mainTex.chopped(4) + u".tex"_s;

    if (!mainTex.isEmpty()) {
        if (openFile(mainTex))
            target = qobject_cast<LatexView *>(current());
    } else {
        // Il documento aperto che include questo sorgente (o è questo sorgente).
        for (int i = 0; i < m_tabs->count() && !target; ++i) {
            auto *lv = qobject_cast<LatexView *>(m_tabs->widget(i));
            if (lv && lv->usesSource(file)) {
                target = lv;
                m_tabs->setCurrentIndex(i);
            }
        }
        if (!target && openFile(file))
            target = qobject_cast<LatexView *>(current());
    }

    if (target)
        target->forwardSearch(file, line, column);
    else
        statusBar()->showMessage(tr("SyncTeX: nessun documento LaTeX per %1").arg(QFileInfo(file).fileName()), 5000);
}

void MainWindow::chooseEditor()
{
    QStringList items = LatexView::editorPresets();
    const QString currentCmd = LatexView::editorCommand();
    items.removeAll(currentCmd);
    items.prepend(currentCmd);

    bool ok = false;
    const QString cmd = QInputDialog::getItem(
        this, tr("Editor per SyncTeX"),
        tr("Comando lanciato con Ctrl+clic sul PDF di un documento LaTeX.\n"
           "%f = file sorgente, %l = riga, %c = colonna."),
        items, 0, true, &ok);
    if (ok && !cmd.trimmed().isEmpty())
        LatexView::setEditorCommand(cmd);
}

void MainWindow::openDialog()
{
    QSettings s;
    const QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Apri documento"), s.value("lastDir").toString(),
        tr("Documenti (*.pdf *.md *.markdown *.mkd *.mdown *.tex *.latex *.ltx);;"
           "PDF (*.pdf);;Markdown (*.md *.markdown *.mkd *.mdown);;LaTeX (*.tex *.latex *.ltx);;"
           "Tutti i file (*)"));
    for (const QString &f : files)
        openFile(f);
    if (!files.isEmpty())
        s.setValue("lastDir", QFileInfo(files.first()).absolutePath());
}

void MainWindow::closeTab(int index)
{
    if (index < 0)
        return;
    QWidget *w = m_tabs->widget(index);
    m_tabs->removeTab(index);
    w->deleteLater();
}

void MainWindow::reloadCurrent()
{
    if (DocumentView *v = current(); v && !v->load())
        statusBar()->showMessage(v->errorString(), 5000);
}

void MainWindow::toggleFullScreen()
{
    setWindowState(windowState() ^ Qt::WindowFullScreen);
}

void MainWindow::showFindBar()
{
    m_findBar->show();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void MainWindow::findNext(bool backward)
{
    DocumentView *v = current();
    const QString text = m_findEdit->text();
    if (!v || text.isEmpty())
        return;
    const bool found = v->find(text, backward);
    m_findEdit->setStyleSheet(found ? QString() : NotFoundStyle);
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    for (const History::Entry &e : m_history->entries()) {
        if (m_recentMenu->actions().size() >= MaxRecentMenu)
            break;
        const QString p = e.path;
        if (!QFileInfo::exists(p))
            continue;
        QAction *a = m_recentMenu->addAction(QFileInfo(p).fileName());
        a->setToolTip(p);
        connect(a, &QAction::triggered, this, [this, p] { openFile(p); });
    }
    m_recentMenu->setEnabled(!m_recentMenu->isEmpty());
}

void MainWindow::updateUi()
{
    DocumentView *v = current();
    const bool welcome = m_tabs->count() == 0;
    m_central->setCurrentWidget(welcome ? static_cast<QWidget *>(m_welcome) : m_tabs);
    // Sulla pagina iniziale l'indice sarebbe vuoto: si nasconde, e torna con il primo documento.
    // isHidden(), non isVisible(): prima del primo show() la finestra intera risulta invisibile.
    const bool show = !welcome && m_outlineWanted;
    if (m_outlineDock->isHidden() == show)
        m_outlineDock->setVisible(show);
    if (m_outline->view() != v)
        m_outline->setView(v);
    else
        m_outline->syncToPosition();

    const bool paged = v && v->pageCount() > 0;

    for (QAction *a : std::as_const(m_docActions))
        a->setEnabled(v);
    for (QAction *a : std::as_const(m_pageActions))
        a->setEnabled(paged);
    m_pageSpinAction->setVisible(paged);
    m_pageTotalAction->setVisible(paged);

    if (paged) {
        const QSignalBlocker block(m_pageSpin);
        m_pageSpin->setRange(1, v->pageCount());
        m_pageSpin->setValue(v->currentPage() + 1);
        m_pageTotal->setText(tr("di %1").arg(v->pageCount()));
    }

    setWindowTitle(v ? QStringLiteral("%1 — DisOkular").arg(QFileInfo(v->path()).fileName())
                     : QStringLiteral("DisOkular"));
    m_statusLabel->setText(v ? v->statusText() : QString());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", saveState());
    s.setValue("window/outlineVisible", m_outlineWanted);
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls())
        return;
    for (const QUrl &u : event->mimeData()->urls()) {
        if (u.isLocalFile() && isSupported(u.toLocalFile())) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (const QUrl &u : event->mimeData()->urls()) {
        if (u.isLocalFile())
            openFile(u.toLocalFile());
    }
}
