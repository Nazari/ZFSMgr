#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QTcpSocket>

struct ConnectionProfile;

namespace zfsmgr {

struct DaemonRpcResult {
    int code{0};
    QString message;
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

private:
    bool sendRpc(const QString& host,
                 const QString& method,
                 const QJsonObject& params,
                 QJsonObject& outPayload);
    bool writeRequest(QTcpSocket& socket, const QString& method, const QJsonObject& params);
    bool readResponse(QTcpSocket& socket, QJsonObject& response);

    int m_port{32099};
    bool m_enabled{true};
    QString m_lastError;
};
} // namespace zfsmgr
