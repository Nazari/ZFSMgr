#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QFile>
#include <QTextStream>
#include <QByteArray>
#include <QDebug>
#include <libssh/libssh.h>

static constexpr int kDefaultPort = 32099;

static QString jsonResult(const QString& requestId, int code, const QString& message, const QJsonObject& payload = {}) {
    QJsonObject result;
    result.insert(QStringLiteral("code"), code);
    result.insert(QStringLiteral("message"), message);
    if (!payload.isEmpty()) {
        result.insert(QStringLiteral("payload"), payload);
    }
    QJsonObject root;
    root.insert(QStringLiteral("id"), requestId);
    root.insert(QStringLiteral("result"), result);
    root.insert(QStringLiteral("error"), QJsonValue::Null);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

static QString fingerprintFromBuffer(const QByteArray& buffer) {
    const QByteArray hash = QByteArray::fromBase64(buffer);
    return QString::fromUtf8(hash).toLower();
}

static bool handleRequest(const QByteArray& buffer, QString& response) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(buffer, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject obj = doc.object();
    const QString id = obj.value(QStringLiteral("id")).toString();
    const QString method = obj.value(QStringLiteral("method")).toString();
    if (method == QStringLiteral("/health")) {
        response = jsonResult(id, 200, QStringLiteral("ok"), QJsonObject{{QStringLiteral("status"), QStringLiteral("healthy")}});
        return true;
    }
    if (method == QStringLiteral("/exec")) {
        response = jsonResult(id, 200, QStringLiteral("executed"), QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}});
        return true;
    }
    response = jsonResult(id, 404, QStringLiteral("not implemented"));
    return true;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"port", "Port to listen on", "port", QString::number(kDefaultPort)});
    parser.addOption({"hostkey", "Path to host key", "hostkey", "/etc/zfsmgr/host_rsa"});
    parser.addOption({"fingerprint", "Allowed fingerprint base64", "fingerprint", QStringLiteral("")});
    parser.process(app);

    const int port = parser.value("port").toInt();
    const QString hostKey = parser.value("hostkey");
    const QString allowedFingerprint = parser.value("fingerprint").toLower();
    if (hostKey.isEmpty() || allowedFingerprint.isEmpty()) {
        qWarning() << "hostkey and fingerprint are required";
        return 1;
    }

    ssh_bind bind = ssh_bind_new();
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, "0.0.0.0");
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port);
    ssh_bind_options_set(bind, SSH_BIND_OPTIONS_RSAKEY, hostKey.toUtf8().constData());
    if (ssh_bind_listen(bind) != SSH_OK) {
        qWarning() << "unable to bind" << ssh_get_error(bind);
        ssh_bind_free(bind);
        return 1;
    }

    while (true) {
        ssh_session session = ssh_new();
        if (ssh_bind_accept(bind, session) != SSH_OK) {
            qWarning() << "accept failed" << ssh_get_error(bind);
            ssh_free(session);
            continue;
        }
        if (ssh_handle_key_exchange(session) != SSH_OK) {
            qWarning() << "key exchange failed" << ssh_get_error(session);
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }
        unsigned char* hash = nullptr;
        size_t hlen = 0;
        ssh_key pubkey = ssh_get_publickey(session);
        bool authorized = false;
        if (pubkey && ssh_get_publickey_hash(pubkey, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) == SSH_OK) {
            const QByteArray fingerprint = QByteArray::fromRawData(reinterpret_cast<char*>(hash), hlen).toBase64();
            ssh_clean_pubkey_hash(&hash);
            authorized = fingerprint.toLower() == allowedFingerprint;
        }
        if (!authorized) {
            qWarning() << "unauthorized fingerprint";
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }
        ssh_channel channel = ssh_channel_new(session);
        if (!channel || ssh_channel_open_session(channel) != SSH_OK) {
            qWarning() << "channel open failed" << ssh_get_error(session);
            if (channel) ssh_channel_free(channel);
            ssh_disconnect(session);
            ssh_free(session);
            continue;
        }
        QByteArray payload;
        char buffer[512];
        int n;
        while ((n = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            payload.append(buffer, n);
        }
        QString response;
        if (!handleRequest(payload, response)) {
            response = jsonResult(QStringLiteral("-"), 400, QStringLiteral("bad request"));
        }
        ssh_channel_write(channel, response.toUtf8().constData(), response.size());
        ssh_channel_send_eof(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(session);
        ssh_free(session);
    }
    ssh_bind_free(bind);
    return 0;
}
