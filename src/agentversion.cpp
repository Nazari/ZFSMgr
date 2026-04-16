#include "agentversion.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QVector>

namespace {

constexpr const char* kAgentSchemaMarker =
    "linux-service:/etc/systemd/system/zfsmgr-agent.service\n"
    "linux-bin:/usr/local/libexec/zfsmgr-agent\n"
    "linux-config:/etc/zfsmgr/agent.toml\n"
    "linux-state:/var/lib/zfsmgr-agent/state.db\n"
    "mac-plist:/Library/LaunchDaemons/org.zfsmgr.agent.plist\n"
    "freebsd-rc:/usr/local/etc/rc.d/zfsmgr_agent\n"
    "win-service:ZFSMgrAgent\n"
    "tls:mtls-required\n"
    "api:v1\n";

QVector<int> versionOrderingKey(const QString& version) {
    QVector<int> out;
    const QRegularExpression rx(QStringLiteral("^(\\d+)\\.(\\d+)\\.(\\d+)(?:rc(\\d+))?(?:[.-](\\d+))?$"),
                                QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = rx.match(version.trimmed());
    if (!m.hasMatch()) {
        return out;
    }
    out << m.captured(1).toInt()
        << m.captured(2).toInt()
        << m.captured(3).toInt();
    out << (m.captured(4).isEmpty() ? 999999 : m.captured(4).toInt());
    out << (m.captured(5).isEmpty() ? 0 : m.captured(5).toInt());
    return out;
}

QString versionFingerprintSuffix() {
    const QByteArray digest = QCryptographicHash::hash(QByteArray(kAgentSchemaMarker), QCryptographicHash::Sha256).toHex();
    bool ok = false;
    const quint32 raw = digest.left(8).toUInt(&ok, 16);
    const quint32 folded = (ok ? raw : 0u) % 1000000000u;
    return QString::number(folded);
}

} // namespace

namespace agentversion {

QString currentVersion() {
    return QStringLiteral(ZFSMGR_APP_VERSION) + QStringLiteral(".") + versionFingerprintSuffix();
}

QString expectedApiVersion() {
    return QStringLiteral("1");
}

int compareVersions(const QString& a, const QString& b) {
    const QVector<int> ka = versionOrderingKey(a);
    const QVector<int> kb = versionOrderingKey(b);
    if (ka.isEmpty() || kb.isEmpty()) {
        const int cmp = QString::compare(a.trimmed(), b.trimmed(), Qt::CaseInsensitive);
        return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    }
    const int n = std::min(ka.size(), kb.size());
    for (int i = 0; i < n; ++i) {
        if (ka.at(i) < kb.at(i)) {
            return -1;
        }
        if (ka.at(i) > kb.at(i)) {
            return 1;
        }
    }
    if (ka.size() == kb.size()) {
        return 0;
    }
    return ka.size() < kb.size() ? -1 : 1;
}

} // namespace agentversion

