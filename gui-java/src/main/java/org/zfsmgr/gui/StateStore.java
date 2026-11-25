package org.zfsmgr.gui;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Function;
import javax.swing.Timer;

/**
 * What the interface knows about the machines right now: {@code ls zfsm://} and the
 * {@code overview} of each. Each machine is asked through its OWN service, in parallel: one that is
 * off costs its own wait, never the others'. Listeners run when each answer arrives. Everything
 * here runs on the Swing thread.
 */
public final class StateStore {
    private final CliService general;
    private final Function<String, CliService> forMachine;
    private List<StatusModel.MachineView> machines = new ArrayList<>();
    private final List<Runnable> listeners = new ArrayList<>();
    private final List<Runnable> refreshing = new ArrayList<>();
    private final List<JobListener> jobEnded = new ArrayList<>();
    private final Set<String> followed = new HashSet<>();
    /** The machines whose {@code overview} is on its way, and whether the list itself is. */
    private final Set<String> reading = new HashSet<>();
    private boolean listing;
    /** The machines this interface just did something to, until an overview asked after it arrives. */
    private final Set<String> changing = new HashSet<>();
    /** Of those, the ones whose command has not answered yet: no overview clears them. */
    private final Map<String, Integer> held = new java.util.HashMap<>();
    private long updatedAt;
    private int generation;
    private boolean listFailed;
    private String listError = "";
    /** Called once when every connected machine has answered for the first time (timings). */
    private Runnable firstComplete;

    public StateStore(CliService general, Function<String, CliService> forMachine) {
        this.general = general;
        this.forMachine = forMachine;
    }

    public void onChanged(Runnable r) {
        listeners.add(r);
    }

    public void onRefreshing(Runnable r) {
        refreshing.add(r);
    }

    /**
     * Runs when a job this interface launched ends, however it ends: what it copied is on its
     * destination now, and whatever shows that destination has to ask again.
     */
    public void onJobEnded(JobListener l) {
        jobEnded.add(l);
    }

    /** How a followed job ended: its state («done», «failed», «cancelled»…) and its error. */
    public interface JobListener {
        void ended(String machine, String id, String state, String error);
    }

    /** Milliseconds between two looks at a followed job: the daemon updates it at that pace. */
    static final int JOB_POLL_MS = 2000;

    /**
     * If {@code out} is the answer of a command that started a job ({@code {"job_id", "connection"}},
     * as {@code send} gives it), follows that job until it ends; then the listeners of
     * {@link #onJobEnded} run and everything is refreshed. Without this, a copy that ended a
     * minute after the command returned stayed invisible until the interface was opened again.
     */
    public void followIfJob(String out) {
        Map<String, Object> o = Json.firstObject(out);
        String id = Json.str(o, "job_id");
        String machine = Json.str(o, "connection");
        if (id.isEmpty() || machine.isEmpty() || !followed.add(machine + "/" + id)) {
            return;
        }
        Timer t = new Timer(JOB_POLL_MS, null);
        t.setRepeats(false);
        int[] failures = {0};
        t.addActionListener(e -> forMachine(machine).requestQuiet("job " + id + " --on zfsm://" + machine, r -> {
            String state = jobField(r.out(), "state");
            boolean unknown = state.isEmpty();
            if (unknown && ++failures[0] < 5 || state.equals("running") || state.equals("queued")) {
                t.restart();
                return;
            }
            followed.remove(machine + "/" + id);
            String error = jobField(r.out(), "error");
            jobEnded.forEach(l -> l.ended(machine, id, state, error));
            refresh();
        }));
        t.start();
    }

    /** One field of a {@code job <id>} answer: {"job": [{"field": …, "value": …}, …]}. */
    static String jobField(String out, String field) {
        for (Map<String, Object> f : Json.objects(Json.firstObject(out), "job")) {
            if (field.equals(Json.str(f, "field"))) {
                return Json.str(f, "value");
            }
        }
        return "";
    }

    public void onFirstComplete(Runnable r) {
        firstComplete = r;
    }

    public List<StatusModel.MachineView> machines() {
        return machines;
    }

    public long updatedAt() {
        return updatedAt;
    }

    public boolean listFailed() {
        return listFailed;
    }

    public String listError() {
        return listError;
    }

    /** The machine's own service: what is asked about one machine goes through it. */
    public CliService forMachine(String id) {
        return forMachine.apply(id);
    }

    public CliService general() {
        return general;
    }

