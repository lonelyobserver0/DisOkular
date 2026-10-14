#include "MarkdownRenderer.h"

#include <md4c-html.h>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

using namespace Qt::StringLiterals;

static constexpr unsigned ParserFlags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS | MD_FLAG_WIKILINKS;

static const QStringList ImageSuffixes = {"png", "jpg", "jpeg", "gif", "webp", "svg", "bmp", "avif"};

static QString findVaultRoot(const QString &dir)
{
    for (QDir d(dir);; ) {
        if (d.exists(QStringLiteral(".obsidian")))
            return d.absolutePath();
        if (!d.cdUp())
            return {};
    }
}

static QString unescapeAttr(QString s)
{
    s.replace(u"&quot;"_s, u"\""_s).replace(u"&lt;"_s, u"<"_s).replace(u"&gt;"_s, u">"_s);
    return s.replace(u"&amp;"_s, u"&"_s);
}

void MarkdownRenderer::setDocument(const QString &path)
{
    m_dir = QFileInfo(path).absolutePath();
    m_index.clear();

    const QString vault = findVaultRoot(m_dir);
    const QString root = vault.isEmpty() ? m_dir : vault;
    const auto flags = vault.isEmpty() ? QDirIterator::NoIteratorFlags : QDirIterator::Subdirectories;

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, flags);
    while (it.hasNext()) {
        const QFileInfo fi(it.next());
        // Le cartelle nascoste (.git, .obsidian, .trash) non contengono note.
        if (fi.absoluteFilePath().mid(root.size()).contains(u"/."_s))
            continue;
        const QString abs = fi.absoluteFilePath();
        // In caso di omonimi vince il più vicino alla radice, come in Obsidian.
        m_index.insert(fi.fileName().toLower(), abs);
        if (fi.suffix().compare(u"md"_s, Qt::CaseInsensitive) == 0)
            m_index.insert(fi.completeBaseName().toLower(), abs);
        // Per i link con percorso, es. [[Cartella/Nota]]:
        m_index.insert(QDir(root).relativeFilePath(abs).toLower(), abs);
        if (fi.suffix().compare(u"md"_s, Qt::CaseInsensitive) == 0)
            m_index.insert(QDir(root).relativeFilePath(abs).chopped(3).toLower(), abs);
    }
}

QString MarkdownRenderer::resolve(const QString &target) const
{
    const QString key = target.trimmed().toLower();
    if (auto it = m_index.constFind(key); it != m_index.cend())
        return *it;
    // Percorso relativo al documento, anche fuori dall'indice.
    const QFileInfo rel(QDir(m_dir).filePath(target));
    if (rel.isFile())
        return rel.absoluteFilePath();
    return {};
}

QString MarkdownRenderer::render(const QByteArray &markdown) const
{
    QString text = QString::fromUtf8(markdown);
    QString front;

    // Frontmatter: un blocco YAML tra due righe "---" in testa al file.
    static const QRegularExpression fm(QStringLiteral(R"(\A---\r?\n(.*?)\r?\n---[ \t]*(\r?\n|\z))"),
                                       QRegularExpression::DotMatchesEverythingOption);
    if (const auto m = fm.match(text); m.hasMatch()) {
        front = renderFrontMatter(m.captured(1));
        text.remove(0, m.capturedLength());
    }

    const QByteArray src = expandEmbeds(text).toUtf8();
    QByteArray html;
    md_html(src.constData(), MD_SIZE(src.size()),
            [](const MD_CHAR *chunk, MD_SIZE size, void *out) { static_cast<QByteArray *>(out)->append(chunk, size); },
            &html, ParserFlags, MD_HTML_FLAG_SKIP_UTF8_BOM);

    return front + expandWikiLinks(QString::fromUtf8(html));
}

