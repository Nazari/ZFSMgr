package org.zfsmgr.gui;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.DisabledOnOs;
import org.junit.jupiter.api.condition.OS;
import org.junit.jupiter.api.io.TempDir;

/**
 * CliService against a fake {@code zfsmgr-cli --serve}: a shell script that speaks the protocol.
 * Events are delivered on this test's thread through a queue.
 */
@DisabledOnOs(OS.WINDOWS)
class CliServiceTest {
    @TempDir
    Path dir;

    private Path fake() throws Exception {
        Path f = dir.resolve("zfsmgr-cli");
        Files.writeString(f, """
                #!/bin/sh
                read pw
                [ "$pw" = "wrong" ] && { echo '{"type":"error","text":"wrong master password"}'; exit 1; }
                printf '%s\\n' "$*" > "$(dirname "$0")/args"
                echo '{"type":"ready","version":"9.9","data_key":"dk"}'
                while read line; do
                  id=$(printf '%s' "$line" | sed 's/.*"id":\\([0-9]*\\).*/\\1/')
                  case "$line" in *ask*) echo "{\\"type\\":\\"confirm\\",\\"id\\":$id,\\"title\\":\\"q\\"}"; read answer; echo "$answer" >> "$(dirname "$0")/answers";; esac
                  case "$line" in *hard*) echo "{\\"type\\":\\"confirm\\",\\"id\\":$id,\\"title\\":\\"q\\",\\"irreversible\\":true}"; read answer;; esac
                  case "$line" in *bye*) echo '{"type":"close_asked","host":"unib","user":"ana"}';; esac
                  echo "{\\"type\\":\\"result\\",\\"id\\":$id,\\"rc\\":0,\\"stdout\\":\\"ok $id\\",\\"stderr\\":\\"\\"}"
                done
                """);
        Files.setPosixFilePermissions(f, PosixFilePermissions.fromString("rwx------"));
        return f;
    }

    /** Runs what the service delivers, here, until {@code done} holds or 5 s. */
    private static void pump(LinkedBlockingQueue<Runnable> q, java.util.function.BooleanSupplier done)
            throws InterruptedException {
        long end = System.currentTimeMillis() + 5000;
        while (!done.getAsBoolean() && System.currentTimeMillis() < end) {
            Runnable r = q.poll(50, TimeUnit.MILLISECONDS);
            if (r != null) {
                r.run();
            }
        }
    }

    @Test
    void requestsGoOneAtATimeAndConfirmedOnesAreNotAsked() throws Exception {
        Path cli = fake();
        LinkedBlockingQueue<Runnable> q = new LinkedBlockingQueue<>();
        List<String> events = new CopyOnWriteArrayList<>();
        CliService s = new CliService(cli.toString(), "en", q::add, new CliService.Listener() {
            @Override
            public void ready(java.util.Map<String, Object> ev) {
                events.add("ready");
            }

            @Override
            public void confirm(CliService service, CliService.Question question) {
                events.add("asked " + question.irreversible());
                service.answerConfirm(question.id(), true, null);
            }

            @Override
            public void closeAsked(String host, String user) {
                events.add("close " + host + " " + user);
            }
        });
        s.setPresence(true);
        s.start("master".toCharArray(), false);
        List<String> outs = new CopyOnWriteArrayList<>();
        s.request("pwd", r -> outs.add(r.out()));
        s.request("ask", "json", true, r -> outs.add(r.out()));
        s.request("hard", "json", true, r -> outs.add(r.out()));
        s.request("bye", r -> outs.add(r.out()));
        pump(q, () -> outs.size() == 4);
        assertEquals(List.of("ok 1", "ok 2", "ok 3", "ok 4"), outs, "in order, one at a time");
        assertEquals("dk", s.dataKey());
        assertEquals("9.9", s.version());
        assertTrue(events.contains("ready"));
        assertEquals(List.of("ready", "asked true", "close unib ana"), events,
                "a confirmed request is not asked; an irreversible question still is");
        assertTrue(Files.readString(dir.resolve("answers")).contains("\"answer\":true"));
        String args = Files.readString(dir.resolve("args"));
        assertTrue(args.contains("--serve --lang en --presence gui"), args);
        s.stop(2000);
    }

