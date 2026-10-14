#include "MarkdownView.h"

#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QWebEngineContextMenuRequest>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>

using namespace Qt::StringLiterals;

static constexpr qreal ZoomStep = 1.1;
static constexpr int OutlineIdRole = Qt::UserRole + 10;
static const QByteArray ResourceScheme = QByteArrayLiteral("disokular");

namespace {

// Pagina che non naviga mai da sola: ogni link viene girato a MarkdownView.
class MarkdownPage : public QWebEnginePage
{
    Q_OBJECT

public:
    using QWebEnginePage::QWebEnginePage;

signals:
    void linkActivated(const QUrl &url);

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        // L'unico caricamento ammesso è il nostro setHtml(), che arriva come data: URL.
        if (url.scheme() == u"data")
            return true;
        if (type == NavigationTypeLinkClicked && isMainFrame)
            emit linkActivated(url);
        return false;
    }
};

// Serve le risorse Qt (CSS, script, font di KaTeX) sullo schema disokular:.
// Con qrc: i font verrebbero bloccati: il browser li chiede in CORS e qrc non risponde.
class ResourceSchemeHandler : public QWebEngineUrlSchemeHandler
{
public:
    using QWebEngineUrlSchemeHandler::QWebEngineUrlSchemeHandler;

    void requestStarted(QWebEngineUrlRequestJob *job) override
    {
        const QString path = job->requestUrl().path();
        auto *file = new QFile(u":"_s + path, job);
        if (!path.startsWith(u"/md/"_s) || !file->open(QIODevice::ReadOnly)) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }
        job->setAdditionalResponseHeaders({{"Access-Control-Allow-Origin", "*"}});
        job->reply(mimeType(path), file);
    }

private:
    static QByteArray mimeType(const QString &path)
    {
        if (path.endsWith(u".css"_s))   return "text/css";
        if (path.endsWith(u".js"_s))    return "text/javascript";
        if (path.endsWith(u".woff2"_s)) return "font/woff2";
        return "application/octet-stream";
    }
};

// Letterale stringa JavaScript sicuro per qualsiasi contenuto.
QString jsString(const QString &s)
{
    const QByteArray json = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString readResource(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// Script di arricchimento: librerie + markdown.js, eseguiti nel mondo isolato.
QWebEngineScript enhancerScript()
{
    static const QString source = [] {
        QString s;
        for (const char *file : {"highlight/highlight.min.js", "highlight/latex.min.js", "highlight/cmake.min.js",
                                 "katex/katex.min.js", "markdown.js"})
            s += readResource(u":/md/"_s + QLatin1String(file)) + u";\n"_s;
        return s;
    }();

    QWebEngineScript script;
    script.setName(u"disokular-markdown"_s);
    script.setSourceCode(source);
    script.setWorldId(QWebEngineScript::ApplicationWorld);
    script.setInjectionPoint(QWebEngineScript::DocumentReady);
    script.setRunsOnSubFrames(false);
    return script;
}

// QtWebEngine condivide le immagini del renderer con Qt tramite GBM; se crede di essere
// su NVIDIA lo disattiva e ripiega su Vulkan. Con grafica ibrida (sessione con
// __GLX_VENDOR_LIBRARY_NAME=nvidia, Qt che rende su Intel con Mesa) quel ripiego va in
// crash dentro Mesa all'apertura della prima pagina. Se Qt è su Mesa, GBM funziona:
// lo si forza. Va fatto prima che nasca il primo QWebEngineView.
void enableGbmOnMesa()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    if (qEnvironmentVariableIsSet("QTWEBENGINE_FORCE_USE_GBM"))
        return;   // scelta esplicita dell'utente

    QOpenGLContext ctx;
    QOffscreenSurface surface;
    surface.create();
    if (!ctx.create() || !ctx.makeCurrent(&surface))
        return;
    const auto str = [&](GLenum name) {
        return QString::fromLatin1(reinterpret_cast<const char *>(ctx.functions()->glGetString(name)));
    };
    const bool mesa = str(GL_RENDERER).contains(u"Mesa"_s) || str(GL_VERSION).contains(u"Mesa"_s);
    ctx.doneCurrent();
    if (mesa)
        qputenv("QTWEBENGINE_FORCE_USE_GBM", "1");
}

} // namespace

