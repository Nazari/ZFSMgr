// The status panel's model (src/gui/statusmodel.cpp): from what the CLI answers to what the
// panel counts and orders. Qt Core only, no screen. The language is forced to English so the
// texts compared do not depend on the machine running the test.

#include "../src/gui/copyplan.h"
#include "../src/gui/statusmodel.h"

#include <QCoreApplication>
#include <QLocale>

#include <cstdio>

using namespace zfsmgr::gui;

namespace {

int g_fails = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

const char* kLs = R"({"connections":[
  {"id":"local","name":"Local","connected":true},
  {"id":"unib","name":"unib","connected":true},
  {"id":"old","name":"old","connected":false}]})";

// A degraded pool, a failing pull, a pending push, a local schedule and a running job.
const char* kOverviewLocal = R"(aviso en stdout antes del documento
{"machines":[{"connection":"local","reachable":true,"daemon":"ok","status":"error",
  "pools":[{"name":"back","status":"ok","capacity":70},{"name":"sback","status":"warning"}],
  "schedules":[{"dataset":"back/a","kind":"pull","peer":"zfsm://unib/ppal/a","state":"error",
                "last_level_time":0,"error":"no route"},
               {"dataset":"back/b","kind":"push","peer":"zfsm://unib/ppal/b","state":"pending"},
               {"dataset":"back/c","kind":"local","state":"ok"}],
  "jobs":[{"id":"j1","state":"running","description":"copia","bytes":2100000000,"rate":94.1},
          {"id":"j2","state":"queued","description":"otra"}]}],
 "warnings":[{"level":"warning","connection":"local","subject":"pool sback","text":"W1"},
             {"level":"error","connection":"local","subject":"schedule back/a","text":"E1"}]})";

const char* kOverviewUnibDown = R"({"machines":[{"connection":"unib","reachable":false,
  "reason":"timeout","status":"error"}],
 "warnings":[{"level":"error","connection":"unib","subject":"machine","text":"E2"}]})";

}  // namespace

