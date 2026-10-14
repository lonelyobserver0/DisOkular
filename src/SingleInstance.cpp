#include "SingleInstance.h"

#include <QJsonDocument>
#include <QLocalSocket>

static constexpr int ConnectTimeoutMs = 300;
static constexpr qsizetype MaxRequestSize = 64 * 1024;

SingleInstance::SingleInstance(QObject *parent)
    : QObject(parent)
{
    // Solo lo stesso utente può parlare con il socket.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (socket->bytesAvailable() > MaxRequestSize) {
                    socket->abort();
                    return;
                }
                if (!socket->canReadLine())
                    return;
                const QJsonDocument doc = QJsonDocument::fromJson(socket->readLine());
                socket->disconnectFromServer();
                if (doc.isObject())
                    emit requestReceived(doc.object());
            });
        }
    });
}

QString SingleInstance::serverName()
{
    return QStringLiteral("disokular-") + qEnvironmentVariable("USER", QStringLiteral("default"));
}

bool SingleInstance::sendToRunning(const QJsonObject &request)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(ConnectTimeoutMs))
        return false;
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(ConnectTimeoutMs))
        return false;
    socket.waitForDisconnected(ConnectTimeoutMs);
    return true;
}

bool SingleInstance::listen()
{
    if (m_server.listen(serverName()))
        return true;
    // Un socket rimasto da un'istanza terminata male: nessuno ha risposto, quindi è orfano.
    QLocalServer::removeServer(serverName());
    return m_server.listen(serverName());
}
