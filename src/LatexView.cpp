#include "LatexView.h"

#include <QDir>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

// Riferimenti incrociati e indice richiedono più passate.
using namespace Qt::StringLiterals;

static constexpr int MaxPasses = 5;

static const QString DefaultEditor = QStringLiteral("codium --goto %f:%l:%c");

LatexView::LatexView(const QString &path, QWidget *parent)
    : PdfView(path, parent)
    , m_log(new QPlainTextEdit(this))
{
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMaximumHeight(220);
    m_log->hide();
    m_layout->addWidget(m_log);

    // Ctrl+clic sul PDF → riga del sorgente nell'editor.
    viewport()->installEventFilter(this);

    m_proc.setWorkingDirectory(QFileInfo(m_path).absolutePath());
    m_proc.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_proc, &QProcess::finished, this, &LatexView::onFinished);
    connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        if (m_phase == Phase::Bib) {
            // Senza bibtex/biber il documento si compila lo stesso, con citazioni "?".
            onBibFinished(false, tr("%1 non è installato: bibliografia non generata.").arg(m_bibTool));
            return;
        }
        m_phase = Phase::Idle;
        showLog(tr("Impossibile avviare il compilatore LaTeX.\n%1").arg(m_proc.errorString()));
    });
}

LatexView::~LatexView()
{
    m_proc.disconnect(this);
    m_proc.kill();
    m_proc.waitForFinished(1000);
}

QString LatexView::detectEngine() const
{
    QString engine = QStringLiteral("pdflatex");

    QFile f(m_path);
    if (f.open(QIODevice::ReadOnly)) {
        static const QRegularExpression magic(
            QStringLiteral(R"(^%\s*!TEX\s+(?:TS-)?program\s*=\s*(\w+))"),
            QRegularExpression::CaseInsensitiveOption);
        // Il commento magico sta per convenzione nelle prime righe.
        for (int i = 0; i < 20 && !f.atEnd(); ++i) {
            const auto m = magic.match(QString::fromUtf8(f.readLine()).trimmed());
            if (m.hasMatch()) {
                engine = m.captured(1).toLower();
                break;
            }
        }
    }
    return engine;
}

bool LatexView::load()
{
    if (!m_outDir.isValid()) {
        m_error = tr("Impossibile creare la cartella temporanea");
        return false;
    }
    m_engine = detectEngine();
    if (QStandardPaths::findExecutable(m_engine).isEmpty()) {
        m_error = tr("Compilatore \"%1\" non trovato nel PATH").arg(m_engine);
        return false;
    }

    if (m_proc.state() != QProcess::NotRunning) {
        m_pending = true;
        return true;
    }
    m_pass = 0;
    m_minPasses = 0;
    m_bibWarning.clear();
    startPass();
    return true;
}

void LatexView::startPass()
{
    ++m_pass;
    m_phase = Phase::Latex;
    m_error.clear();
    m_proc.setWorkingDirectory(QFileInfo(m_path).absolutePath());
    m_proc.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    m_proc.start(m_engine, {
        QStringLiteral("-interaction=nonstopmode"),
        QStringLiteral("-halt-on-error"),
        QStringLiteral("-file-line-error"),
        QStringLiteral("-synctex=1"),
        QStringLiteral("-output-directory=") + m_outDir.path(),
        QFileInfo(m_path).fileName(),
    });
    emit statusChanged();
}

QString LatexView::outFile(const QString &suffix) const
{
    return m_outDir.filePath(QFileInfo(m_path).completeBaseName() + suffix);
}

void LatexView::onFinished(int exitCode, QProcess::ExitStatus status)
{
    const QString log = QString::fromLocal8Bit(m_proc.readAll());

    if (m_pending) {
        m_pending = false;
        m_pass = 0;
        m_minPasses = 0;
        m_bibWarning.clear();
        startPass();
        return;
    }

    const bool ok = status == QProcess::NormalExit && exitCode == 0;
    if (m_phase == Phase::Bib) {
        // bibtex esce con 1 per i soli avvisi: non è un fallimento.
        onBibFinished(status == QProcess::NormalExit && exitCode <= (m_bibTool == u"bibtex"_s ? 1 : 0), log);
        return;
    }
    onLatexFinished(ok, log);
}

