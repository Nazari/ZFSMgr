package org.zfsmgr.gui;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Deque;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/**
 * One {@code zfsmgr-cli --serve} (docs/protocolo_serve.md).
 *
 * <p>The service runs ONE command at a time, so requests wait here in a queue and the next one
 * goes when the previous one's {@code result} arrives. Everything the service says —results,
 * questions, events— is delivered through {@code deliver}, which for the interface is the Swing
 * thread: widgets are only touched there.
 *
 * <p>Secrets: the master password or the data key goes as the FIRST line of the service's input,
 * never as an argument nor in the environment.
 */
public final class CliService {
    public record Result(int rc, String out, String err) {
        public boolean ok() {
            return rc == 0;
        }
    }

    public record Question(int id, String title, String text, boolean irreversible,
                           List<String> lose, List<String> keep, List<String> notes,
                           String typeToConfirm) {}

    /** What the service says outside the results. Every method is optional. */
    public interface Listener {
        default void ready(Map<String, Object> event) {}

        /** It could not start or it died: wrong master password, missing executable… */
        default void failed(String reason, boolean wrongPassword) {}

        default void confirm(CliService service, Question q) {
            service.answerConfirm(q.id(), false, null);
        }

        default void secret(CliService service, int id, String text) {
            service.answerSecret(id, null);
        }

        default void closeAsked(String host, String user) {}

        /**
         * The machine's session is someone else's ({@code --session}): {@code text} says whose,
         * already written. {@code couldNotClose}: it was asked to close it
         * ({@code --take-session}) and it could not.
         */
        default void sessionBusy(String text, boolean couldNotClose) {}

        /** FIDO2 keys are registered and the first line was empty: it waits for a touch or the password. */
        default void locked(boolean keyPresent) {}

        default void touchWaiting() {}

        default void keyPresence(boolean present) {}

        /** {@code reason} «password» or «key»; {@code code} for a key: not_registered, pin_required… */
        default void unlockFailed(String reason, String code) {}
    }

    /** What the service does with the machine's session lock (docs/protocolo_serve.md). */
    public enum Session { NONE, TAKE, TAKE_BY_FORCE }

    private static final class Pending {
        final int id;
        final String line;
        final String format;
        final boolean confirmed;
        final Consumer<Result> done;
        long sentAt;

        Pending(int id, String line, String format, boolean confirmed, Consumer<Result> done) {
            this.id = id;
            this.line = line;
            this.format = format;
            this.confirmed = confirmed;
            this.done = done;
        }
    }

    /** Passed to every service, as given to the interface: {@code --config-dir <dir>}. */
    private static final List<String> COMMON_ARGS = new ArrayList<>();

    public static void setCommonArgs(List<String> args) {
        COMMON_ARGS.clear();
        COMMON_ARGS.addAll(args);
    }

    private final String cli;
    private final String lang;
    private final Executor deliver;
    private final Listener listener;
    private boolean presence;
    private Process proc;
    private OutputStream in;
    private final Deque<Pending> queue = new ArrayDeque<>();
    private Pending current;
    private boolean ready;
    private boolean stopping;
    private int nextId = 1;
    private String dataKey = "";
    private String version = "";
    private boolean noMaster;
    private boolean failedSaid;

    public CliService(String cli, String lang, Executor deliver, Listener listener) {
        this.cli = cli;
        this.lang = lang;
        this.deliver = deliver;
        this.listener = listener == null ? new Listener() {} : listener;
    }

    /**
     * This one announces the interface's session to the daemons, with ITS OWN PID
     * ({@code --presence gui}): a forced close from another machine ends this service, and the
     * interface, which lives on it, closes.
     */
    public void setPresence(boolean on) {
        presence = on;
    }

    private Session session = Session.NONE;

    /** This one holds the machine's session lock for the interface ({@code --session}). */
    public void setSession(Session s) {
        session = s;
    }

    public String dataKey() {
        return dataKey;
    }

    public String version() {
        return version;
    }

    public boolean noMaster() {
        return noMaster;
    }

