#include "daemon_transport.h"
#include "connectionstore.h"

#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

namespace zfsmgr {

DaemonTransport::DaemonTransport(QObject* parent)
    : QObject(parent) {}

void DaemonTransport::setDaemonPort(int port) {
    m_port = port;
}

int DaemonTransport::daemonPort() const {
    return m_port;
}

bool DaemonTransport::isDaemonAvailable() const {
    return m_enabled;
}

QString DaemonTransport::lastError() const {
    return m_lastError;
}

bool DaemonTransport::probeDaemon(const ConnectionProfile& profile, QString* detail) {
    Q_UNUSED(detail);
    QJsonObject res;
    if (!sendRpc(profile.host, QStringLiteral("/health"), {}, res)) {
        return false;
    }
    return true;
}

DaemonRpcResult DaemonTransport::call(const QString& method,
                                      const QJsonObject& params,
                                      const ConnectionProfile& profile) {
    DaemonRpcResult result;
    QJsonObject payload;
    if (!sendRpc(profile.host, method, params, payload)) {
        result.code = 500;
        result.message = m_lastError;
        return result;
    }
    result.code = payload.value(QStringLiteral("code")).toInt(200);
    result.message = payload.value(QStringLiteral("message")).toString();
    result.payload = payload.value(QStringLiteral("payload")).toObject();
    return result;
}

bool DaemonTransport::writeRequest(QTcpSocket& socket, const QString& method, const QJsonObject& params) {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject req;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("method"), method);
    req.insert(QStringLiteral("params"), params);
    const QByteArray body = QJsonDocument(req).toJson(QJsonDocument::Compact);
    const QByteArray line = body + '\n';
    socket.write(line);
    return socket.waitForBytesWritten(1000);
}

bool DaemonTransport::readResponse(QTcpSocket& socket, QJsonObject& response) {
    if (!socket.waitForReadyRead(2000)) {
        m_lastError = QStringLiteral("daemon timeout waiting for response");
        return false;
    }
    const QByteArray data = socket.readAll();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        m_lastError = QStringLiteral("invalid JSON response from daemon");
        return false;
    }
    response = doc.object();
    return true;
}

bool DaemonTransport::sendRpc(const QString& host,
                              const QString& method,
                              const QJsonObject& params,
                              QJsonObject& outPayload) {
    QTcpSocket socket;
    socket.connectToHost(host, m_port);
    if (!socket.waitForConnected(1500)) {
        m_lastError = QStringLiteral("daemon not reachable");
        return false;
    }
    if (!writeRequest(socket, method, params)) {
        m_lastError = QStringLiteral("daemon request failed to send");
        return false;
    }
    QJsonObject response;
    if (!readResponse(socket, response)) {
        return false;
    }
    const QJsonValue res = response.value(QStringLiteral("result"));
    if (!res.isObject()) {
        m_lastError = QStringLiteral("daemon response missing result");
        return false;
    }
    outPayload = res.toObject();
    const QJsonValue err = response.value(QStringLiteral("error"));
    if (err.isObject()) {
        const QJsonObject errObj = err.toObject();
        m_lastError = errObj.value(QStringLiteral("message")).toString();
        return false;
    }
    return true;
}

} // namespace zfsmgr
