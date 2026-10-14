#pragma once

#include <QList>
#include <QRectF>
#include <QString>

#include <optional>

typedef struct synctex_scanner_t synctex_scanner_s;
typedef synctex_scanner_s *synctex_scanner_p;

// Corrispondenza sorgente ↔ PDF tramite libsynctex.
// Pagine 0-based; coordinate in punti PDF, origine in alto a sinistra (come QtPdf).
class SyncTex
{
public:
    SyncTex() = default;
    ~SyncTex();
    SyncTex(const SyncTex &) = delete;
    SyncTex &operator=(const SyncTex &) = delete;

    // Legge il .synctex(.gz) accanto al PDF. false se manca.
    bool load(const QString &pdfPath);
    void clear();
    bool isValid() const { return m_scanner != nullptr; }

    struct Source {
        QString file;    // percorso assoluto
        int line = 0;    // 1-based
        int column = 0;  // 1-based; 0 se il motore non la registra (pdflatex)
    };
    // Ricerca inversa: dal punto nel PDF alla riga del sorgente.
    std::optional<Source> sourceAt(int page, QPointF point);

    struct Box {
        int page;
        QRectF rect;
    };
    // Ricerca in avanti: le righe del PDF prodotte da file:line.
    QList<Box> boxesFor(const QString &file, int line, int column = 0);

    // Tutti i file letti da TeX (percorsi normalizzati), compresi quelli di sistema.
    QStringList inputs();

    // Il file ha contribuito al documento (es. un capitolo incluso con \input)?
    bool hasSource(const QString &file);

private:
    // Il nome con cui SyncTeX conosce `file`, o vuoto.
    QByteArray nameFor(const QString &file);

    synctex_scanner_p m_scanner = nullptr;
};
