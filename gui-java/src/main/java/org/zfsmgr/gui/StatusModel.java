package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * What the status panel shows, computed from what the CLI answers —{@code ls zfsm://} for the
 * machines and {@code overview <machine>} for each— and nothing else. No widget here: the part
 * tested without a screen. The CLI decides what is a warning and writes it; this only counts
 * and orders. (The Qt one: src/gui/statusmodel.cpp.)
 */
public final class StatusModel {
    private StatusModel() {}

    public static final class MachineView {
        public String id = "";
        public String name = "";
        public String uid = "";
        public boolean connected = true;
        public boolean loaded;
        public String failure = "";
        /** machines[0] of {@code overview}. */
        public Map<String, Object> overview = Map.of();
        /** The top-level warnings of that {@code overview}. */
        public List<Map<String, Object>> warnings = List.of();

        public boolean reachable() {
            return loaded && failure.isEmpty() && Json.bool(overview, "reachable", false);
        }
    }

    public record Summary(int machines, int answering, int pools, int poolsWithProblems,
                          int copies, int copiesOk, int copiesPending, int copiesFailing,
                          int jobsRunning) {}

    public record Attention(boolean error, String connection, String text, String subject,
                            String sessionHost) {}

    public record ScheduleRow(String machineId, String machine, String dataset, String kind,
                              String peer, String state, long lastLevel, String lastSnapshot,
                              String error, String job, Map<String, Object> keep) {}

    public static List<MachineView> machinesFromLs(String lsJson) {
        List<MachineView> out = new ArrayList<>();
        for (Map<String, Object> c : Json.objects(Json.firstObject(lsJson), "connections")) {
            MachineView m = new MachineView();
            m.id = Json.str(c, "id");
            m.name = Json.str(c, "name", m.id);
            if (m.name.isEmpty()) {
                m.name = m.id;
            }
            m.uid = Json.str(c, "uid");
            m.connected = Json.bool(c, "connected", true);
            out.add(m);
        }
        return out;
    }

    public static void applyOverview(MachineView m, String overviewJson) {
        Map<String, Object> doc = Json.firstObject(overviewJson);
        List<Map<String, Object>> machines = Json.objects(doc, "machines");
        m.loaded = true;
        if (machines.isEmpty()) {
            m.failure = L("no answer", "sin respuesta");
            return;
        }
        m.failure = "";
        m.overview = machines.get(0);
        m.warnings = Json.objects(doc, "warnings");
    }

    public static Summary summarize(List<MachineView> ms) {
        int machines = 0, answering = 0, pools = 0, poolsBad = 0, copies = 0, ok = 0, pending = 0,
            failing = 0, jobs = 0;
        for (MachineView m : ms) {
            if (!m.connected) {
                continue;
            }
            machines++;
            if (!m.reachable()) {
                continue;
            }
            answering++;
            for (Map<String, Object> p : Json.objects(m.overview, "pools")) {
                pools++;
                if (!Json.str(p, "status").equals("ok")) {
                    poolsBad++;
                }
            }
            for (Map<String, Object> s : Json.objects(m.overview, "schedules")) {
                if (Json.str(s, "kind").equals("local")) {
                    continue;
                }
                copies++;
                String st = Json.str(s, "state");
                if (st.equals("error")) {
                    failing++;
                } else if (st.equals("pending") || st.equals("paused")) {
                    pending++;
                } else {
                    ok++;
                }
            }
            for (Map<String, Object> j : Json.objects(m.overview, "jobs")) {
                if (Json.str(j, "state").equals("running")) {
                    jobs++;
                }
            }
        }
        return new Summary(machines, answering, pools, poolsBad, copies, ok, pending, failing, jobs);
    }

    /** Errors first, then warnings; a machine that could not be asked counts as an error. */
    public static List<Attention> attention(List<MachineView> ms) {
        List<Attention> errors = new ArrayList<>();
        List<Attention> warnings = new ArrayList<>();
        // ZFSMgr open elsewhere comes back from every machine that session reaches: once.
        Set<String> sessions = new HashSet<>();
        for (MachineView m : ms) {
            if (!m.connected || !m.loaded) {
                continue;
            }
            if (!m.failure.isEmpty()) {
                errors.add(new Attention(true, m.id,
                        L("%1 could not be asked: %2", "no se pudo preguntar a %1: %2", m.name, m.failure),
                        "machine", ""));
                continue;
            }
            for (Map<String, Object> w : m.warnings) {
                String sid = Json.str(w, "session");
                if (!sid.isEmpty() && !sessions.add(sid)) {
                    continue;
                }
                Attention a = new Attention(Json.str(w, "level").equals("error"), m.id,
                        Json.str(w, "text"), Json.str(w, "subject"), Json.str(w, "host"));
                (a.error() ? errors : warnings).add(a);
            }
        }
        errors.addAll(warnings);
        return errors;
    }

    public static List<ScheduleRow> scheduleRows(List<MachineView> ms) {
        List<ScheduleRow> out = new ArrayList<>();
        for (MachineView m : ms) {
            if (!m.reachable()) {
                continue;
            }
            for (Map<String, Object> s : Json.objects(m.overview, "schedules")) {
                out.add(new ScheduleRow(m.id, m.name, Json.str(s, "dataset"), Json.str(s, "kind"),
                        Json.str(s, "peer"), Json.str(s, "state"), Json.num(s, "last_level_time", 0),
                        Json.str(s, "last_snapshot"), Json.str(s, "error"), Json.str(s, "job"),
                        Json.obj(s, "keep")));
            }
        }
        return out;
    }

    /** A job running or queued on a machine, from its overview. */
    public record JobRow(String machineId, String machine, String id, String description, String state,
                         long bytes, double rate) {}

    public static List<JobRow> jobRows(List<MachineView> ms) {
        List<JobRow> out = new ArrayList<>();
        for (MachineView m : ms) {
            if (!m.reachable()) {
                continue;
            }
            for (Map<String, Object> j : Json.objects(m.overview, "jobs")) {
                String d = Json.str(j, "description");
                out.add(new JobRow(m.id, m.name, Json.str(j, "id"), d.isEmpty() ? Json.str(j, "snap") : d,
                        Json.str(j, "state"), Json.num(j, "bytes", 0), Json.dbl(j, "rate", 0)));
            }
        }
        return out;
    }

    /** SI units with one decimal, in the interface's language: «1,5 GB» / «1.5 GB». */
    public static String bytesText(long bytes) {
        String[] units = {"B", "kB", "MB", "GB", "TB", "PB", "EB"};
        if (bytes < 1000) {
            return bytes + " B";
        }
        double v = bytes;
        int u = 0;
        while (v >= 1000 && u < units.length - 1) {
            v /= 1000;
            u++;
        }
        String n = String.format(java.util.Locale.ROOT, "%.1f", v);
        if (Text.lang().equals("es")) {
            n = n.replace('.', ',');
        }
        return n + " " + units[u];
    }

    /** «hace 2 h» / «2 h ago»; «nunca» / «never» for 0. */
    public static String ago(long t, long now) {
        if (t <= 0) {
            return L("never", "nunca");
        }
        long d = Math.max(0, now - t);
        String amount;
        if (d < 90) {
            amount = L("%1 s", "%1 s", d);
        } else if (d < 90 * 60) {
            amount = L("%1 min", "%1 min", d / 60);
        } else if (d < 36 * 3600) {
            amount = L("%1 h", "%1 h", d / 3600);
        } else {
            amount = L("%1 days", "%1 días", d / 86400);
        }
        return L("%1 ago", "hace %1", amount);
    }

    /** {"daily":7,"weekly":4,…} → «7 diarias, 4 semanales»; «—» if none. */
    public static String keepText(Map<String, Object> keep) {
        String[][] many = {{"hourly", "%1 hourly", "%1 por hora"}, {"daily", "%1 daily", "%1 diarias"},
                           {"weekly", "%1 weekly", "%1 semanales"}, {"monthly", "%1 monthly", "%1 mensuales"},
                           {"yearly", "%1 yearly", "%1 anuales"}};
        String[][] one = {{"1 hourly", "1 por hora"}, {"1 daily", "1 diaria"}, {"1 weekly", "1 semanal"},
                          {"1 monthly", "1 mensual"}, {"1 yearly", "1 anual"}};
        List<String> parts = new ArrayList<>();
        for (int i = 0; i < many.length; i++) {
            long n = Json.num(keep, many[i][0], 0);
            if (n == 1) {
                parts.add(L(one[i][0], one[i][1]));
            } else if (n > 1) {
                parts.add(L(many[i][1], many[i][2], n));
            }
        }
        return parts.isEmpty() ? "—" : String.join(", ", parts);
    }
}
