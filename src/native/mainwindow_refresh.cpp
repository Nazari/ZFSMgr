#include "mainwindow.h"
#include "base/helpers.h"

#include "commands/refreshparse.h"
#include "commands/zfsmurl.h"
#include "base/osinfo.h"

namespace BR = zfsmgr::base::refresh;
#include "mainwindow_helpers.h"
#include "requests.h"
#include "daemonpayload.h"
#include "agentversion.h"

#include <algorithm>
#include <QElapsedTimer>
#include <QFuture>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>

namespace {
using mwhelpers::oneLine;

// Los analizadores viven en `src/base/refreshparse.cpp`, sin Qt. Aquí solo se convierte.
//
// De aquí desaparecieron además refreshCompareAppVersions y refreshVersionOrderingKey:
// no las llamaba nadie. Parecían el resto de una función pensada —distinguir un agente
// más viejo de uno más nuevo— que nunca llegó a usarse, porque la comprobación de
// versión que hay hoy es de igualdad exacta. Están en el historial si hacen falta.

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string bs(const QString& s) { return s.toStdString(); }

QString extractMachineUuid(const QString& text) { return qs(BR::extractMachineUuid(bs(text))); }

struct PoolGuidStatusEntry {
    QString guid;
    QString status;
};

} // namespace