    /**
     * Whether what is shown of that machine may be about to change: its overview is being read
     * again, or the list of machines is. Pages veil what comes from it meanwhile ({@link Veil}).
     */
    public boolean reading(String id) {
        return listing || reading.contains(id) || changing.contains(id);
    }

    /** Whether any machine is being read: what sums them all (pools, jobs) is then veiled. */
    public boolean readingAny() {
        return listing || !reading.isEmpty() || !changing.isEmpty();
    }

    /**
     * Whether this interface did something to that machine and waits to see it: what only
     * changes from here —its copies, for one— is veiled only then.
     */
    public boolean changing(String id) {
        return changing.contains(id);
    }

    /**
     * Something was just done to these machines: what is shown of them is veiled until a fresh
     * overview of each arrives. Refreshes.
     */
    /**
     * A command for that machine starts: it is veiled now, and stays so until the command answers
     * ({@link #done}) and a fresh overview of it arrives after that.
     */
    public void begin(String id) {
        if (id == null || id.isEmpty()) {
            return;
        }
        held.merge(id, 1, Integer::sum);
        changing.add(id);
        changed();
    }

    /** The command {@link #begin} announced answered: its machine is read again. */
    public void done(String id) {
        if (id == null || id.isEmpty()) {
            return;
        }
        held.computeIfPresent(id, (k, n) -> n > 1 ? n - 1 : null);
        expect(id);
    }

    public void expect(String... ids) {
        for (String id : ids) {
            if (id != null && !id.isEmpty()) {
                changing.add(id);
            }
        }
        refresh();
    }

    public void refresh() {
        final int gen = ++generation;
        listing = true;
        refreshing.forEach(Runnable::run);
        changed();
        general.requestQuiet("ls zfsm://", r -> {
            if (gen != generation) {
                return;
            }
            listing = false;
            if (!r.ok() && !r.out().contains("{")) {
                listFailed = true;
                listError = r.err().trim();
                reading.clear();
                changing.retainAll(held.keySet());
                changed();
                return;
            }
            listFailed = false;
            List<StatusModel.MachineView> fresh = StatusModel.machinesFromLs(r.out());
            // The spike's diagnosis: which machines it read, and what came if none.
            StringBuilder seen = new StringBuilder();
            for (StatusModel.MachineView m : fresh) {
                seen.append(' ').append(m.id).append(m.connected ? "" : "(disconnected)");
            }
            if (Timing.on()) {
                System.err.println("[ls] " + fresh.size() + " machines:" + seen);
            }
            if (Timing.on() && fresh.isEmpty()) {
                String o = r.out();
                System.err.println("[ls] stdout (" + o.length() + " chars): "
                        + o.substring(0, Math.min(400, o.length())).replace("\n", " "));
                System.err.println("[ls] stderr: " + r.err().trim());
            }
            for (StatusModel.MachineView m : fresh) {
                for (StatusModel.MachineView old : machines) {
                    if (old.id.equals(m.id)) {
                        m.overview = old.overview;
                        m.warnings = old.warnings;
                        m.loaded = old.loaded;
                        m.failure = old.failure;
                    }
                }
            }
            machines = fresh;
            reading.clear();
            for (StatusModel.MachineView m : machines) {
                if (m.connected) {
                    reading.add(m.id);
                }
            }
            // A machine that is no longer there, or no longer connected, will not answer.
            changing.removeIf(x -> !reading.contains(x) && !held.containsKey(x));
            changed();
            for (StatusModel.MachineView m : machines) {
                if (!m.connected) {
                    continue;
                }
                final String id = m.id;
                forMachine.apply(id).requestQuiet("overview zfsm://" + id, o -> {
                    if (gen != generation) {
                        return;
                    }
                    reading.remove(id);
                    if (!held.containsKey(id)) {
                        changing.remove(id);
                    }
                    for (StatusModel.MachineView mv : machines) {
                        if (!mv.id.equals(id)) {
                            continue;
                        }
                        if (o.out().contains("{")) {
                            StatusModel.applyOverview(mv, o.out());
                        } else {
                            mv.loaded = true;
                            mv.failure = o.err().trim();
                        }
                    }
                    updatedAt = System.currentTimeMillis() / 1000;
                    changed();
                    if (firstComplete != null
                            && machines.stream().allMatch(x -> !x.connected || x.loaded)) {
                        Runnable f = firstComplete;
                        firstComplete = null;
                        f.run();
                    }
                });
            }
        });
    }

    private void changed() {
        listeners.forEach(Runnable::run);
    }
}
