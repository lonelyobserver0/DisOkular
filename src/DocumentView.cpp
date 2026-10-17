#include "DocumentView.h"

#include <QFileInfo>
#include <QImageReader>

const QStringList &imageSuffixes()
{
    // Chiesto a Qt e non scritto a mano: installando kimageformats arrivano da soli AVIF, JXL, HEIF…
    static const QStringList suffixes = [] {
        QStringList list;
        for (const QByteArray &f : QImageReader::supportedImageFormats())
            list << QString::fromLatin1(f).toLower();
        list.removeAll(QStringLiteral("pdf"));   // il plugin di QtPdf: i PDF hanno la loro vista
        for (const char *svg : {"svg", "svgz"})
            if (!list.contains(QLatin1String(svg)))
                list << QLatin1String(svg);       // gli SVG li disegna QSvgRenderer, plugin o no
        list.removeDuplicates();
        list.sort();
        return list;
    }();
    return suffixes;
}

Kind kindOf(const QString &path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == u"pdf")
        return Kind::Pdf;
    if (ext == u"md" || ext == u"markdown" || ext == u"mkd" || ext == u"mdown")
        return Kind::Markdown;
    if (ext == u"tex" || ext == u"latex" || ext == u"ltx")
        return Kind::Latex;
    if (imageSuffixes().contains(ext))
        return Kind::Image;
    return Kind::Unknown;
}

DocumentView::DocumentView(const QString &path, QWidget *parent)
    : QWidget(parent)
    , m_path(QFileInfo(path).absoluteFilePath())
{
    m_watcher.addPath(m_path);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &DocumentView::onFileChanged);

    // Gli editor salvano spesso in più scritture: aspetta che il file si stabilizzi.
    m_reloadTimer.setSingleShot(true);
    m_reloadTimer.setInterval(250);
    connect(&m_reloadTimer, &QTimer::timeout, this, [this] {
        if (QFileInfo::exists(m_path))
            load();
    });
}

void DocumentView::setExtraWatchedFiles(const QStringList &files)
{
    for (const QString &f : m_watcher.files())
        if (f != m_path && !files.contains(f))
            m_watcher.removePath(f);
    for (const QString &f : files)
        if (f != m_path && !m_watcher.files().contains(f) && QFileInfo::exists(f))
            m_watcher.addPath(f);
}

void DocumentView::onFileChanged(const QString &file)
{
    // Salvataggio atomico (scrivi + rinomina): il watcher perde il file, va riagganciato.
    if (!m_watcher.files().contains(file) && QFileInfo::exists(file))
        m_watcher.addPath(file);
    if (m_autoReload)
        m_reloadTimer.start();
}