void MarkdownView::registerUrlScheme()
{
    QWebEngineUrlScheme scheme(ResourceScheme);
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalAccessAllowed
                    | QWebEngineUrlScheme::CorsEnabled);
    QWebEngineUrlScheme::registerScheme(scheme);
}

MarkdownView::MarkdownView(const QString &path, QWidget *parent)
    : DocumentView(path, parent)
    , m_outline(new QStandardItemModel(this))
{
    enableGbmOnMesa();
    m_view = new QWebEngineView(this);

    m_outlineTimer.setSingleShot(true);
    m_outlineTimer.setInterval(150);
    connect(&m_outlineTimer, &QTimer::timeout, this, &MarkdownView::refreshOutline);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);

    QWebEngineProfile *profile = QWebEngineProfile::defaultProfile();
    if (!profile->urlSchemeHandler(ResourceScheme))
        profile->installUrlSchemeHandler(ResourceScheme, new ResourceSchemeHandler(profile));

    auto *page = new MarkdownPage(profile, m_view);
    m_view->setPage(page);

    // Il JavaScript scritto nel documento non gira; i nostri script sì (mondo isolato).
    QWebEngineSettings *s = page->settings();
    s->setAttribute(QWebEngineSettings::JavascriptEnabled, false);
    s->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    s->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);   // immagini e badge remoti
    s->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    s->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
    page->scripts().insert(enhancerScript());

    connect(page, &MarkdownPage::linkActivated, this, &MarkdownView::onLinkActivated);
    connect(page, &QWebEnginePage::linkHovered, this, [this](const QString &url) {
        m_view->setToolTip(url);
    });
    connect(page, &QWebEnginePage::findTextFinished, this, [this](const QWebEngineFindTextResult &r) {
        m_findResult = r;
        if (r.numberOfMatches() == 0)
            emit searchFailed();
        emit statusChanged();
    });
    connect(page, &QWebEnginePage::loadStarted, this, [this] { m_loading = true; });
    // Formule, immagini e font cambiano l'altezza dopo il caricamento: le posizioni dei titoli vanno riprese.
    connect(page, &QWebEnginePage::contentsSizeChanged, this, [this] { m_outlineTimer.start(); });
    connect(page, &QWebEnginePage::scrollPositionChanged, this, &DocumentView::positionChanged);
    connect(page, &QWebEnginePage::loadFinished, this, [this] {
        m_loading = false;
        refreshOutline();
        if (!m_pendingAnchor.isEmpty()) {
            goToAnchor(m_pendingAnchor);
            m_pendingAnchor.clear();
        } else if (!m_restoreScroll.isNull()) {
            m_view->page()->runJavaScript(u"window.scrollTo(%1, %2)"_s.arg(m_restoreScroll.x()).arg(m_restoreScroll.y()),
                                          QWebEngineScript::ApplicationWorld);
        }
    });
    connect(page, &QWebEnginePage::zoomFactorChanged, this, &DocumentView::statusChanged);

    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QWidget::customContextMenuRequested, this, &MarkdownView::showContextMenu);
}

bool MarkdownView::load()
{
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_error = f.errorString();
        return false;
    }
    m_source = f.readAll();
    m_renderer.setDocument(m_path);
    m_error.clear();
    render();
    return true;
}

void MarkdownView::render()
{
    // Al primo caricamento scrollPosition() è (0,0): nessun ripristino.
    m_restoreScroll = m_view->page()->scrollPosition();
    // Il baseUrl fa risolvere immagini e link relativi rispetto al file.
    m_view->setHtml(pageHtml(m_renderer.render(m_source)), QUrl::fromLocalFile(m_path));
    emit statusChanged();
}

