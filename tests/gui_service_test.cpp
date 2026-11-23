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
            // «ask»: a question first, and the answer line read, as a confirm does.
            "  case \"$line\" in *ask*) echo \"{\\\"type\\\":\\\"confirm\\\",\\\"id\\\":$id,"
            "\\\"title\\\":\\\"q\\\"}\"; read answer;; esac\n"
            // «bye»: another machine asked this session to close, an event between results.
            "  case \"$line\" in *bye*) echo '{\"type\":\"close_asked\",\"host\":\"unib\","
            "\"user\":\"ana\"}';; esac\n"
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

    // Asked from another machine to close: the event says who, and the queue goes on.
    {
        QString host;
        QString user;
        QObject::connect(&s, &CliService::closeAsked, [&](const QString& h, const QString& u) {
            host = h;
            user = u;
        });
        int answered = 0;
        s.request(QStringLiteral("bye"), &alive, [&answered](const CliResult&) { ++answered; });
        waitFor([&] { return answered > 0; });
        check(host == QLatin1String("unib") && user == QLatin1String("ana"),
              "close_asked says who asked");
        check(answered == 1, "and the request still gets its answer");
    }

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

    // The busy cursor's count: held by a request made during a click, given back while its
    // question is open, held again once answered, and given back with the answer.
    {
        int count = 0;
        bool input = true;
        BusyHooks::userInput = [&input] { return input; };
        BusyHooks::adjust = [&count](int d) { count += d; };
        int begun = 0;
        int ended = 0;
        BusyHooks::begin = [&begun] { return ++begun; };
        BusyHooks::end = [&ended](int) { ++ended; };
        int asked = 0;
        int qid = 0;
        auto c = QObject::connect(&s, &CliService::confirmRequested, [&](const CliQuestion& q) {
            ++asked;
            qid = q.id;
        });
        int done = 0;
        s.request(QStringLiteral("ask"), &alive, [&done](const CliResult&) { ++done; });
        input = false;
        check(count == 1, "a request made during a click holds the busy cursor");
        waitFor([&] { return asked > 0; });
        check(asked == 1 && count == 0, "a question gives the cursor back");
        check(begun == 1 && ended == 0, "but the pressed control stays disabled meanwhile");
        s.answerConfirm(qid, true);
        check(count == 1, "answering it takes the cursor again");
        waitFor([&] { return done > 0; });
        check(done == 1 && count == 0, "the answer gives it back");
        check(ended == 1, "and enables the control again");
        s.request(QStringLiteral("pwd"), &alive, [&done](const CliResult&) { ++done; });
        check(count == 0 && begun == 1, "a request made outside a click does not touch them");
        waitFor([&] { return done > 1; });
        QObject::disconnect(c);
        BusyHooks::userInput = nullptr;
        BusyHooks::adjust = nullptr;
        BusyHooks::begin = nullptr;
        BusyHooks::end = nullptr;
    }

    if (g_fails == 0) {
        std::printf("gui service: all good\n");
    }
    return g_fails == 0 ? 0 : 1;
}
