#include "SyncTex.h"

#include <synctex_parser.h>

#include <QDir>
#include <QFileInfo>

SyncTex::~SyncTex()
{
    clear();
}

void SyncTex::clear()
{
    if (m_scanner)
        synctex_scanner_free(m_scanner);
    m_scanner = nullptr;
}

bool SyncTex::load(const QString &pdfPath)
{
    clear();
    m_scanner = synctex_scanner_new_with_output_file(QFile::encodeName(pdfPath).constData(), nullptr, 1);
    return m_scanner != nullptr;
}

// SyncTeX registra i percorsi così come li ha visti TeX, es. "/progetto/./cap/uno.tex".
static QString normalized(const char *name)
{
    return QDir::cleanPath(QFile::decodeName(name));
}

QByteArray SyncTex::nameFor(const QString &file)
{
    if (!m_scanner)
        return {};
    const QString wanted = QDir::cleanPath(QFileInfo(file).absoluteFilePath());
    for (synctex_node_p input = synctex_scanner_input(m_scanner); input; input = synctex_node_sibling(input)) {
        const char *name = synctex_scanner_get_name(m_scanner, synctex_node_tag(input));
        if (name && normalized(name) == wanted)
            return QByteArray(name);
    }
    return {};
}

QStringList SyncTex::inputs()
{
    QStringList files;
    if (!m_scanner)
        return files;
    for (synctex_node_p input = synctex_scanner_input(m_scanner); input; input = synctex_node_sibling(input))
        if (const char *name = synctex_scanner_get_name(m_scanner, synctex_node_tag(input)))
            files << normalized(name);
    return files;
}

bool SyncTex::hasSource(const QString &file)
{
    return !nameFor(file).isEmpty();
}

std::optional<SyncTex::Source> SyncTex::sourceAt(int page, QPointF point)
{
    if (!m_scanner || synctex_edit_query(m_scanner, page + 1, float(point.x()), float(point.y())) <= 0)
        return std::nullopt;

    synctex_node_p node = synctex_scanner_next_result(m_scanner);
    if (!node)
        return std::nullopt;
    const char *name = synctex_scanner_get_name(m_scanner, synctex_node_tag(node));
    if (!name)
        return std::nullopt;

    Source src;
    src.file = normalized(name);
    src.line = qMax(1, synctex_node_line(node));
    src.column = qMax(0, synctex_node_column(node));
    return src;
}

QList<SyncTex::Box> SyncTex::boxesFor(const QString &file, int line, int column)
{
    QList<Box> boxes;
    const QByteArray name = nameFor(file);
    if (name.isEmpty() || synctex_display_query(m_scanner, name.constData(), line, column > 0 ? column : -1, -1) <= 0)
        return boxes;

    while (synctex_node_p node = synctex_scanner_next_result(m_scanner)) {
        // v è la linea di base: il riquadro va da v - altezza a v + profondità.
        const qreal h = synctex_node_box_visible_h(node);
        const qreal v = synctex_node_box_visible_v(node);
        const qreal height = synctex_node_box_visible_height(node);
        const qreal depth = synctex_node_box_visible_depth(node);
        const qreal width = synctex_node_box_visible_width(node);
        boxes.append({synctex_node_page(node) - 1, QRectF(h, v - height, width, height + depth)});
    }
    return boxes;
}
