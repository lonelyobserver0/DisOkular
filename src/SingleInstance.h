#pragma once

#include <QJsonObject>
#include <QLocalServer>

// Una sola finestra per utente: le invocazioni successive (es. la ricerca in avanti
// lanciata dall'editor) consegnano la richiesta all'istanza già aperta ed escono.
//
// Richiesta: {"open": [percorsi assoluti],
//             "forward": {"file": …, "line": n, "column": n, "main": … | ""}}
class SingleInstance : public QObject
{
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);

    // true se un'istanza in esecuzione ha ricevuto la richiesta.
    static bool sendToRunning(const QJsonObject &request);

    bool listen();

signals:
    void requestReceived(const QJsonObject &request);

private:
    static QString serverName();

    QLocalServer m_server;
};