QString MarkdownView::pageHtml(const QString &body) const
{
    const QPalette pal = palette();
    const bool dark = pal.color(QPalette::Base).lightness() < 128;
    auto c = [&](QPalette::ColorRole role) { return pal.color(role).name(); };

    // Il bordo e lo sfondo del codice si ricavano dal fondo, così restano leggibili con ogni tema.
    const QColor base = pal.color(QPalette::Base);
    const QString border = (dark ? base.lighter(165) : base.darker(115)).name();
    const QString codeBg = (dark ? base.lighter(130) : base.darker(104)).name();

    return uR"(<!doctype html>
<html class="%1"><head><meta charset="utf-8">
<style>:root { --bg:%2; --fg:%3; --link:%4; --accent:%5; --muted:%6; --border:%7; --code-bg:%8; }</style>
<link rel="stylesheet" href="disokular:/md/katex/katex.min.css">
<link rel="stylesheet" href="disokular:/md/highlight/%9.min.css">
<link rel="stylesheet" href="disokular:/md/markdown.css">
</head><body><article>
%10
</article></body></html>)"_s
        .arg(dark ? u"dark"_s : u"light"_s,
             c(QPalette::Base), c(QPalette::Text),
             dark ? pal.color(QPalette::Link).lighter(140).name() : c(QPalette::Link),
             c(QPalette::Highlight), c(QPalette::PlaceholderText), border, codeBg,
             dark ? u"github-dark"_s : u"github"_s)
        .arg(body);
}

void MarkdownView::onLinkActivated(const QUrl &url)
{
    if (url.isLocalFile()) {
        const QString file = QFileInfo(url.toLocalFile()).absoluteFilePath();
        if (file == m_path) {
            if (url.hasFragment())
                goToAnchor(url.fragment(QUrl::FullyEncoded));
            return;
        }
        if (QFileInfo(file).isFile()) {
            emit openRequested(file, url.fragment(QUrl::FullyEncoded));
            return;
        }
    }
    QDesktopServices::openUrl(url);
}

void MarkdownView::showContextMenu(const QPoint &pos)
{
    QWebEnginePage *page = m_view->page();
    const QWebEngineContextMenuRequest *req = m_view->lastContextMenuRequest();

    QMenu menu;
    if (req && !req->selectedText().isEmpty())
        menu.addAction(page->action(QWebEnginePage::Copy));
    if (req && req->linkUrl().isValid())
        menu.addAction(page->action(QWebEnginePage::CopyLinkToClipboard));
    if (req && req->mediaType() == QWebEngineContextMenuRequest::MediaTypeImage)
        menu.addAction(page->action(QWebEnginePage::CopyImageToClipboard));
    menu.addAction(page->action(QWebEnginePage::SelectAll));
    menu.exec(m_view->mapToGlobal(pos));
}

void MarkdownView::zoomIn()    { m_view->setZoomFactor(qMin(m_view->zoomFactor() * ZoomStep, 5.0)); }
void MarkdownView::zoomOut()   { m_view->setZoomFactor(qMax(m_view->zoomFactor() / ZoomStep, 0.25)); }
void MarkdownView::zoomReset() { m_view->setZoomFactor(1.0); }

bool MarkdownView::find(const QString &text, bool backward)
{
    m_view->page()->findText(text, backward ? QWebEnginePage::FindBackward : QWebEnginePage::FindFlags());
    // L'esito arriva con findTextFinished; un fallimento viene segnalato da searchFailed().
    return true;
}

void MarkdownView::goToAnchor(const QString &anchor)
{
    if (m_loading) {
        m_pendingAnchor = anchor;
        return;
    }
    m_view->page()->runJavaScript(u"window.__disGoto(%1)"_s.arg(jsString(anchor)), QWebEngineScript::ApplicationWorld);
}

QString MarkdownView::statusText() const
{
    if (!m_error.isEmpty())
        return m_error;
    QString s = tr("Markdown — zoom %1%").arg(qRound(m_view->zoomFactor() * 100));
    if (m_findResult.numberOfMatches() > 0)
        s += tr(" — risultato %1 di %2").arg(m_findResult.activeMatch()).arg(m_findResult.numberOfMatches());
    return s;
}

