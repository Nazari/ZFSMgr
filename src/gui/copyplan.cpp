#include "copyplan.h"

#include "text.h"

namespace zfsmgr::gui {

namespace {

QString url(const QString& machine, const QString& dataset = {}) {
    return QStringLiteral("zfsm://") + machine
           + (dataset.isEmpty() ? QString() : QStringLiteral("/") + dataset);
}

QString keepOptions(const CopyKeep& k) {
    return QStringLiteral("--daily %1 --weekly %2 --monthly %3 --yearly %4")
        .arg(k.daily)
        .arg(k.weekly)
        .arg(k.monthly)
        .arg(k.yearly);
}

// A name the CLI will take as one word: ZFS names cannot hold spaces or quotes anyway.
bool plainName(const QString& s) {
    if (s.isEmpty()) {
        return false;
    }
    for (const QChar c : s) {
        if (c.isSpace() || c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('@')
            || c == QLatin1Char('#')) {
            return false;
        }
    }
    return !s.startsWith(QLatin1Char('/')) && !s.endsWith(QLatin1Char('/'));
}

}  // namespace

QString copyProblem(const CopyRequest& r) {
    if (r.originMachine.isEmpty() || r.originDataset.isEmpty()) {
        return L("Choose what to copy.", "Elige qué copiar.");
    }
    if (r.destMachine.isEmpty() || r.destDataset.isEmpty()) {
        return L("Choose where the copy goes.", "Elige adónde va la copia.");
    }
    if (!plainName(r.destDataset) || !r.destDataset.contains(QLatin1Char('/'))) {
        return L("The copy's name must be pool/name, without spaces.",
                 "El nombre de la copia tiene que ser pool/nombre, sin espacios.");
    }
    if (r.originMachine == r.destMachine && r.originDataset == r.destDataset) {
        return L("The copy cannot be the original itself.",
                 "La copia no puede ser el propio original.");
    }
    if (r.originMachine == r.destMachine
        && r.destDataset.startsWith(r.originDataset + QLatin1Char('/'))) {
        return L("The copy cannot go inside the original.",
                 "La copia no puede ir dentro del original.");
    }
    if (r.mode != CopyMode::Once
        && r.keep.daily + r.keep.weekly + r.keep.monthly + r.keep.yearly <= 0) {
        return L("The copy has to keep at least one snapshot.",
                 "La copia tiene que guardar al menos un snapshot.");
    }
    if (r.mode == CopyMode::Once && r.stamp.isEmpty()) {
        return L("Missing the snapshot name.", "Falta el nombre del snapshot.");
    }
    return {};
}

QStringList copyCommands(const CopyRequest& r) {
    if (!copyProblem(r).isEmpty()) {
        return {};
    }
    QStringList c;
    const QString origin = url(r.originMachine, r.originDataset);
    const QString dest = url(r.destMachine, r.destDataset);
    switch (r.mode) {
        case CopyMode::Once: {
            const QString snap = QStringLiteral("copia-") + r.stamp;
            c << QStringLiteral("create @%1 --on %2").arg(snap, origin);
            c << QStringLiteral("send %1 --from %2@%3").arg(dest, origin, snap);
            break;
        }
        case CopyMode::Pull: {
            if (r.destRoleForOrigin != QLatin1String("full")) {
                // The destination gets the full credentials (the only ones the interface
                // gives, keys.h).
                c << QStringLiteral("peers --push --role full --on %1").arg(url(r.destMachine));
            }
            if (r.destRoleForOrigin.isEmpty()) {
                // And the origin listens on the network so that its daemon can be reached
                // without a tunnel.
                c << QStringLiteral("peers --listen 0.0.0.0 --on %1").arg(url(r.originMachine));
            }
            if (!r.originHasSchedule) {
                // And its first snapshots now: otherwise the first pull has nothing to bring
                // until the origin's daemon passes by itself, up to five minutes later.
                c << QStringLiteral("schedule --daily 1 --weekly 1 --monthly 1 --yearly 1 --on %1")
                         .arg(origin);
                c << QStringLiteral("schedule run --on %1").arg(origin);
            }
            const qsizetype slash = r.destDataset.indexOf(QLatin1Char('/'));
            const QString pool = r.destDataset.left(slash);
            const QString rest = r.destDataset.mid(slash + 1);
            c << QStringLiteral("create %1 -p --on %2").arg(rest, url(r.destMachine, pool));
            c << QStringLiteral("schedule --origin %1 %2 --on %3")
                     .arg(origin, keepOptions(r.keep), dest);
            c << QStringLiteral("schedule run --on %1").arg(dest);
            break;
        }
        case CopyMode::Push: {
            if (r.originRoleForDest != QLatin1String("full")) {
                c << QStringLiteral("peers --push --role full --on %1").arg(url(r.originMachine));
            }
            if (r.originRoleForDest.isEmpty()) {
                c << QStringLiteral("peers --listen 0.0.0.0 --on %1").arg(url(r.destMachine));
            }
            c << QStringLiteral("schedule %1 --to %2 --level --on %3")
                     .arg(keepOptions(r.keep), dest, origin);
            c << QStringLiteral("schedule run --on %1").arg(origin);
            break;
        }
    }
    return c;
}

}  // namespace zfsmgr::gui
