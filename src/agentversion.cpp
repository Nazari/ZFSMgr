#include "agentversion.h"

#include <QRegularExpression>
#include <QVector>

namespace {

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

} // namespace

namespace agentversion {

QString currentVersion() {
    return QStringLiteral(ZFSMGR_AGENT_VERSION_STRING);
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