    /**
     * Starts the process. {@code secret}: the master password, or with {@code isDataKey} the data
     * key another service opened. Wiped from the array once written.
     */
    public void start(char[] secret, boolean isDataKey) {
        List<String> args = new ArrayList<>(List.of(cli, "--serve", "--lang", lang));
        args.addAll(COMMON_ARGS);
        if (isDataKey) {
            args.add("--data-key");
        }
        if (presence) {
            args.add("--presence");
            args.add("gui");
        }
        if (session == Session.TAKE) {
            args.add("--session");
        } else if (session == Session.TAKE_BY_FORCE) {
            args.add("--take-session");
        }
        try {
            // What the services write on stderr, to a file everybody can find: without it a
            // service that hangs (a1278, outside an administrator session) says nothing.
            proc = new ProcessBuilder(args)
                    .redirectError(ProcessBuilder.Redirect.appendTo(Timing.cliLog()))
                    .start();
        } catch (IOException e) {
            deliver.execute(() -> listener.failed(Text.L("cannot start %1: %2",
                    "no se pudo arrancar %1: %2", cli, e.getMessage()), false));
            return;
        }
        in = proc.getOutputStream();
        byte[] first = new String(secret == null ? new char[0] : secret).getBytes(StandardCharsets.UTF_8);
        if (secret != null) {
            Arrays.fill(secret, '\0');
        }
        try {
            synchronized (this) {
                in.write(first);
                in.write('\n');
                in.flush();
            }
        } catch (IOException e) {
            // The reader sees the end and says it failed.
        } finally {
            Arrays.fill(first, (byte) 0);
        }
        // NOT a daemon thread: while a service starts there may be no window at all, and with
        // only daemon threads left the JVM ends (a1278: the CLI took long to start, and the
        // interface closed silently after the password). Closing the window ends the program.
        Thread reader = new Thread(this::readLoop, "zfsmgr-cli-reader");
        reader.start();
    }

    /** Queues one command line, exactly as typed in the terminal. {@code done} gets its result. */
    public void request(String line, Consumer<Result> done) {
        request(line, "json", false, done);
    }

    /**
     * {@code confirmed}: the person already said yes in the interface, so a plain question of
     * the CLI is answered here. Never an irreversible one, nor one that asks to type a word.
     */
    public void request(String line, String format, boolean confirmed, Consumer<Result> done) {
        queue.add(new Pending(nextId++, line, format, confirmed, done));
        sendNext();
    }

    /** The master password to a LOCKED service, which also waits for a touch of a key. */
    public void unlockWithPassword(char[] password) {
        String p = new String(password);
        Arrays.fill(password, '\0');
        write(Json.write("unlock", "password", "password", p));
    }

    public void answerConfirm(int id, boolean yes, String typed) {
        if (typed != null && !typed.isEmpty()) {
            write(Json.write("id", id, "answer", yes, "typed", typed));
        } else {
            write(Json.write("id", id, "answer", yes));
        }
    }

    public void answerSecret(int id, char[] secret) {
        if (secret == null) {
            write(Json.write("id", id, "secret", null));
            return;
        }
        String s = new String(secret);
        Arrays.fill(secret, '\0');
        write(Json.write("id", id, "secret", s));
    }