void LatexView::onLatexFinished(bool ok, const QString &log)
{
    if (!ok) {
        m_phase = Phase::Idle;
        showLog(log);
        return;
    }

    // Dopo la prima passata il .aux (o il .bcf) dice se c'è una bibliografia da generare.
    if (m_pass == 1) {
        const QString tool = bibTool();
        if (!tool.isEmpty()) {
            const QByteArray sig = bibSignature(tool);
            if (sig != m_bibSignature || !QFileInfo::exists(outFile(u".bbl"_s))) {
                m_bibSignature = sig;
                startBib(tool);
                return;
            }
        }
    }

    static const QRegularExpression rerun(QStringLiteral(
        "Rerun to get|Label\\(s\\) may have changed|Please rerun LaTeX"));
    if (m_pass < m_minPasses || (m_pass < MaxPasses && rerun.match(log).hasMatch())) {
        startPass();
        return;
    }
    finish(log);
}

void LatexView::finish(const QString &log)
{
    m_phase = Phase::Idle;
    m_log->hide();
    const QString pdf = outFile(u".pdf"_s);
    m_hasPdf = loadPdf(pdf);
    if (!m_hasPdf) {
        showLog(log);
        return;
    }
    m_synctex.load(pdf);
    updateWatchedFiles();

    if (!m_bibWarning.isEmpty()) {
        m_log->setPlainText(m_bibWarning);
        m_log->show();
        emit notice(tr("Bibliografia con errori: vedi il pannello sotto il documento"));
    }
    emit statusChanged();
    if (m_pendingForward)
        runForwardSearch();
}

QString LatexView::bibTool() const
{
    // biblatex con biber scrive il .bcf; bibtex classico (e biblatex con backend=bibtex) \bibdata nel .aux.
    if (QFileInfo::exists(outFile(u".bcf"_s)))
        return u"biber"_s;
    QFile aux(outFile(u".aux"_s));
    if (aux.open(QIODevice::ReadOnly) && aux.readAll().contains("\\bibdata{"))
        return u"bibtex"_s;
    return {};
}

QStringList LatexView::bibFiles() const
{
    const QDir srcDir(QFileInfo(m_path).absolutePath());
    QStringList files;
    auto add = [&](QString name) {
        name = name.trimmed();
        if (name.isEmpty())
            return;
        if (!name.endsWith(u".bib"_s))
            name += u".bib"_s;
        const QString abs = srcDir.absoluteFilePath(name);
        if (QFileInfo::exists(abs))   // i .bib di sistema non cambiano: non servono
            files << QDir::cleanPath(abs);
    };

    QFile bcf(outFile(u".bcf"_s));
    if (bcf.open(QIODevice::ReadOnly)) {
        static const QRegularExpression ds(QStringLiteral("<bcf:datasource[^>]*>([^<]+)</bcf:datasource>"));
        for (auto it = ds.globalMatch(QString::fromUtf8(bcf.readAll())); it.hasNext();)
            add(it.next().captured(1));
        return files;
    }
    QFile aux(outFile(u".aux"_s));
    if (aux.open(QIODevice::ReadOnly)) {
        static const QRegularExpression bibdata(QStringLiteral(R"(\\bibdata\{([^}]*)\})"));
        for (auto it = bibdata.globalMatch(QString::fromUtf8(aux.readAll())); it.hasNext();)
            for (const QString &n : it.next().captured(1).split(u','))
                add(n);
    }
    return files;
}

