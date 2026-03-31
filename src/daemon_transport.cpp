#include "daemon_transport.h"

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
    Q_UNUSED(profile)
    if (detail) {
        *detail = QStringLiteral("daemon transport not implemented yet");
    }
    m_lastError = QStringLiteral("daemon probe not implemented");
    return false;
}

DaemonRpcResult DaemonTransport::call(const QString& method,
                                      const QJsonObject& params,
                                      const ConnectionProfile& profile) {
    Q_UNUSED(params)
    Q_UNUSED(profile)
    DaemonRpcResult result;
    result.code = 501;
    result.message = QStringLiteral("daemon RPC not implemented: %1").arg(method);
    return result;
}

} // namespace zfsmgr
