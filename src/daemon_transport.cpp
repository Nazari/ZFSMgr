#include "connectionstore.h"
#include "daemon_transport.h"
#include "daemon_rpc_protocol.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QUuid>
#include <QStandardPaths>
#include <libssh/libssh.h>

namespace zfsmgr {

DaemonTransport::DaemonTransport(QObject* parent)
    : QObject(parent) {}

void DaemonTransport::setDaemonPort(int port) {
    m_port = port;
}

int DaemonTransport::daemonPort() const {
    return m_port;
}

void DaemonTransport::setEnabled(bool enabled) {
    m_enabled = enabled;
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
    result.fallback = payload.value(QStringLiteral("fallback")).toString();
    result.payload = payload.value(QStringLiteral("payload")).toObject();
    return result;
}

bool DaemonTransport::sendRpc(const QString& host,
                              const QString& method,
                              const QJsonObject& params,
                              QJsonObject& outPayload) {
    ssh_session session = nullptr;
    if (!openSession(session, host)) {
        return false;
    }
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
        m_lastError = ssh_get_error(session);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    if (ssh_channel_open_session(channel) != SSH_OK) {
        m_lastError = ssh_get_error(session);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const daemonrpc::RpcRequest request{id, method, params};
    const QByteArray body = request.toJson();
    if (ssh_channel_write(channel, body.constData(), body.size()) != body.size()) {
        m_lastError = QStringLiteral("failed to write request");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    ssh_channel_send_eof(channel);
    QByteArray response;
    char buf[256];
    int n;
    while ((n = ssh_channel_read(channel, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, n);
    }
    if (n < 0) {
        m_lastError = ssh_get_error(session);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);
    daemonrpc::RpcResponse responseEnvelope;
    QString parseError;
    if (!daemonrpc::RpcResponse::fromJson(response, &responseEnvelope, &parseError)) {
        m_lastError = parseError;
        return false;
    }
    if (responseEnvelope.id != id) {
        m_lastError = QStringLiteral("daemon response id mismatch");
        return false;
    }
    if (responseEnvelope.hasError) {
        m_lastError = responseEnvelope.errorMessage;
        return false;
    }
    outPayload = responseEnvelope.result.toJsonObject();
    return true;
}

bool DaemonTransport::openSession(ssh_session& session, const QString& host) {
    session = ssh_new();
    if (!session) {
        m_lastError = QStringLiteral("failed to allocate ssh session");
        return false;
    }
    int port = m_port;
    ssh_options_set(session, SSH_OPTIONS_HOST, host.toUtf8().constData());
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    ssh_options_set(session, SSH_OPTIONS_USER, QStringLiteral("zfsmgr").toUtf8().constData());
    if (ssh_connect(session) != SSH_OK) {
        m_lastError = ssh_get_error(session);
        ssh_free(session);
        return false;
    }
    if (!authenticateSession(session)) {
        ssh_disconnect(session);
        ssh_free(session);
        return false;
    }
    return true;
}

bool DaemonTransport::authenticateSession(ssh_session session) {
    ssh_key key = nullptr;
    const QString keyPath = privateKeyPath();
    if (!QFile::exists(keyPath)) {
        m_lastError = QStringLiteral("daemon key missing");
        return false;
    }
    if (ssh_pki_import_privkey_file(keyPath.toUtf8().constData(), nullptr, nullptr, nullptr, &key) != SSH_OK) {
        m_lastError = QStringLiteral("unable to load private key");
        return false;
    }
    if (ssh_userauth_publickey(session, nullptr, key) != SSH_AUTH_SUCCESS) {
        m_lastError = ssh_get_error(session);
        ssh_key_free(key);
        return false;
    }
    ssh_key_free(key);
    return true;
}

QString DaemonTransport::privateKeyPath() const {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
        .filePath(QStringLiteral(".zfsmgr/zfsmgr_daemon_id"));
}

} // namespace zfsmgr
