#include "MainWindow.h"
#include "MarkdownView.h"
#include "SingleInstance.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonArray>

static QString absolute(const QString &path)
{
    return QFileInfo(path).absoluteFilePath();
}

int main(int argc, char *argv[])
{
    MarkdownView::registerUrlScheme();
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("DisOkular"));
    QApplication::setOrganizationName(QStringLiteral("LoneObs"));
    QApplication::setApplicationVersion(QStringLiteral("0.1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Lettore di documenti PDF, Markdown e LaTeX.\n\n"
        "Ricerca in avanti SyncTeX (dall'editor al PDF):\n"
        "  disokular --forward RIGA[:COLONNA] sorgente.tex [principale.tex|.pdf]"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption forward(QStringLiteral("forward"),
                               QStringLiteral("Mostra nel PDF la riga RIGA[:COLONNA] del primo file."),
                               QStringLiteral("riga[:colonna]"));
    QCommandLineOption newInstance(QStringLiteral("new-instance"),
                                   QStringLiteral("Apri una finestra nuova invece di usare quella già aperta."));
    parser.addOption(forward);
    parser.addOption(newInstance);
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("Documenti da aprire"), QStringLiteral("[file...]"));
    parser.process(app);

    // I percorsi vanno resi assoluti qui: l'istanza che li riceve ha un'altra cartella corrente.
    const QStringList files = parser.positionalArguments();
    QJsonObject request;
    if (parser.isSet(forward)) {
        const QStringList pos = parser.value(forward).split(u':');
        bool ok = false;
        const int line = pos.value(0).toInt(&ok);
        if (!ok || line < 1 || files.isEmpty())
            parser.showHelp(1);
        request[QStringLiteral("forward")] = QJsonObject{
            {QStringLiteral("file"), absolute(files.at(0))},
            {QStringLiteral("line"), line},
            {QStringLiteral("column"), pos.value(1).toInt()},
            {QStringLiteral("main"), files.size() > 1 ? absolute(files.at(1)) : QString()},
        };
    } else {
        QJsonArray open;
        for (const QString &f : files)
            open.append(absolute(f));
        request[QStringLiteral("open")] = open;
    }

    if (!parser.isSet(newInstance) && SingleInstance::sendToRunning(request))
        return 0;

    MainWindow w;
    SingleInstance instance;
    if (!parser.isSet(newInstance)) {
        instance.listen();
        QObject::connect(&instance, &SingleInstance::requestReceived, &w, &MainWindow::handleRequest);
    }
    w.show();
    w.handleRequest(request);

    return app.exec();
}
