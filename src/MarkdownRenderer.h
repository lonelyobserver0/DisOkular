#pragma once

#include <QHash>
#include <QString>

// Converte Markdown in HTML con md4c, più le estensioni di Obsidian:
// frontmatter YAML, [[wikilink]] e ![[embed]] risolti dentro il vault.
class MarkdownRenderer
{
public:
    // Indicizza i file raggiungibili da `path`: tutto il vault se il file ne fa parte
    // (c'è una cartella .obsidian più in alto), altrimenti la sua sola cartella.
    void setDocument(const QString &path);

    // Restituisce il solo corpo HTML (niente <html>/<head>).
    QString render(const QByteArray &markdown) const;

private:
    QString resolve(const QString &target) const;
    QString renderFrontMatter(const QString &yaml) const;
    QString expandEmbeds(const QString &markdown) const;
    QString expandWikiLinks(const QString &html) const;

    QString m_dir;
    QHash<QString, QString> m_index;   // nome in minuscolo (con e senza .md) → percorso
};
