#include "statusmodel.h"

#include <QSet>

#include "text.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>

#include <algorithm>

namespace zfsmgr::gui {

namespace {

QJsonObject parseObject(const QString& json) {
    // The CLI prints ONE document; a warning that slipped onto stdout before it would break
    // a plain parse, so it starts at the first brace.
    const qsizetype start = json.indexOf(QLatin1Char('{'));
    if (start < 0) {
        return {};
    }
    return QJsonDocument::fromJson(json.mid(start).toUtf8()).object();
}

}  // namespace

QJsonArray MachineView::warnings() const {
    return overview.value(QStringLiteral("warnings_of_machine")).toArray();
}

bool MachineView::reachable() const {
    return loaded && failure.isEmpty() && overview.value(QStringLiteral("reachable")).toBool();
}

QVector<MachineView> machinesFromLs(const QString& lsJson) {
    QVector<MachineView> out;
    for (const QJsonValue& v : parseObject(lsJson).value(QStringLiteral("connections")).toArray()) {
        const QJsonObject c = v.toObject();
        MachineView m;
        m.id = c.value(QStringLiteral("id")).toString();
        m.name = c.value(QStringLiteral("name")).toString(m.id);
        if (m.name.isEmpty()) {
            m.name = m.id;
        }
        m.uid = c.value(QStringLiteral("uid")).toString();
        m.connected = c.value(QStringLiteral("connected")).toBool(true);
        out.push_back(m);
    }
    return out;
}

void applyOverview(MachineView& m, const QString& overviewJson) {
    const QJsonObject doc = parseObject(overviewJson);
    const QJsonArray machines = doc.value(QStringLiteral("machines")).toArray();
    m.loaded = true;
    if (machines.isEmpty()) {
        m.failure = L("no answer", "sin respuesta");
        return;
    }
    m.failure.clear();
    m.overview = machines.first().toObject();
    // The warnings are top-level in `overview`; kept with their machine.
    m.overview.insert(QStringLiteral("warnings_of_machine"),
                      doc.value(QStringLiteral("warnings")).toArray());
}

Summary summarize(const QVector<MachineView>& ms) {
    Summary s;
    for (const MachineView& m : ms) {
        if (!m.connected) {
            continue;
        }
        ++s.machines;
        if (!m.reachable()) {
            continue;
        }
        ++s.answering;
        for (const QJsonValue& p : m.overview.value(QStringLiteral("pools")).toArray()) {
            ++s.pools;
            if (p.toObject().value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
                ++s.poolsWithProblems;
            }
        }
        for (const QJsonValue& v : m.overview.value(QStringLiteral("schedules")).toArray()) {
            const QJsonObject sc = v.toObject();
            if (sc.value(QStringLiteral("kind")).toString() == QLatin1String("local")) {
                continue;
            }
            ++s.copies;
            const QString st = sc.value(QStringLiteral("state")).toString();
            if (st == QLatin1String("error")) {
                ++s.copiesFailing;
            } else if (st == QLatin1String("pending") || st == QLatin1String("paused")) {
                ++s.copiesPending;
            } else {
                ++s.copiesOk;
            }
        }
        for (const QJsonValue& j : m.overview.value(QStringLiteral("jobs")).toArray()) {
            if (j.toObject().value(QStringLiteral("state")).toString()
                == QLatin1String("running")) {
                ++s.jobsRunning;
            }
        }
    }
    return s;
}

QVector<Attention> attention(const QVector<MachineView>& ms) {
    QVector<Attention> out;
    // ZFSMgr open elsewhere comes back from every machine that session reaches: once.
    QSet<QString> sessions;
    for (const MachineView& m : ms) {
        if (!m.connected || !m.loaded) {
            continue;
        }
        if (!m.failure.isEmpty()) {
            out.push_back({true, m.id,
                           L("%1 could not be asked: %2", "no se pudo preguntar a %1: %2")
                               .arg(m.name, m.failure)});
            continue;
        }
        for (const QJsonValue& v : m.warnings()) {
            const QJsonObject w = v.toObject();
            if (const QString sid = w.value(QStringLiteral("session")).toString(); !sid.isEmpty()) {
                if (sessions.contains(sid)) {
                    continue;
                }
                sessions.insert(sid);
            }
            out.push_back({w.value(QStringLiteral("level")).toString() == QLatin1String("error"),
                           m.id, w.value(QStringLiteral("text")).toString()});
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Attention& a, const Attention& b) { return a.error && !b.error; });
    return out;
}

QVector<ScheduleRow> scheduleRows(const QVector<MachineView>& ms) {
    QVector<ScheduleRow> out;
    for (const MachineView& m : ms) {
        if (!m.reachable()) {
            continue;
        }
        for (const QJsonValue& v : m.overview.value(QStringLiteral("schedules")).toArray()) {
            const QJsonObject s = v.toObject();
            ScheduleRow r;
            r.machineId = m.id;
            r.machine = m.name;
            r.dataset = s.value(QStringLiteral("dataset")).toString();
            r.kind = s.value(QStringLiteral("kind")).toString();
            r.peer = s.value(QStringLiteral("peer")).toString();
            r.state = s.value(QStringLiteral("state")).toString();
            r.lastLevel = s.value(QStringLiteral("last_level_time")).toVariant().toLongLong();
            r.error = s.value(QStringLiteral("error")).toString();
            r.lastSnapshot = s.value(QStringLiteral("last_snapshot")).toString();
            r.job = s.value(QStringLiteral("job")).toString();
            r.keep = s.value(QStringLiteral("keep")).toObject();
            out.push_back(r);
        }
    }
    return out;
}

QVector<JobRow> jobRows(const QVector<MachineView>& ms) {
    QVector<JobRow> out;
    for (const MachineView& m : ms) {
        if (!m.reachable()) {
            continue;
        }
        for (const QJsonValue& v : m.overview.value(QStringLiteral("jobs")).toArray()) {
            const QJsonObject j = v.toObject();
            JobRow r;
            r.machineId = m.id;
            r.machine = m.name;
            r.id = j.value(QStringLiteral("id")).toString();
            r.description = j.value(QStringLiteral("description")).toString();
            if (r.description.isEmpty()) {
                r.description = j.value(QStringLiteral("snap")).toString();
            }
            r.state = j.value(QStringLiteral("state")).toString();
            r.bytes = j.value(QStringLiteral("bytes")).toVariant().toLongLong();
            r.rate = j.value(QStringLiteral("rate")).toDouble();
            out.push_back(r);
        }
    }
    return out;
}

QString ago(qint64 t, qint64 now) {
    if (t <= 0) {
        return L("never", "nunca");
    }
    const qint64 d = std::max<qint64>(0, now - t);
    QString amount;
    if (d < 90) {
        amount = L("%1 s", "%1 s").arg(d);
    } else if (d < 90 * 60) {
        amount = L("%1 min", "%1 min").arg(d / 60);
    } else if (d < 36 * 3600) {
        amount = L("%1 h", "%1 h").arg(d / 3600);
    } else {
        amount = L("%1 days", "%1 días").arg(d / 86400);
    }
    return L("%1 ago", "hace %1").arg(amount);
}

QString bytesText(qint64 bytes) {
    const QLocale loc =
        uiLang() == QLatin1String("es") ? QLocale(QLocale::Spanish) : QLocale(QLocale::English);
    return loc.formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

QString keepText(const QJsonObject& keep) {
    struct Clase {
        const char* key;
        const char* en;
        const char* es;
    };
    // Singular and plural: «1 anual», not «1 anuales».
    static const Clase clases[] = {{"hourly", "%1 hourly", "%1 por hora"},
                                   {"daily", "%1 daily", "%1 diarias"},
                                   {"weekly", "%1 weekly", "%1 semanales"},
                                   {"monthly", "%1 monthly", "%1 mensuales"},
                                   {"yearly", "%1 yearly", "%1 anuales"}};
    static const Clase una[] = {{"hourly", "1 hourly", "1 por hora"},
                                {"daily", "1 daily", "1 diaria"},
                                {"weekly", "1 weekly", "1 semanal"},
                                {"monthly", "1 monthly", "1 mensual"},
                                {"yearly", "1 yearly", "1 anual"}};
    QStringList parts;
    for (std::size_t i = 0; i < std::size(clases); ++i) {
        const int n = keep.value(QLatin1String(clases[i].key)).toInt();
        if (n == 1) {
            parts << L(una[i].en, una[i].es);
        } else if (n > 1) {
            parts << L(clases[i].en, clases[i].es).arg(n);
        }
    }
    return parts.isEmpty() ? QStringLiteral("—") : parts.join(QStringLiteral(", "));
}

}  // namespace zfsmgr::gui