QByteArray LatexView::bibSignature(const QString &tool) const
{
    // Si rigenera la bibliografia solo se cambiano le citazioni o i .bib:
    // bibtex/biber a ogni salvataggio raddoppierebbero i tempi.
    QCryptographicHash h(QCryptographicHash::Sha1);
    h.addData(tool.toUtf8());
    if (tool == u"biber") {
        QFile bcf(outFile(u".bcf"_s));
        if (bcf.open(QIODevice::ReadOnly))
            h.addData(bcf.readAll());
    } else {
        // Righe rilevanti del .aux principale e di quelli dei capitoli (\include).
        static const QRegularExpression rel(QStringLiteral(R"(^\\(citation|bibdata|bibstyle)\{.*$)"),
                                            QRegularExpression::MultilineOption);
        QDirIterator it(m_outDir.path(), {u"*.aux"_s}, QDir::Files, QDirIterator::Subdirectories);
        QStringList auxFiles;
        while (it.hasNext())
            auxFiles << it.next();
        auxFiles.sort();
        for (const QString &f : std::as_const(auxFiles)) {
            QFile aux(f);
            if (!aux.open(QIODevice::ReadOnly))
                continue;
            for (auto m = rel.globalMatch(QString::fromUtf8(aux.readAll())); m.hasNext();)
                h.addData(m.next().captured(0).toUtf8());
        }
    }
    for (const QString &bib : bibFiles())
        h.addData((bib + QString::number(QFileInfo(bib).lastModified().toMSecsSinceEpoch())).toUtf8());
    return h.result();
}

void LatexView::startBib(const QString &tool)
{
    m_phase = Phase::Bib;
    m_bibTool = tool;
    const QString srcDir = QFileInfo(m_path).absolutePath();
    const QString base = QFileInfo(m_path).completeBaseName();

    if (tool == u"bibtex") {
        // bibtex lavora nella cartella del .aux; .bib e .bst li cerca con kpathsea.
        // Il ':' finale conserva i percorsi predefiniti di TeX Live.
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char *var : {"BIBINPUTS", "BSTINPUTS"})
            env.insert(QLatin1String(var), srcDir + u':' + env.value(QLatin1String(var)));
        m_proc.setProcessEnvironment(env);
        m_proc.setWorkingDirectory(m_outDir.path());
        m_proc.start(tool, {base});
    } else {
        m_proc.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        m_proc.setWorkingDirectory(srcDir);
        m_proc.start(tool, {u"--input-directory="_s + srcDir, u"--output-directory="_s + m_outDir.path(), base});
    }
    emit statusChanged();
}

void LatexView::onBibFinished(bool ok, const QString &log)
{
    if (!ok) {
        // Si va avanti comunque: un PDF con le citazioni a "?" è meglio di nessun PDF.
        static const QRegularExpression interesting(QStringLiteral(
            R"(^(I couldn't|I found no|I was expecting|Repeated entry|Warning--|---line|.*\b(ERROR|WARN)\b).*$)"),
            QRegularExpression::MultilineOption);
        QStringList lines;
        for (auto it = interesting.globalMatch(log); it.hasNext();)
            lines << it.next().captured(0);
        m_bibWarning = tr("Errori di %1:\n").arg(m_bibTool)
                     + (lines.isEmpty() ? log.trimmed() : lines.join(u'\n'));
        m_bibSignature.clear();   // al prossimo salvataggio si riprova
    }
    m_minPasses = m_pass + 2;
    startPass();
}

void LatexView::updateWatchedFiles()
{
    // Capitoli inclusi e .bib del progetto: salvarli deve ricompilare come salvare il .tex.
    const QString srcDir = QFileInfo(m_path).absolutePath() + u'/';
    QStringList files;
    for (const QString &f : m_synctex.inputs())
        if (f.startsWith(srcDir) && f.endsWith(u".tex"_s))
            files << f;
    files += bibFiles();
    setExtraWatchedFiles(files);
}

bool LatexView::usesSource(const QString &file)
{
    const QString abs = QDir::cleanPath(QFileInfo(file).absoluteFilePath());
    return abs == m_path || m_synctex.hasSource(abs);
}

void LatexView::forwardSearch(const QString &file, int line, int column)
{
    m_pendingForward = Forward{QFileInfo(file).absoluteFilePath(), line, column};
    // Durante la compilazione il PDF e il .synctex stanno per cambiare: si aspetta finish().
    if (m_hasPdf && m_phase == Phase::Idle)
        runForwardSearch();
}

