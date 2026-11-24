#include "keys.h"

#include "cliservice.h"
#include "statestore.h"
#include "text.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>

namespace zfsmgr::gui {

void giveAllKeys(StateStore* store, QObject* owner,
                 std::function<void(const QStringList& given, const QStringList& failed)> done) {
    CliService* general = store->general();
    general->request(
        QStringLiteral("ls zfsm://"), owner, [general, owner, done](const CliResult& r) {
            const qsizetype start = r.out.indexOf(QLatin1Char('{'));
            const QJsonArray conns = start < 0 ? QJsonArray()
                                               : QJsonDocument::fromJson(r.out.mid(start).toUtf8())
                                                     .object()
                                                     .value(QStringLiteral("connections"))
                                                     .toArray();
            struct Tally {
                int pending{0};
                QStringList given;
                QStringList failed;
            };
            auto tally = std::make_shared<Tally>();
            QStringList targets;
            for (const QJsonValue& v : conns) {
                const QJsonObject c = v.toObject();
                const QString id = c.value(QStringLiteral("id")).toString();
                if (id.isEmpty() || !c.value(QStringLiteral("connected")).toBool(true)) {
                    continue;
                }
                const QString daemon = c.value(QStringLiteral("daemon")).toString();
                if (daemon.isEmpty() || daemon == QLatin1String("-")) {
                    tally->failed << L("%1: it has no daemon", "%1: no tiene daemon").arg(id);
                    continue;
                }
                targets << id;
            }
            if (!r.ok() || targets.isEmpty()) {
                if (!r.ok()) {
                    tally->failed << r.err.trimmed();
                }
                done(tally->given, tally->failed);
                return;
            }
            tally->pending = static_cast<int>(targets.size());
            for (const QString& id : targets) {
                general->request(
                    QStringLiteral("peers --push --role full --on zfsm://") + id, owner,
                    [tally, id, done](const CliResult& p) {
                        if (p.ok()) {
                            tally->given << id;
                        } else {
                            tally->failed << id + QStringLiteral(": ") + p.err.trimmed();
                        }
                        if (--tally->pending == 0) {
                            done(tally->given, tally->failed);
                        }
                    },
                    QStringLiteral("json"), /*confirmed=*/true);
            }
        });
}

}  // namespace zfsmgr::gui
