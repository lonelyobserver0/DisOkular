#pragma once

#include "PdfView.h"
#include "SyncTex.h"

#include <QProcess>
#include <QTemporaryDir>

class QPlainTextEdit;
class QSplitter;

// Compila il .tex in una cartella temporanea e mostra il PDF risultante.
// Il motore si sceglie con il commento magico "% !TEX program = xelatex".
class LatexView : public PdfView
{
    Q_OBJECT

public:
    explicit LatexView(const QString &path, QWidget *parent = nullptr);
    ~LatexView() override;

    bool load() override;
    QString statusText() const override;

    // SyncTeX in avanti: mostra ed evidenzia nel PDF la riga `line` di `file`.
    // Se è in corso una compilazione, aspetta il PDF nuovo.
    void forwardSearch(const QString &file, int line, int column = 0);
    // Il documento usa questo sorgente (sé stesso o un file incluso)?
    bool usesSource(const QString &file);

    // Comando per aprire l'editor nella ricerca inversa: %f file, %l riga, %c colonna.
    static QString editorCommand();
    static void setEditorCommand(const QString &command);
    static QStringList editorPresets();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString detectEngine() const;
    void startPass();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onLatexFinished(bool ok, const QString &log);
    void onBibFinished(bool ok, const QString &log);
    void finish(const QString &log);

    // Bibliografia: quale strumento serve (vuoto = nessuno) e se va rilanciato.
    QString bibTool() const;
    QByteArray bibSignature(const QString &tool) const;
    QStringList bibFiles() const;
    void startBib(const QString &tool);
    void updateWatchedFiles();
    QString outFile(const QString &suffix) const;
    void showLog(const QString &log);
    void runForwardSearch();
    void inverseSearch(QPoint viewportPos);

    QTemporaryDir m_outDir;
    QProcess m_proc;
    QPlainTextEdit *m_log;
    enum class Phase { Idle, Latex, Bib };
    Phase m_phase = Phase::Idle;
    QString m_engine;
    QString m_bibTool;          // bibtex o biber, durante la fase Bib
    QByteArray m_bibSignature;  // citazioni e .bib dell'ultima bibliografia generata
    QString m_bibWarning;       // errori della bibliografia, mostrati a PDF pronto
    int m_pass = 0;
    int m_minPasses = 0;        // dopo la bibliografia servono almeno due passate
    bool m_pending = false;     // il file è cambiato durante la compilazione
    bool m_hasPdf = false;
    SyncTex m_synctex;
    struct Forward { QString file; int line; int column; };
    std::optional<Forward> m_pendingForward;
};