    /** Ends the service: its input is closed, so it says goodbye to the daemons and exits. */
    public void stop(long waitMs) {
        stopping = true;
        if (proc == null) {
            return;
        }
        try {
            synchronized (this) {
                in.close();
            }
        } catch (IOException ignored) {
            // already gone
        }
        try {
            if (!proc.waitFor(waitMs, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                proc.destroy();
            }
        } catch (InterruptedException e) {
            proc.destroy();
            Thread.currentThread().interrupt();
        }
    }

    /** The request that has not answered yet, and for how long (the spike's diagnosis). */
    public String waiting() {
        Pending p = current;
        return p == null ? "" : p.line + " (" + (System.currentTimeMillis() - p.sentAt) + " ms)";
    }

    private void sendNext() {
        if (!ready || current != null || queue.isEmpty()) {
            return;
        }
        current = queue.poll();
        current.sentAt = System.currentTimeMillis();
        write(Json.write("id", current.id, "line", current.line, "format", current.format));
    }

    private void write(String line) {
        try {
            synchronized (this) {
                in.write((line + "\n").getBytes(StandardCharsets.UTF_8));
                in.flush();
            }
        } catch (IOException | NullPointerException e) {
            // The reader thread notices the end of the process.
        }
    }

    private void readLoop() {
        try (BufferedReader r = new BufferedReader(
                new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                final String l = line;
                deliver.execute(() -> handle(l));
            }
        } catch (IOException ignored) {
            // treated as the end below
        }
        deliver.execute(this::ended);
    }

    private void ended() {
        if (stopping) {
            return;
        }
        String why = ready ? Text.L("the service ended", "el servicio terminó")
                           : Text.L("the service did not start", "el servicio no arrancó");
        if (!failedSaid) {
            failedSaid = true;
            listener.failed(why, false);
        }
        Pending p = current;
        current = null;
        if (p != null) {
            p.done.accept(new Result(1, "", why));
        }
        while (!queue.isEmpty()) {
            queue.poll().done.accept(new Result(1, "", why));
        }
    }

    private void handle(String line) {
        Map<String, Object> ev;
        try {
            Object v = Json.parse(line);
            if (!(v instanceof Map)) {
                return;
            }
            @SuppressWarnings("unchecked")
            Map<String, Object> m = (Map<String, Object>) v;
            ev = m;
        } catch (Json.ParseException e) {
            return;
        }
        String type = Json.str(ev, "type");
        switch (type) {
            case "ready" -> {
                ready = true;
                dataKey = Json.str(ev, "data_key");
                version = Json.str(ev, "version");
                noMaster = Json.bool(ev, "no_master", false);
                listener.ready(ev);
                sendNext();
            }
            case "error" -> {
                String text = Json.str(ev, "text");
                String code = Json.str(ev, "code");
                failedSaid = true;
                if (code.equals("session_busy") || code.equals("session_not_closed")) {
                    listener.sessionBusy(text, code.equals("session_not_closed"));
                } else {
                    listener.failed(text, text.contains("wrong master password")
                            || text.contains("wrong data key"));
                }
            }
            case "locked" -> listener.locked(Json.bool(ev, "key_present", false));
            case "touch" -> listener.touchWaiting();
            case "key_present" -> listener.keyPresence(true);
            case "key_absent" -> listener.keyPresence(false);
            case "unlock_failed" -> listener.unlockFailed(Json.str(ev, "reason"), Json.str(ev, "code"));
            case "result" -> {
                Pending p = current;
                current = null;
                if (p != null) {
                    int rc = (int) Json.num(ev, "rc", 1);
                    Timing.request(p.line, System.currentTimeMillis() - p.sentAt, rc);
                    p.done.accept(new Result(rc, Json.str(ev, "stdout"), Json.str(ev, "stderr")));
                }
                sendNext();
            }
            case "confirm" -> {
                Question q = new Question((int) Json.num(ev, "id", 0),
                        Json.str(ev, "title", Json.str(ev, "text")), Json.str(ev, "text"),
                        Json.bool(ev, "irreversible", false), Json.strings(ev, "lose"),
                        Json.strings(ev, "keep"), Json.strings(ev, "notes"),
                        Json.str(ev, "type_to_confirm"));
                if (current != null && current.id == q.id() && current.confirmed && !q.irreversible()
                        && q.typeToConfirm().isEmpty()) {
                    answerConfirm(q.id(), true, null);
                    return;
                }
                listener.confirm(this, q);
            }
            case "secret" -> listener.secret(this, (int) Json.num(ev, "id", 0), Json.str(ev, "text"));
            case "close_asked" -> listener.closeAsked(Json.str(ev, "host"), Json.str(ev, "user"));
            default -> {
                // an event this version does not know: skipped
            }
        }
    }
}
