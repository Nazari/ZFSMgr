package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.function.BiConsumer;

/**
 * Keys between machines, as the interface handles them: one kind only, the FULL keys, and every
 * machine holds those of every other. Adding a machine gives it the keys of all the others and
 * gives it to all of them; Administration can repeat that for everybody. (src/gui/keys.cpp.)
 */
public final class Keys {
    private Keys() {}

    /**
     * Every connected machine whose daemon answers gets the full keys of all the others
     * ({@code peers --push --role full --on <it>}), one after another on the general service and
     * already confirmed. {@code done} gets the machines that received them and, for the rest,
     * «machine: why».
     */
    public static void giveAll(CliService general, BiConsumer<List<String>, List<String>> done) {
        general.request("ls zfsm://", r -> {
            List<String> given = new ArrayList<>();
            List<String> failed = new ArrayList<>();
            List<String> targets = new ArrayList<>();
            for (Map<String, Object> c : Json.objects(Json.firstObject(r.out()), "connections")) {
                String id = Json.str(c, "id");
                if (id.isEmpty() || !Json.bool(c, "connected", true)) {
                    continue;
                }
                String daemon = Json.str(c, "daemon");
                if (daemon.isEmpty() || daemon.equals("-")) {
                    failed.add(L("%1: it has no daemon", "%1: no tiene daemon", id));
                    continue;
                }
                targets.add(id);
            }
            if (!r.ok() || targets.isEmpty()) {
                if (!r.ok()) {
                    failed.add(r.err().trim());
                }
                done.accept(given, failed);
                return;
            }
            final int[] pending = {targets.size()};
            for (String id : targets) {
                general.request("peers --push --role full --on zfsm://" + id, "json", true, p -> {
                    if (p.ok()) {
                        given.add(id);
                    } else {
                        failed.add(id + ": " + p.err().trim());
                    }
                    if (--pending[0] == 0) {
                        done.accept(given, failed);
                    }
                });
            }
        });
    }
}