QString MarkdownRenderer::renderFrontMatter(const QString &yaml) const
{
    QString rows;
    QString key, value;
    auto flush = [&] {
        if (!key.isEmpty())
            rows += u"<tr><th>%1</th><td>%2</td></tr>"_s.arg(key.toHtmlEscaped(), value.toHtmlEscaped());
        key.clear();
        value.clear();
    };

    for (const QString &line : yaml.split(u'\n')) {
        // Le liste YAML su più righe ("  - voce") si accodano alla chiave precedente.
        if (line.startsWith(u' ') || line.startsWith(u'-')) {
            const QString item = line.trimmed().remove(QRegularExpression(u"^-\\s*"_s));
            if (!item.isEmpty())
                value += (value.isEmpty() ? QString() : u", "_s) + item;
            continue;
        }
        flush();
        const int colon = line.indexOf(u':');
        if (colon > 0) {
            key = line.left(colon).trimmed();
            value = line.mid(colon + 1).trimmed();
            if (value.startsWith(u'[') && value.endsWith(u']'))
                value = value.mid(1, value.size() - 2);
            if (value.size() >= 2 && value.startsWith(u'"') && value.endsWith(u'"'))
                value = value.mid(1, value.size() - 2);
        }
    }
    flush();

    if (rows.isEmpty())
        return {};
    return u"<details class=\"frontmatter\"><summary>Proprietà</summary><table>%1</table></details>\n"_s.arg(rows);
}

QString MarkdownRenderer::expandEmbeds(const QString &markdown) const
{
    // md4c non riconosce ![[...]]: li trasformiamo in HTML grezzo, fuori dai blocchi di codice.
    static const QRegularExpression embed(QStringLiteral(R"(!\[\[([^\]|#]+)(?:#[^\]|]*)?(?:\|([^\]]*))?\]\])"));
    static const QRegularExpression fence(QStringLiteral(R"(^\s{0,3}(```|~~~))"));

    QStringList lines = markdown.split(u'\n');
    bool inFence = false;
    for (QString &line : lines) {
        if (fence.match(line).hasMatch()) {
            inFence = !inFence;
            continue;
        }
        if (inFence || !line.contains(u"![["_s))
            continue;

        QString out;
        qsizetype last = 0;
        for (auto it = embed.globalMatch(line); it.hasNext();) {
            const auto m = it.next();
            out += line.mid(last, m.capturedStart() - last);
            last = m.capturedEnd();

            const QString target = m.captured(1).trimmed();
            const QString alt = m.captured(2).trimmed();
            const QString file = resolve(target);
            const bool isImage = ImageSuffixes.contains(QFileInfo(target).suffix().toLower());

            if (file.isEmpty()) {
                out += u"<span class=\"wikilink missing\">%1</span>"_s.arg(target.toHtmlEscaped());
            } else if (isImage) {
                // ![[img.png|300]] fissa la larghezza; ![[img.png|testo]] è il testo alternativo.
                bool isWidth = false;
                const int width = alt.toInt(&isWidth);
                out += u"<img src=\"%1\" alt=\"%2\"%3>"_s.arg(
                    QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded),
                    (isWidth ? target : alt).toHtmlEscaped(),
                    isWidth ? u" width=\"%1\""_s.arg(width) : QString());
            } else {
                out += u"<a class=\"wikilink\" href=\"%1\">%2</a>"_s.arg(
                    QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded),
                    (alt.isEmpty() ? target : alt).toHtmlEscaped());
            }
        }
        line = out + line.mid(last);
    }
    return lines.join(u'\n');
}

QString MarkdownRenderer::expandWikiLinks(const QString &html) const
{
    static const QRegularExpression wiki(QStringLiteral(R"re(<x-wikilink data-target="([^"]*)">(.*?)</x-wikilink>)re"),
                                         QRegularExpression::DotMatchesEverythingOption);
    QString out;
    qsizetype last = 0;
    for (auto it = wiki.globalMatch(html); it.hasNext();) {
        const auto m = it.next();
        out += html.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();

        const QString target = unescapeAttr(m.captured(1));
        const QString label = m.captured(2);
        const qsizetype hash = target.indexOf(u'#');
        const QString note = hash < 0 ? target : target.left(hash);
        const QString heading = hash < 0 ? QString() : target.mid(hash + 1);

        QString url;
        if (note.isEmpty()) {
            url = u"#"_s + heading;                     // [[#Sezione]] nello stesso file
        } else if (const QString file = resolve(note); !file.isEmpty()) {
            QUrl u = QUrl::fromLocalFile(file);
            if (!heading.isEmpty())
                u.setFragment(heading);
            url = u.toString(QUrl::FullyEncoded);
        }

        if (url.isEmpty())
            out += u"<span class=\"wikilink missing\" title=\"Nota non ancora scritta\">%1</span>"_s.arg(label);
        else
            out += u"<a class=\"wikilink\" href=\"%1\">%2</a>"_s.arg(url.toHtmlEscaped(), label);
    }
    return out + html.mid(last);
}
