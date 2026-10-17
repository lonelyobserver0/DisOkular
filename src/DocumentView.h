#pragma once

#include <QFileSystemWatcher>
#include <QModelIndex>
#include <QTimer>
#include <QWidget>

// Base comune a tutti i visualizzatori: un tab = un documento.
// Le sottoclassi implementano caricamento, zoom, navigazione e ricerca.
class QAbstractItemModel;

enum class Kind { Unknown, Pdf, Markdown, Latex, Image };

// Le estensioni d'immagine leggibili con i plugin Qt installati (minuscole, senza punto).
const QStringList &imageSuffixes();

// Il tipo di documento dall'estensione del file.
Kind kindOf(const QString &path);

class DocumentView : public QWidget
{
    Q_OBJECT

public:
    // Ruolo del modello d'indice con la pagina (0-based) di ogni voce, se c'è.
    static constexpr int OutlinePageRole = Qt::UserRole + 2;

    explicit DocumentView(const QString &path, QWidget *parent = nullptr);

    QString path() const { return m_path; }
    QString errorString() const { return m_error; }

    // (Ri)carica dal disco. false = errore bloccante, vedi errorString().
    virtual bool load() = 0;

    virtual void zoomIn() = 0;
    virtual void zoomOut() = 0;
    virtual void zoomReset() = 0;
    virtual void fitWidth() {}

    // Navigazione a pagine: pageCount() == 0 significa "documento continuo".
    virtual int pageCount() const { return 0; }
    virtual int currentPage() const { return 0; }
    virtual void goToPage(int) {}

    // I formati senza testo (immagini) non offrono la ricerca.
    virtual bool canSearch() const { return true; }
    // Cerca l'occorrenza successiva (o precedente). false = nessun risultato.
    virtual bool find(const QString &text, bool backward) = 0;

    // Indice del documento (segnalibri, titoli). nullptr = nessun indice per questo formato.
    virtual QAbstractItemModel *outlineModel() const { return nullptr; }
    virtual void activateOutline(const QModelIndex &) {}
    // La voce dell'indice che contiene la posizione corrente.
    virtual QModelIndex currentOutlineIndex() const { return {}; }

    // Salta a una sezione interna (es. "#titolo" di un link). Solo i formati che la supportano.
    virtual void goToAnchor(const QString &) {}

    virtual QString statusText() const { return {}; }

    void setAutoReload(bool on) { m_autoReload = on; }

signals:
    void statusChanged();
    // La posizione di lettura è cambiata (scroll): per sincronizzare l'indice.
    void positionChanged();
    // L'indice è stato ricostruito (per i modelli che non emettono modelReset).
    void outlineChanged();
    // Un link interno punta a un altro documento apribile, eventualmente a una sua sezione.
    void openRequested(const QString &path, const QString &anchor);
    // Per le viste con ricerca asincrona: l'ultima ricerca non ha trovato nulla.
    void searchFailed();
    // Un messaggio passeggero per la barra di stato.
    void notice(const QString &text);

protected:
    // Altri file la cui modifica ricarica il documento (capitoli inclusi, .bib).
    void setExtraWatchedFiles(const QStringList &files);

    QString m_path;
    QString m_error;

private:
    void onFileChanged(const QString &file);

    QFileSystemWatcher m_watcher;
    QTimer m_reloadTimer;
    bool m_autoReload = true;
};
