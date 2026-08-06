#include "agentversion.h"

#include <QFile>

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
    return QStringLiteral("3");
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


QString versionFromBinary(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    // El sufijo de esquema va detrás de la versión de la aplicación, así que basta con
    // buscar ese prefijo y leer los dígitos que siguen. Se lee entero: son unos pocos MB
    // y buscar por trozos podría partir la cadena justo por la mitad.
    const QByteArray blob = f.readAll();
    const QByteArray prefix = (QStringLiteral(ZFSMGR_APP_VERSION) + QLatin1Char('.')).toLatin1();
    int at = blob.indexOf(prefix);
    while (at >= 0) {
        int end = at + prefix.size();
        while (end < blob.size() && blob.at(end) >= '0' && blob.at(end) <= '9') {
            ++end;
        }
        if (end > at + prefix.size()) {
            return QString::fromLatin1(blob.mid(at, end - at));
        }
        at = blob.indexOf(prefix, at + 1);
    }
    return QString();
}

} // namespace agentversion
