package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.util.ArrayList;
import java.util.List;

/**
 * The «new copy» assistant's plan: the exact command lines it shows and runs. Pure, tested
 * without a screen.
 *
 * <ul>
 *   <li>Once: a snapshot now and a send.
 *   <li>Pulled: the destination's daemon pulls (`schedule --origin`). From the destination the
 *       original cannot be deleted, even if someone gets into it.
 *   <li>Pushed: the origin's daemon sends after each snapshot (`schedule --to … --level`).
 * </ul>
 *
 * Credentials and listening addresses are added only when the daemon that has to reach the other
 * one lacks them: `peers --listen` restarts a daemon. The interface gives FULL credentials only
 * (Keys), in both directions.
 */
public final class CopyPlan {
    private CopyPlan() {}

    public enum Mode { ONCE, PULL, PUSH }

    public static final class Keep {
        public int daily = 7;
        public int weekly = 4;
        public int monthly = 6;
        public int yearly = 1;
    }

    public static final class Request {
        public String originMachine = "";
        public String originDataset = "";
        public String destMachine = "";
        /** pool/x/y: the copy's full name. */
        public String destDataset = "";
        public Mode mode = Mode.PULL;
        public Keep keep = new Keep();
        /** The origin already takes snapshots by itself. */
        public boolean originHasSchedule;
        /** What credentials one daemon has for the other, as `peers` says: "", pull, full, known. */
        public String destRoleForOrigin = "";
        public String originRoleForDest = "";
        /** «20261122-1905», for the one-off snapshot's name. */
        public String stamp = "";
    }

    static String url(String machine, String dataset) {
        return "zfsm://" + machine + (dataset.isEmpty() ? "" : "/" + dataset);
    }

    static String keepOptions(Keep k) {
        return "--daily " + k.daily + " --weekly " + k.weekly + " --monthly " + k.monthly + " --yearly " + k.yearly;
    }

    /** A name the CLI takes as one word: ZFS names cannot hold spaces or quotes anyway. */
    static boolean plainName(String s) {
        if (s.isEmpty()) {
            return false;
        }
        for (char c : s.toCharArray()) {
            if (Character.isWhitespace(c) || c == '\'' || c == '"' || c == '@' || c == '#') {
                return false;
            }
        }
        return !s.startsWith("/") && !s.endsWith("/");
    }

    /** Why a request cannot be planned, in the interface's language; empty when it can. */
    public static String problem(Request r) {
        if (r.originMachine.isEmpty() || r.originDataset.isEmpty()) {
            return L("Choose what to copy.", "Elige qué copiar.");
        }
        if (r.destMachine.isEmpty() || r.destDataset.isEmpty()) {
            return L("Choose where the copy goes.", "Elige adónde va la copia.");
        }
        if (!plainName(r.destDataset) || !r.destDataset.contains("/")) {
            return L("The copy's name must be pool/name, without spaces.",
                    "El nombre de la copia tiene que ser pool/nombre, sin espacios.");
        }
        if (r.originMachine.equals(r.destMachine) && r.originDataset.equals(r.destDataset)) {
            return L("The copy cannot be the original itself.", "La copia no puede ser el propio original.");
        }
        if (r.originMachine.equals(r.destMachine) && r.destDataset.startsWith(r.originDataset + "/")) {
            return L("The copy cannot go inside the original.", "La copia no puede ir dentro del original.");
        }
        if (r.mode != Mode.ONCE && r.keep.daily + r.keep.weekly + r.keep.monthly + r.keep.yearly <= 0) {
            return L("The copy has to keep at least one snapshot.", "La copia tiene que guardar al menos un snapshot.");
        }
        if (r.mode == Mode.ONCE && r.stamp.isEmpty()) {
            return L("Missing the snapshot name.", "Falta el nombre del snapshot.");
        }
        return "";
    }

    /** The command lines, in order. Empty when the request makes no sense. */
    public static List<String> commands(Request r) {
        List<String> c = new ArrayList<>();
        if (!problem(r).isEmpty()) {
            return c;
        }
        String origin = url(r.originMachine, r.originDataset);
        String dest = url(r.destMachine, r.destDataset);
        switch (r.mode) {
            case ONCE -> {
                String snap = "copia-" + r.stamp;
                c.add("create @" + snap + " --on " + origin);
                c.add("send " + dest + " --from " + origin + "@" + snap);
            }
            case PULL -> {
                if (!r.destRoleForOrigin.equals("full")) {
                    c.add("peers --push --role full --on " + url(r.destMachine, ""));
                }
                if (r.destRoleForOrigin.isEmpty()) {
                    c.add("peers --listen 0.0.0.0 --on " + url(r.originMachine, ""));
                }
                if (!r.originHasSchedule) {
                    // Its first snapshots now: otherwise the first pull has nothing to bring
                    // until the origin's daemon passes by itself, up to five minutes later.
                    c.add("schedule --daily 1 --weekly 1 --monthly 1 --yearly 1 --on " + origin);
                    c.add("schedule run --on " + origin);
                }
                int slash = r.destDataset.indexOf('/');
                String pool = r.destDataset.substring(0, slash);
                String rest = r.destDataset.substring(slash + 1);
                c.add("create " + rest + " -p --on " + url(r.destMachine, pool));
                c.add("schedule --origin " + origin + " " + keepOptions(r.keep) + " --on " + dest);
                c.add("schedule run --on " + dest);
            }
            case PUSH -> {
                if (!r.originRoleForDest.equals("full")) {
                    c.add("peers --push --role full --on " + url(r.originMachine, ""));
                }
                if (r.originRoleForDest.isEmpty()) {
                    c.add("peers --listen 0.0.0.0 --on " + url(r.destMachine, ""));
                }
                c.add("schedule " + keepOptions(r.keep) + " --to " + dest + " --level --on " + origin);
                c.add("schedule run --on " + origin);
            }
        }
        return c;
    }
}