int MainWindow::findConnectionIndexByName(const QString& name) const {
    const QString key = name.trimmed();
    for (int i = 0; i < m_conns.profiles.size(); ++i) {
        if (m_conns.profiles[i].name.compare(key, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

ConnectionRuntimeState MainWindow::refreshConnection(const ConnectionProfile& p) {
    // Everything from the shell, on the read process of this connection: `refresh` for the
    // machine and its daemon, `ls` for its pools —imported and importable— and `status` for
    // each imported one. The window probes nothing by itself: no uname, no os-release, no
    // service manager script. A machine whose daemon does not answer has no data, and the
    // shell says why.
    ConnectionRuntimeState state;
    state.connectionMethod = p.connType;
    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_inicio_ref_521ce1"), QStringLiteral("Inicio refresh: %1 [%2]"),
               QStringLiteral("Refresh start: %1 [%2]"))
               .arg(p.name, p.connType));
    QElapsedTimer reloj;
    reloj.start();
    const QString clave = cliQuote(p.id.isEmpty() ? p.name : p.id);
    zfsmgr::base::ZfsmUrl u;
    u.connection = (p.id.isEmpty() ? p.name : p.id).toStdString();
    u.kind = zfsmgr::base::ZfsmKind::Connection;
    const QString url = QString::fromStdString(zfsmgr::base::formatZfsmUrl(u));
    const auto lee = [this, &clave](const QString& linea, QJsonObject& out, QString* error,
                                    int plazo) {
        const CliService::Result r = m_cliReader->read(clave, linea, plazo);
        if (error) {
            *error = r.rc == 0
                         ? QString()
                         : oneLine(r.err.isEmpty() ? QStringLiteral("rc=%1").arg(r.rc) : r.err);
        }
        const QJsonDocument doc = QJsonDocument::fromJson(r.out.toUtf8());
        out = doc.object();
        return doc.isObject();
    };

    // 1. The machine and its daemon.
    QJsonObject r;
    QString error;
    if (!lee(QStringLiteral("refresh --on %1").arg(url), r, &error, 60000) || !error.isEmpty()) {
        state.status = QStringLiteral("ERROR");
        state.detail = error.isEmpty() ? QStringLiteral("refresh") : error;
        state.daemonNeedsAttention = true;
        state.daemonAttentionReasons.push_back(state.detail);
        appLog(QStringLiteral("WARN"),
               QStringLiteral("Fin refresh: %1 -> ERROR (%2)").arg(p.name, state.detail));
        return state;
    }
    QMap<QString, QString> kv;
    for (const QJsonValue& v : r.value(QStringLiteral("refresh")).toArray()) {
        const QJsonObject o = v.toObject();
        kv.insert(o.value(QStringLiteral("field")).toString(),
                  o.value(QStringLiteral("value")).toString().trimmed());
    }
    state.osLine = kv.value(QStringLiteral("os_line"));
    state.status = QStringLiteral("OK");
    state.detail = state.osLine;
    state.machineUuid = extractMachineUuid(kv.value(QStringLiteral("machine_uuid")));
    state.zfsUnavailableDetail = oneLine(kv.value(QStringLiteral("zfs_detail")));
    const QString zfsRaw = kv.value(QStringLiteral("zfs_version_raw"));
    state.zfsVersion = mwhelpers::parseOpenZfsVersionText(zfsRaw);
    state.zfsVersionFull = oneLine(zfsRaw);
    state.daemonVersion = kv.value(QStringLiteral("version"));
    state.daemonApiVersion = kv.value(QStringLiteral("api"));
    state.daemonInstalled = state.daemonActive =
        kv.value(QStringLiteral("status")).compare(QStringLiteral("OK"), Qt::CaseInsensitive) == 0;
    state.daemonNativeBinary = state.daemonInstalled;
    state.daemonJobsSupported = kv.value(QStringLiteral("jobs_support")) == QStringLiteral("1");
    for (const QString& c :
         kv.value(QStringLiteral("caps")).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        state.daemonCaps.insert(c.trimmed());
    }
    state.daemonLastSeenZedEvent = kv.value(QStringLiteral("zed_last_event_utc"));
    state.daemonDetail =
        QStringLiteral("cache=%1/%2 inval=%3 pool_inval=%4 rpc_fail=%5 zed_active=%6 zed_last=%7")
            .arg(kv.value(QStringLiteral("cache_entries"), QStringLiteral("-")),
                 kv.value(QStringLiteral("cache_max_entries"), QStringLiteral("-")),
                 kv.value(QStringLiteral("cache_invalidations"), QStringLiteral("-")),
                 kv.value(QStringLiteral("pool_invalidations"), QStringLiteral("-")),
                 kv.value(QStringLiteral("rpc_failures"), QStringLiteral("-")),
                 kv.value(QStringLiteral("zed_active"), QStringLiteral("-")),
                 kv.value(QStringLiteral("zed_last_event_utc"), QStringLiteral("-")));
    if (!state.daemonActive) {
        state.daemonNeedsAttention = true;
        state.daemonAttentionReasons.push_back(
            QStringLiteral("el agente no está en marcha: sin él no hay datos"));
    }
    // The version: the same verdict the shell's status zone gives.
    const QString esperada = agentversion::currentVersion().trimmed();
    if (!state.daemonVersion.isEmpty() && state.daemonVersion != esperada) {
        const auto baseDe = [](const QString& v) {
            const QStringList x = v.split(QLatin1Char('.'));
            return x.size() >= 3 ? QStringList{x[0], x[1], x[2]}.join(QLatin1Char('.')) : v;
        };
        if (agentversion::compareVersions(baseDe(state.daemonVersion), baseDe(esperada)) <= 0) {
            state.daemonNeedsAttention = true;
            state.daemonAttentionReasons.push_back(QStringLiteral("versión daemon desactualizada"));
        }
    }
    const QString apiEsperada = agentversion::expectedApiVersion().trimmed();
    if (!apiEsperada.isEmpty() && !state.daemonApiVersion.isEmpty()
        && state.daemonApiVersion != apiEsperada) {
        state.daemonNeedsAttention = true;
        state.daemonAttentionReasons.push_back(QStringLiteral("API daemon incompatible"));
    }

    // 2. Its pools, imported and importable, with their guids.
    QJsonObject pools;
    if (lee(QStringLiteral("ls %1").arg(url), pools, &error, 90000)) {
        for (const QJsonValue& v : pools.value(QStringLiteral("pools")).toArray()) {
            const QJsonObject o = v.toObject();
            const QString nombre = o.value(QStringLiteral("name")).toString();
            const QString guid = o.value(QStringLiteral("guid")).toString();
            if (o.value(QStringLiteral("imported")).toBool()) {
                state.importedPools.push_back(
                    PoolImported{p.name, nombre, QStringLiteral("Exportar")});
                if (!guid.isEmpty()) {
                    state.poolGuidByName.insert(nombre, guid);
                }
            } else {
                state.importablePools.push_back(PoolImportable{
                    p.name, nombre, guid, o.value(QStringLiteral("state")).toString(),
                    o.value(QStringLiteral("reason")).toString(), QStringLiteral("Importar")});
            }
        }
    } else {
        appLog(QStringLiteral("WARN"), QStringLiteral("%1: ls -> %2").arg(p.name, error));
    }

    // 3. The `zpool status` of each imported pool: its tooltip, and whether it is degraded.
    for (const PoolImported& pool : std::as_const(state.importedPools)) {
        const CliService::Result st = m_cliReader->read(
            clave, QStringLiteral("status --on %1/%2").arg(url, pool.pool), 30000);
        state.poolStatusByName.insert(pool.pool, (st.rc == 0 ? st.out : st.err).trimmed());
    }

    appLog(QStringLiteral("NORMAL"),
           trk(QStringLiteral("t_fin_refres_6eead9"), QStringLiteral("Fin refresh: %1 -> OK (%2)"),
               QStringLiteral("Refresh end: %1 -> OK (%2)"))
               .arg(p.name, state.detail));
    appLog(QStringLiteral("INFO"),
           QStringLiteral("%1: refresh %2 ms").arg(p.name).arg(reloj.elapsed()));
    return state;
}
