package org.zfsmgr.gui;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Function;

/**
 * What the interface knows about the machines right now: {@code ls zfsm://} and the
 * {@code overview} of each. Each machine is asked through its OWN service, in parallel: one that is
 * off costs its own wait, never the others'. Listeners run when each answer arrives. Everything
 * here runs on the Swing thread. (The Qt one: src/gui/statestore.cpp.)
 */
public final class StateStore {
    private final CliService general;
    private final Function<String, CliService> forMachine;
    private List<StatusModel.MachineView> machines = new ArrayList<>();
    private final List<Runnable> listeners = new ArrayList<>();
    private final List<Runnable> refreshing = new ArrayList<>();
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

    public CliService general() {
        return general;
    }

    public void refresh() {
        final int gen = ++generation;
        refreshing.forEach(Runnable::run);
        general.request("ls zfsm://", r -> {
            if (gen != generation) {
                return;
            }
            if (!r.ok() && !r.out().contains("{")) {
                listFailed = true;
                listError = r.err().trim();
                changed();
                return;
            }
            listFailed = false;
            List<StatusModel.MachineView> fresh = StatusModel.machinesFromLs(r.out());
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
            changed();
            for (StatusModel.MachineView m : machines) {
                if (!m.connected) {
                    continue;
                }
                final String id = m.id;
                forMachine.apply(id).request("overview zfsm://" + id, o -> {
                    if (gen != generation) {
                        return;
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
