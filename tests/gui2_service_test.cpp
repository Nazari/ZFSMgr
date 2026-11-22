// CliService (src/gui/cliservice.cpp) against a fake `zfsmgr-cli --serve`: a shell script that
// speaks the protocol. What it pins: an answer whose owner was deleted while it travelled is
// dropped, not delivered into a dead widget (the crash of 2026-11-22: closing «Add a machine»
// before its check came back). Qt Core only, no screen.

#include "../src/gui/cliservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

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

// Runs the event loop until `done()` or two seconds.
template <typename F>
void waitFor(F done) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QString fake = dir.filePath(QStringLiteral("zfsmgr-cli"));
    {
        QFile f(fake);
        f.open(QIODevice::WriteOnly);
        // The master password, `ready`, and a `result` with the request's id for each line.
        f.write(
            "#!/bin/sh\n"
            "read pw\n"
            "echo '{\"type\":\"ready\"}'\n"
            "while read line; do\n"
            "  id=$(printf '%s' \"$line\" | sed 's/.*\"id\":\\([0-9]*\\).*/\\1/')\n"
            "  echo \"{\\\"type\\\":\\\"result\\\",\\\"id\\\":$id,\\\"rc\\\":0,"
            "\\\"stdout\\\":\\\"ok\\\",\\\"stderr\\\":\\\"\\\"}\"\n"
            "done\n");
        f.close();
        f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    CliService s(fake, QStringLiteral("en"));
    bool ready = false;
    QObject::connect(&s, &CliService::ready, [&ready](const QString&) { ready = true; });
    s.start(QByteArray());
    waitFor([&] { return ready; });
    check(ready, "the fake service says ready");

    // A live owner gets its answer.
    QObject alive;
    int got = 0;
    QString out;
    s.request(QStringLiteral("pwd"), &alive, [&](const CliResult& r) {
        ++got;
        out = r.out;
    });
    waitFor([&] { return got > 0; });
    check(got == 1 && out == QLatin1String("ok"), "an answer reaches a live owner");

    // An owner deleted while its request travels: its answer is dropped. A second request after
    // it, with a live owner, proves the first answer did arrive and was handled.
    auto* gone = new QObject();
    int called = 0;
    s.request(QStringLiteral("pwd"), gone, [&called](const CliResult&) { ++called; });
    delete gone;
    int after = 0;
    s.request(QStringLiteral("pwd"), &alive, [&after](const CliResult&) { ++after; });
    waitFor([&] { return after > 0; });
    check(after == 1, "the queue goes on after a dropped answer");
    check(called == 0, "an answer for a deleted owner is not delivered");

    // Destroying a service whose process is running —what happens to every service when the
    // window closes— tells nobody: its owner may be half destroyed (the exit crash).
    {
        auto* t = new CliService(fake, QStringLiteral("en"));
        bool up = false;
        int failures = 0;
        QObject::connect(t, &CliService::ready, [&up](const QString&) { up = true; });
        QObject::connect(t, &CliService::failed, [&failures](const QString&, bool) { ++failures; });
        t->start(QByteArray());
        waitFor([&] { return up; });
        int late = 0;
        t->request(QStringLiteral("pwd"), &alive, [&late](const CliResult&) { ++late; });
        delete t;
        QCoreApplication::processEvents();
        check(up && failures == 0, "destroying a running service does not emit failed");
        check(late == 0, "nor delivers the answers still pending");
    }

    if (g_fails == 0) {
        std::printf("gui2 service: all good\n");
    }
    return g_fails == 0 ? 0 : 1;
}