void MarkdownView::refreshOutline()
{
    m_view->page()->runJavaScript(u"window.__disOutline ? window.__disOutline() : []"_s,
                                  QWebEngineScript::ApplicationWorld,
                                  [this](const QVariant &result) { applyOutline(result); });
}

void MarkdownView::applyOutline(const QVariant &headings)
{
    const QVariantList list = headings.toList();

    // Solo le posizioni cambiate (formule, immagini): niente reset, il pannello resta com'è.
    if (list.size() == m_outlineOrder.size()) {
        bool same = true;
        for (int i = 0; i < list.size() && same; ++i)
            same = list[i].toMap().value(u"id"_s).toString() == m_outlineOrder[i]->data(OutlineIdRole).toString();
        if (same) {
            for (int i = 0; i < list.size(); ++i)
                m_outlineTops[i] = list[i].toMap().value(u"top"_s).toReal();
            emit positionChanged();
            return;
        }
    }

    // Prima le liste: clear() emette modelReset e il pannello chiede subito la sezione corrente.
    m_outlineOrder.clear();
    m_outlineTops.clear();
    m_outline->clear();

    // Annidamento per livello: ogni titolo sta sotto l'ultimo titolo di livello minore.
    // Un salto (## subito dopo un #### ) si appoggia al più vicino disponibile.
    QList<QPair<int, QStandardItem *>> stack;
    QList<QStandardItem *> roots;
    for (const QVariant &v : list) {
        const QVariantMap h = v.toMap();
        const int level = h.value(u"level"_s).toInt();
        auto *item = new QStandardItem(h.value(u"text"_s).toString());
        item->setData(h.value(u"id"_s), OutlineIdRole);
        item->setToolTip(item->text());
        item->setEditable(false);

        while (!stack.isEmpty() && stack.last().first >= level)
            stack.removeLast();
        if (stack.isEmpty())
            roots << item;
        else
            stack.last().second->appendRow(item);
        stack.append({level, item});

        m_outlineOrder << item;
        m_outlineTops << h.value(u"top"_s).toReal();
    }

    // Un unico titolo di primo livello (il titolo della nota) non aiuta: si mostrano i suoi figli.
    if (roots.size() == 1 && roots.first()->rowCount() > 0 && m_outlineOrder.size() > 1) {
        QStandardItem *title = roots.first();
        roots = title->takeColumn(0);
        m_outlineOrder.removeFirst();
        m_outlineTops.removeFirst();
        delete title;
    }
    for (QStandardItem *item : std::as_const(roots))
        m_outline->appendRow(item);

    emit outlineChanged();
}

QAbstractItemModel *MarkdownView::outlineModel() const
{
    return m_outline;
}

void MarkdownView::activateOutline(const QModelIndex &index)
{
    goToAnchor(index.data(OutlineIdRole).toString());
}

QModelIndex MarkdownView::currentOutlineIndex() const
{
    // Come nel PDF: l'ultimo titolo sopra il primo terzo della vista; in fondo al documento,
    // dove l'ultima sezione non può salire fino in cima, l'ultimo titolo visibile.
    // scrollPosition() e contentsSize() sono in pixel del dispositivo (CSS × zoom), i titoli in CSS.
    const qreal zoom = m_view->zoomFactor();
    const qreal scroll = m_view->page()->scrollPosition().y();
    const bool atEnd = scroll + m_view->height() >= m_view->page()->contentsSize().height() - 2;
    const qreal probe = (scroll + (atEnd ? m_view->height() : m_view->height() / 3)) / zoom;
    QStandardItem *best = nullptr;
    for (int i = 0; i < m_outlineOrder.size() && m_outlineTops[i] <= probe; ++i)
        best = m_outlineOrder[i];
    return best ? best->index() : QModelIndex();
}

void MarkdownView::changeEvent(QEvent *event)
{
    // Cambio di tema del sistema: ricostruisci la pagina con i nuovi colori.
    if (event->type() == QEvent::PaletteChange && !m_source.isNull())
        render();
    DocumentView::changeEvent(event);
}

#include "MarkdownView.moc"