int main(int argc, char** argv) {
    QLocale::setDefault(QLocale(QLocale::English));
    QCoreApplication app(argc, argv);

    QVector<MachineView> ms = machinesFromLs(QString::fromUtf8(kLs));
    check(ms.size() == 3, "ls gives three machines");
    check(ms[0].name == QLatin1String("Local") && !ms[2].connected, "names and connected flag");

    Summary s0 = summarize(ms);
    check(s0.machines == 2 && s0.answering == 0, "before any overview nobody counts as answering");

    applyOverview(ms[0], QString::fromUtf8(kOverviewLocal));
    applyOverview(ms[1], QString::fromUtf8(kOverviewUnibDown));
    check(ms[0].reachable(), "a warning line before the JSON does not break the parse");
    check(!ms[1].reachable(), "a machine that does not answer is not reachable");

    const Summary s = summarize(ms);
    check(s.machines == 2, "disconnected machines are not counted");
    check(s.answering == 1, "one of two answers");
    check(s.pools == 2 && s.poolsWithProblems == 1, "pools and the one with problems");
    check(s.copies == 2, "a local schedule is not a copy");
    check(s.copiesFailing == 1 && s.copiesPending == 1 && s.copiesOk == 0, "copy states");
    check(s.jobsRunning == 1, "a queued job is not running");

    const QVector<Attention> a = attention(ms);
    check(a.size() == 3, "every warning of every machine");
    check(a.size() == 3 && a[0].error && a[1].error && !a[2].error, "errors first");
    check(a.size() == 3 && a[2].text == QLatin1String("W1"), "the CLI's text, unchanged");

    check(scheduleRows(ms).size() == 3, "schedules of reachable machines only");
    const QVector<JobRow> jobs = jobRows(ms);
    check(jobs.size() == 2 && jobs[0].bytes == 2100000000LL, "jobs with their bytes");

    check(ago(0, 1000) == QLatin1String("never"), "0 is never");
    check(ago(100000 - 30, 100000) == QLatin1String("30 s ago"), "seconds");
    check(ago(100000 - 7200, 100000) == QLatin1String("2 h ago"), "hours");

    // ZFSMgr open on another machine comes back from every machine it reaches: once.
    {
        QVector<MachineView> two = machinesFromLs(QString::fromUtf8(kLs));
        const char* sesA = R"({"machines":[{"connection":"local","reachable":true}],
          "warnings":[{"level":"warning","connection":"local","subject":"session",
                       "text":"S1","session":"abc"}]})";
        const char* sesB = R"({"machines":[{"connection":"unib","reachable":true}],
          "warnings":[{"level":"warning","connection":"unib","subject":"session",
                       "text":"S1","session":"abc"},
                      {"level":"warning","connection":"unib","subject":"session",
                       "text":"S2","session":"xyz"}]})";
        applyOverview(two[0], QString::fromUtf8(sesA));
        applyOverview(two[1], QString::fromUtf8(sesB));
        const QVector<Attention> b = attention(two);
        check(b.size() == 2 && b[0].text == QLatin1String("S1") && b[1].text == QLatin1String("S2"),
              "a session seen from two machines is one warning");
    }

    // A failed request (no JSON) leaves the machine marked as failing, not as fine.
    MachineView broken;
    broken.connected = true;
    applyOverview(broken, QStringLiteral("not json"));
    check(broken.loaded && !broken.reachable() && !broken.failure.isEmpty(),
          "no JSON is a failure, not a fine machine");

    // The copy plan: the exact commands the assistant shows and runs.
    CopyRequest r;
    r.originMachine = QStringLiteral("unib");
    r.originDataset = QStringLiteral("ppal/user/srvs");
    r.destMachine = QStringLiteral("local");
    r.destDataset = QStringLiteral("back/user/srvs");
    r.mode = CopyMode::Pull;
    r.originHasSchedule = true;
    r.destRoleForOrigin = QStringLiteral("pull");
    QStringList c = copyCommands(r);
    check(
        c
            == QStringList{QStringLiteral("create user/srvs -p --on zfsm://local/back"),
                           QStringLiteral("schedule --origin zfsm://unib/ppal/user/srvs --daily 7 "
                                          "--weekly 4 --monthly 6 --yearly 1 --on "
                                          "zfsm://local/back/user/srvs"),
                           QStringLiteral("schedule run --on zfsm://local/back/user/srvs")},
        "pull between machines that already know each other: no credentials step");
    r.destRoleForOrigin.clear();
    r.originHasSchedule = false;
    c = copyCommands(r);
    check(c.size() == 7 && c[0] == QLatin1String("peers --push --role pull --on zfsm://local")
              && c[1] == QLatin1String("peers --listen 0.0.0.0 --on zfsm://unib")
              && c[2].startsWith(QLatin1String("schedule --daily 1 --weekly 1"))
              && c[3] == QLatin1String("schedule run --on zfsm://unib/ppal/user/srvs"),
          "pull to a machine that does not know the origin: credentials, origin snapshots now");
    r.mode = CopyMode::Push;
    r.originRoleForDest = QStringLiteral("pull");
    c = copyCommands(r);
    check(c.size() == 3 && c[0] == QLatin1String("peers --push --role full --on zfsm://unib"),
          "push from a daemon with pull-only credentials: the full ones first, no --listen");
    r.originRoleForDest = QStringLiteral("full");
    c = copyCommands(r);
    check(c
              == QStringList{QStringLiteral("schedule --daily 7 --weekly 4 --monthly 6 --yearly 1 "
                                            "--to zfsm://local/back/user/srvs --level --on "
                                            "zfsm://unib/ppal/user/srvs"),
                             QStringLiteral("schedule run --on zfsm://unib/ppal/user/srvs")},
          "push: the origin's schedule with --to and --level");
    r.mode = CopyMode::Once;
    r.stamp = QStringLiteral("20261122-1905");
    c = copyCommands(r);
    check(c
              == QStringList{QStringLiteral("create @copia-20261122-1905 --on "
                                            "zfsm://unib/ppal/user/srvs"),
                             QStringLiteral("send zfsm://local/back/user/srvs --from "
                                            "zfsm://unib/ppal/user/srvs@copia-20261122-1905")},
          "once: a snapshot and a send");
    CopyRequest bad = r;
    bad.destDataset = QStringLiteral("back");
    check(copyCommands(bad).isEmpty() && !copyProblem(bad).isEmpty(), "a copy needs pool/name");
    bad = r;
    bad.destMachine = QStringLiteral("unib");
    bad.destDataset = QStringLiteral("ppal/user/srvs/dentro");
    check(!copyProblem(bad).isEmpty(), "not inside the original");
    bad = r;
    bad.destDataset = QStringLiteral("back/con espacio");
    check(!copyProblem(bad).isEmpty(), "no spaces in the name");
    bad = r;
    bad.mode = CopyMode::Pull;
    bad.keep = {0, 0, 0, 0};
    check(!copyProblem(bad).isEmpty(), "a kept copy keeps something");

    if (g_fails == 0) {
        std::printf("gui status model: all good\n");
    }
    return g_fails == 0 ? 0 : 1;
}