void LatexView::runForwardSearch()
{
    const Forward fw = *m_pendingForward;
    m_pendingForward.reset();

    // Una riga vuota o un commento non producono nulla: si prova con le righe vicine.
    QList<SyncTex::Box> boxes;
    for (int delta : {0, 1, -1, 2, -2, 3, -3}) {
        boxes = m_synctex.boxesFor(fw.file, fw.line + delta, fw.column);
        if (!boxes.isEmpty())
            break;
    }
    if (boxes.isEmpty()) {
        emit notice(tr("SyncTeX: nessuna corrispondenza per %1:%2").arg(QFileInfo(fw.file).fileName()).arg(fw.line));
        return;
    }

    // Una riga del sorgente può spezzarsi su più righe (o pagine) del PDF: si mostra la prima pagina.
    const int page = boxes.first().page;
    QList<QRectF> rects;
    for (const SyncTex::Box &b : std::as_const(boxes))
        if (b.page == page)
            rects << b.rect;
    scrollToLocation(page, rects.first().top(), viewport()->height() / 3);
    flashRects(page, rects);
}

bool LatexView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton && (me->modifiers() & Qt::ControlModifier)) {
            inverseSearch(me->position().toPoint());
            return true;
        }
    }
    return PdfView::eventFilter(watched, event);
}

void LatexView::inverseSearch(QPoint viewportPos)
{
    const auto where = pagePointAt(viewportPos);
    if (!where)
        return;
    const auto src = m_synctex.sourceAt(where->first, where->second);
    if (!src) {
        emit notice(tr("SyncTeX: nessuna riga del sorgente in questo punto"));
        return;
    }

    QStringList args = QProcess::splitCommand(editorCommand());
    if (args.isEmpty())
        return;
    for (QString &a : args) {
        a.replace(u"%f"_s, src->file)
         .replace(u"%l"_s, QString::number(src->line))
         .replace(u"%c"_s, QString::number(qMax(1, src->column)));
    }
    const QString program = args.takeFirst();
    if (!QProcess::startDetached(program, args, QFileInfo(src->file).absolutePath()))
        emit notice(tr("Impossibile avviare l'editor \"%1\": controlla il comando in Impostazioni").arg(program));
    else
        emit notice(tr("Apertura di %1:%2").arg(QFileInfo(src->file).fileName()).arg(src->line));
}

QString LatexView::editorCommand()
{
    return QSettings().value("synctex/editor", DefaultEditor).toString();
}

void LatexView::setEditorCommand(const QString &command)
{
    QSettings().setValue("synctex/editor", command.trimmed());
}

QStringList LatexView::editorPresets()
{
    return {
        DefaultEditor,
        QStringLiteral("code --goto %f:%l:%c"),
        QStringLiteral("kate --line %l --column %c %f"),
        QStringLiteral("nvim --server /tmp/nvim.sock --remote-send \"<C-\\><C-N>:e %f<CR>%lG\""),
        QStringLiteral("emacsclient -n +%l:%c %f"),
        QStringLiteral("subl %f:%l:%c"),
    };
}

void LatexView::showLog(const QString &log)
{
    // Le righe "file:riga: messaggio" (da -file-line-error) e "! ..." sono quelle utili.
    QStringList errors;
    static const QRegularExpression errLine(QStringLiteral(R"(^(\S+:\d+:.*|!.*)$)"),
                                            QRegularExpression::MultilineOption);
    for (auto it = errLine.globalMatch(log); it.hasNext();)
        errors << it.next().captured(1);

    m_error = errors.isEmpty() ? tr("Compilazione fallita") : errors.first();
    m_log->setPlainText(errors.isEmpty() ? log : errors.join('\n') + QStringLiteral("\n\n— log completo —\n") + log);
    m_log->show();
    emit statusChanged();
}

QString LatexView::statusText() const
{
    if (m_phase == Phase::Latex)
        return tr("Compilazione con %1 (passata %2)…").arg(m_engine).arg(m_pass);
    if (m_phase == Phase::Bib)
        return tr("Compilazione della bibliografia con %1…").arg(m_bibTool);
    if (!m_error.isEmpty())
        return tr("Errore LaTeX: %1").arg(m_error);
    QString s = PdfView::statusText();
    if (!m_bibWarning.isEmpty())
        s += tr(" — bibliografia con errori");
    if (m_synctex.isValid())
        s += tr(" — Ctrl+clic: vai al sorgente");
    return s;
}