    /** A fake that knows the session lock and the locked start (docs/protocolo_serve.md). */
    private Path fakeWithSession() throws Exception {
        Path f = dir.resolve("zfsmgr-cli-s");
        Files.writeString(f, """
                #!/bin/sh
                case "$*" in *--session*)
                  echo '{"type":"error","code":"session_busy","text":"ya está abierto","holder":{"kind":"cli","pid":42}}'; exit 1;; esac
                case "$*" in *--take-session*) printf '%s\n' "$*" > "$(dirname "$0")/took";; esac
                read pw
                if [ -z "$pw" ]; then
                  echo '{"type":"locked","key_present":false}'
                  echo '{"type":"key_present"}'
                  while read line; do
                    case "$line" in *'"password":"good"'*) break;; esac
                    echo '{"type":"unlock_failed","reason":"password"}'
                  done
                fi
                echo '{"type":"ready","version":"9.9","data_key":"dk"}'
                while read line; do :; done
                """);
        Files.setPosixFilePermissions(f, PosixFilePermissions.fromString("rwx------"));
        return f;
    }

    @Test
    void aBusySessionSaysWhoseAndTakeSessionGoesAhead() throws Exception {
        Path cli = fakeWithSession();
        LinkedBlockingQueue<Runnable> q = new LinkedBlockingQueue<>();
        List<String> events = new CopyOnWriteArrayList<>();
        CliService.Listener l = new CliService.Listener() {
            @Override
            public void sessionBusy(String text, boolean couldNotClose) {
                events.add("busy " + text + " " + couldNotClose);
            }

            @Override
            public void ready(java.util.Map<String, Object> ev) {
                events.add("ready");
            }

            @Override
            public void failed(String reason, boolean wrongPassword) {
                events.add("failed " + reason);
            }
        };
        CliService s = new CliService(cli.toString(), "es", q::add, l);
        s.setSession(CliService.Session.TAKE);
        s.start("m".toCharArray(), false);
        pump(q, () -> !events.isEmpty());
        assertEquals("busy ya está abierto false", events.get(0), "said as a busy session, not as a failure");
        events.clear();
        CliService t = new CliService(cli.toString(), "es", q::add, l);
        t.setSession(CliService.Session.TAKE_BY_FORCE);
        t.start("m".toCharArray(), false);
        pump(q, () -> events.contains("ready"));
        assertTrue(events.contains("ready"));
        assertTrue(Files.readString(dir.resolve("took")).contains("--take-session"));
        t.stop(2000);
    }

    @Test
    void lockedWaitsForTheKeyOrThePassword() throws Exception {
        Path cli = fakeWithSession();
        LinkedBlockingQueue<Runnable> q = new LinkedBlockingQueue<>();
        List<String> events = new CopyOnWriteArrayList<>();
        CliService[] self = new CliService[1];
        self[0] = new CliService(cli.toString(), "es", q::add, new CliService.Listener() {
            @Override
            public void locked(boolean keyPresent) {
                events.add("locked " + keyPresent);
                self[0].unlockWithPassword("bad".toCharArray());
            }

            @Override
            public void keyPresence(boolean present) {
                events.add("key " + present);
            }

            @Override
            public void unlockFailed(String reason, String code) {
                events.add("failed " + reason);
                self[0].unlockWithPassword("good".toCharArray());
            }

            @Override
            public void ready(java.util.Map<String, Object> ev) {
                events.add("ready");
            }
        });
        self[0].start(new char[0], false);
        pump(q, () -> events.contains("ready"));
        assertEquals(List.of("locked false", "key true", "failed password", "ready"), events);
        assertEquals("dk", self[0].dataKey());
        self[0].stop(2000);
    }

    @Test
    void aWrongPasswordIsSaidOnce() throws Exception {
        Path cli = fake();
        LinkedBlockingQueue<Runnable> q = new LinkedBlockingQueue<>();
        List<String> failures = new CopyOnWriteArrayList<>();
        CliService s = new CliService(cli.toString(), "en", q::add, new CliService.Listener() {
            @Override
            public void failed(String reason, boolean wrongPassword) {
                failures.add(wrongPassword + " " + reason);
            }
        });
        s.start("wrong".toCharArray(), false);
        pump(q, () -> false);
        assertEquals(List.of("true wrong master password"), failures);
    }
}
