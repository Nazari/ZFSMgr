#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <libssh/libssh.h>

struct ConnectionProfile;

namespace zfsmgr {

struct DaemonRpcResult {
    int code{0};
    QString message;
    QString fallback;
    QJsonObject payload;
};

class DaemonTransport : public QObject {
    Q_OBJECT

public:
    explicit DaemonTransport(QObject* parent = nullptr);

    void setDaemonPort(int port);
    int daemonPort() const;

    bool isDaemonAvailable() const;
    QString lastError() const;

    bool probeDaemon(const ConnectionProfile& profile, QString* detail);
    DaemonRpcResult call(const QString& method,
                         const QJsonObject& params,
                         const ConnectionProfile& profile);
    void setEnabled(bool enabled);

private:
    bool sendRpc(const QString& host,
                 const QString& method,
                 const QJsonObject& params,
                 QJsonObject& outPayload);
    bool openSession(ssh_session& session, const QString& host);
    bool authenticateSession(ssh_session session);
    QString privateKeyPath() const;

    int m_port{32099};
    bool m_enabled{true};
    QString m_lastError;
};
} // namespace zfsmgr
