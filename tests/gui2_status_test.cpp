// The status panel's model (src/gui/statusmodel.cpp): from what the CLI answers to what the
// panel counts and orders. Qt Core only, no screen. The language is forced to English so the
// texts compared do not depend on the machine running the test.

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

    // A failed request (no JSON) leaves the machine marked as failing, not as fine.
    MachineView broken;
    broken.connected = true;
    applyOverview(broken, QStringLiteral("not json"));
    check(broken.loaded && !broken.reachable() && !broken.failure.isEmpty(),
          "no JSON is a failure, not a fine machine");

    if (g_fails == 0) {
        std::printf("gui2 status model: all good\n");
    }
    return g_fails == 0 ? 0 : 1;
}
