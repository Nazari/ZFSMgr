#include "mainwindow_helpers.h"

#include <QDir>
#include <QRegularExpression>

namespace mwhelpers {

QString oneLine(const QString& v, int maxLen) {
    QString x = v.simplified();
    return x.left(maxLen);
}

QString shSingleQuote(const QString& s) {
    QString out = s;
    out.replace('\'', "'\"'\"'");
    return QStringLiteral("'") + out + QStringLiteral("'");
}

bool isMountedValueTrue(const QString& value) {
    const QString v = value.trimmed().toLower();
    return v == QStringLiteral("yes")
        || v == QStringLiteral("on")
        || v == QStringLiteral("true")
        || v == QStringLiteral("1");
}

QString parentDatasetName(const QString& dataset) {
    const int slash = dataset.lastIndexOf('/');
    if (slash <= 0) {
        return QString();
    }
    return dataset.left(slash);
}

QString normalizeDriveLetterValue(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty() || s == QStringLiteral("-") || s.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    s.replace(QStringLiteral(":\\"), QString());
    s.replace(':', QString());
    s.replace('\\', QString());
    s.replace('/', QString());
    s = s.trimmed().toUpper();
    if (s.isEmpty()) {
        return QString();
    }
    const QChar d = s[0];
    if (!d.isLetter()) {
        return QString();
    }
    return QString(d);
}

bool looksLikePowerShellScript(const QString& cmd) {
    const QString c = cmd.toLower();
    const QString t = c.trimmed();
    if (t.startsWith('[') || t.startsWith('$') || c.contains(QStringLiteral("::"))) {
        return true;
    }
    if (t.startsWith(QStringLiteral("zfs "))
        || t == QStringLiteral("zfs")
        || t.startsWith(QStringLiteral("zpool "))
        || t == QStringLiteral("zpool")
        || t.startsWith(QStringLiteral("where.exe "))) {
        return true;
    }
    return c.contains(QStringLiteral("out-null"))
        || c.contains(QStringLiteral("test-path"))
        || c.contains(QStringLiteral("resolve-path"))
        || c.contains(QStringLiteral("join-path"))
        || c.contains(QStringLiteral("new-item"))
        || c.contains(QStringLiteral("remove-item"))
        || c.contains(QStringLiteral("sort-object"))
        || c.contains(QStringLiteral("select-object"))
        || c.contains(QStringLiteral("get-childitem"))
        || c.contains(QStringLiteral("write-output"))
        || c.contains(QStringLiteral("invoke-expression"))
        || c.contains(QStringLiteral("foreach("))
        || c.contains(QStringLiteral("$lastexitcode"))
        || c.contains(QStringLiteral("[string]::"))
        || c.contains(QStringLiteral("$env:path"))
        || c.contains(QStringLiteral("powershell "));
}

QString parseOpenZfsVersionText(const QString& text) {
    if (text.trimmed().isEmpty()) {
        return QString();
    }
    const QString lower = text.toLower();
    const QList<QRegularExpression> patterns = {
        QRegularExpression(QStringLiteral("\\bzfs(?:-kmod)?[-\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
        QRegularExpression(QStringLiteral("\\bopenzfs(?:[-\\s]+version)?[:\\s]+(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
        QRegularExpression(QStringLiteral("\\b(?:zfs|zpool)[^\\r\\n]*?\\b(\\d+\\.\\d+(?:\\.\\d+)?)\\b")),
    };
    for (const QRegularExpression& rx : patterns) {
        const QRegularExpressionMatch m = rx.match(lower);
        if (m.hasMatch()) {
            const QString ver = m.captured(1);
            const int major = ver.section('.', 0, 0).toInt();
            if (major <= 10) {
                return ver;
            }
        }
    }
    return QString();
}

QVector<ImportablePoolInfo> parseZpoolImportOutput(const QString& text) {
    QVector<ImportablePoolInfo> rows;
    const QRegularExpression poolNameRx(QStringLiteral("^[A-Za-z0-9_.:-]+$"));
    QString currentPool;
    QString currentState;
    QString currentReason;
    bool collectingStatus = false;

    auto flushCurrent = [&]() {
        if (currentPool.isEmpty()) {
            return;
        }
        if (!poolNameRx.match(currentPool).hasMatch()) {
            currentPool.clear();
            currentState.clear();
            currentReason.clear();
            collectingStatus = false;
            return;
        }
        if (currentState.isEmpty() && currentReason.isEmpty()) {
            currentPool.clear();
            collectingStatus = false;
            return;
        }
        rows.push_back(ImportablePoolInfo{
            currentPool,
            currentState.isEmpty() ? QStringLiteral("UNKNOWN") : currentState,
            currentReason,
        });
        currentPool.clear();
        currentState.clear();
        currentReason.clear();
        collectingStatus = false;
    };

    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QStringLiteral("pool: "))) {
            flushCurrent();
            currentPool = line.mid(QStringLiteral("pool: ").size()).trimmed();
            continue;
        }
        if (currentPool.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("state: "))) {
            currentState = line.mid(QStringLiteral("state: ").size()).trimmed();
            collectingStatus = false;
            continue;
        }
        if (line.startsWith(QStringLiteral("status: "))) {
            currentReason = line.mid(QStringLiteral("status: ").size()).trimmed();
            collectingStatus = true;
            continue;
        }
        if (collectingStatus) {
            if (line.startsWith(QStringLiteral("action:")) || line.startsWith(QStringLiteral("see:")) || line.startsWith(QStringLiteral("config:"))) {
                collectingStatus = false;
            } else if (!line.isEmpty()) {
                currentReason = (currentReason + QStringLiteral(" ") + line).trimmed();
                continue;
            }
        }
        if (line.startsWith(QStringLiteral("cannot import"))) {
            if (!currentReason.isEmpty()) {
                currentReason += QStringLiteral(" ");
            }
            currentReason += line;
        }
    }
    flushCurrent();
    return rows;
}

QString sshControlPath() {
#ifdef Q_OS_MAC
    return QStringLiteral("/tmp/zfsmgr-%C");
#else
    return QDir::tempPath() + QStringLiteral("/zfsmgr-ssh-%C");
#endif
}

QString sshBaseCommand(const ConnectionProfile& p) {
    QString cmd = QStringLiteral("ssh -o BatchMode=yes -o LogLevel=ERROR -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
                                 " -o ControlMaster=auto -o ControlPersist=300 -o ControlPath=%1")
                      .arg(shSingleQuote(sshControlPath()));
    if (p.port > 0) {
        cmd += QStringLiteral(" -p ") + QString::number(p.port);
    }
    if (!p.keyPath.isEmpty()) {
        cmd += QStringLiteral(" -i ") + shSingleQuote(p.keyPath);
    }
    return cmd;
}

} // namespace mwhelpers
