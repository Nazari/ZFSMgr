package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.GridBagConstraints;
import java.awt.GridBagLayout;
import java.awt.Insets;
import java.awt.event.ComponentAdapter;
import java.awt.event.ComponentEvent;
import java.time.OffsetDateTime;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.Consumer;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JScrollPane;

/**
 * «Trabajos»: what is copying now —with a button to stop it— and what finished, with when and
 * how. The running ones come from the shared state; the finished ones are asked to each machine
 * ({@code jobs --history}) when the page is shown and after every refresh while it is.
 */
public final class JobsPage extends JPanel {
    private static final int HISTORY_SHOWN = 30;

    private final StateStore store;
    private final Consumer<String> say;
    private final Map<String, List<Map<String, Object>>> history = new HashMap<>();
    private final Set<String> asked = new HashSet<>();
    private final JScrollPane scroll = new JScrollPane();

    public JobsPage(StateStore store, Consumer<String> say) {
        super(new BorderLayout(0, 8));
        this.store = store;
        this.say = say;
        setBorder(javax.swing.BorderFactory.createEmptyBorder(10, 12, 10, 12));
        JLabel title = Ui.heading(L("Jobs", "Trabajos"));
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.3f));
        add(title, BorderLayout.NORTH);
        scroll.setBorder(null);
        scroll.getVerticalScrollBar().setUnitIncrement(16);
        add(scroll, BorderLayout.CENTER);
        store.onChanged(() -> {
            if (isShowing()) {
                loadHistory();
                render();
            }
        });
        store.onRefreshing(asked::clear);
        addComponentListener(new ComponentAdapter() {
            @Override
            public void componentShown(ComponentEvent e) {
                loadHistory();
                render();
            }
        });
        render();
    }

    private void loadHistory() {
        for (StatusModel.MachineView m : store.machines()) {
            if (!m.connected || !asked.add(m.id)) {
                continue;
            }
            final String id = m.id;
            store.forMachine(id).request("jobs --history zfsm://" + id, r -> {
                history.put(id, Json.objects(Json.firstObject(r.out()), "jobs"));
                if (isShowing()) {
                    render();
                }
            });
        }
    }

    private void stop(String machineId, String jobId) {
        // The CLI asks first (the question arrives as a dialog) and says what happened.
        store.forMachine(machineId).request("job cancel " + jobId + " --on zfsm://" + machineId, r -> {
            say.accept(r.ok() ? L("Job stopped.", "Trabajo parado.") : r.err().trim());
            store.refresh();
        });
    }

    static String resultWord(String state) {
        return switch (state) {
            case "done" -> L("● Done", "● Hecho");
            case "failed" -> L("✕ Failed", "✕ Falló");
            case "cancelled" -> L("‖ Stopped", "‖ Parado");
            default -> state;
        };
    }

    static String resultColourState(String state) {
        return switch (state) {
            case "done" -> "ok";
            case "failed" -> "error";
            default -> "paused";
        };
    }

    private static Component left(Component c) {
        if (c instanceof javax.swing.JComponent j) {
            j.setAlignmentX(Component.LEFT_ALIGNMENT);
        }
        return c;
    }

    private void render() {
        int pos = scroll.getVerticalScrollBar().getValue();
        JPanel body = new JPanel();
        body.setLayout(new BoxLayout(body, BoxLayout.Y_AXIS));

        body.add(left(Ui.heading(L("In progress", "En curso"))));
        body.add(Box.createVerticalStrut(4));
        List<StatusModel.JobRow> jobs = StatusModel.jobRows(store.machines());
        if (jobs.isEmpty()) {
            body.add(left(Ui.wrapped(L("Nothing is running. Scheduled copies start on their own at the next pass, every five minutes.",
                    "No hay nada en curso. Las copias programadas arrancan solas en la siguiente pasada, cada cinco minutos."))));
        } else {
            JPanel box = Ui.card();
            box.setLayout(new BoxLayout(box, BoxLayout.Y_AXIS));
            for (StatusModel.JobRow j : jobs) {
                JPanel row = new JPanel(new BorderLayout(8, 0));
                JPanel text = new JPanel();
                text.setLayout(new BoxLayout(text, BoxLayout.Y_AXIS));
                text.add(left(new JLabel(j.description())));
                text.add(left(Ui.muted(j.state().equals("queued")
                        ? L("on %1 · waiting its turn", "en %1 · esperando turno", j.machine())
                        : L("on %1 · %2 · %3 MB/s", "en %1 · %2 · %3 MB/s", j.machine(),
                                StatusModel.bytesText(j.bytes()), String.format("%.1f", j.rate())))));
                row.add(text, BorderLayout.CENTER);
                JButton b = new JButton(L("Stop", "Parar"));
                b.addActionListener(e -> stop(j.machineId(), j.id()));
                JPanel bh = new JPanel(new BorderLayout());
                bh.add(b, BorderLayout.NORTH);
                row.add(bh, BorderLayout.EAST);
                box.add(left(row));
            }
            body.add(left(box));
        }
        body.add(Box.createVerticalStrut(12));

        // Finished, newest first, from every machine.
        record Done(String machine, Map<String, Object> job) {}
        List<Done> done = new ArrayList<>();
        for (StatusModel.MachineView m : store.machines()) {
            for (Map<String, Object> j : history.getOrDefault(m.id, List.of())) {
                String st = Json.str(j, "state");
                if (!st.equals("running") && !st.equals("queued")) {
                    done.add(new Done(m.name, j));
                }
            }
        }
        done.sort((a, b) -> Json.str(b.job(), "finished").compareTo(Json.str(a.job(), "finished")));
        body.add(left(Ui.heading(L("Finished", "Terminados"))));
        body.add(Box.createVerticalStrut(4));
        if (done.isEmpty()) {
            body.add(left(Ui.muted(L("Nothing yet.", "Nada todavía."))));
        } else {
            JPanel box = Ui.card();
            box.setLayout(new GridBagLayout());
            GridBagConstraints c = new GridBagConstraints();
            c.anchor = GridBagConstraints.WEST;
            c.insets = new Insets(2, 0, 2, 14);
            String[] heads = {L("What", "Qué"), L("Where", "Dónde"), L("When", "Cuándo"), L("Size", "Tamaño"),
                              L("Result", "Resultado")};
            c.gridy = 0;
            for (int i = 0; i < heads.length; i++) {
                c.gridx = i;
                c.weightx = i == 0 ? 1 : 0;
                c.fill = i == 0 ? GridBagConstraints.HORIZONTAL : GridBagConstraints.NONE;
                box.add(Ui.muted(heads[i]), c);
            }
            long now = System.currentTimeMillis() / 1000;
            int row = 1;
            for (Done d : done.subList(0, Math.min(HISTORY_SHOWN, done.size()))) {
                Map<String, Object> j = d.job();
                String what = Json.str(j, "description");
                if (what.isEmpty() || what.equals("-")) {
                    what = Json.str(j, "snap");
                }
                String when = "";
                try {
                    when = StatusModel.ago(OffsetDateTime.parse(Json.str(j, "finished")).toEpochSecond(), now);
                } catch (RuntimeException ignored) {
                    // no date or another shape: left empty
                }
                String st = Json.str(j, "state");
                JLabel res = new JLabel(resultWord(st));
                res.setForeground(Ui.colourOf(resultColourState(st)));
                res.setFont(res.getFont().deriveFont(java.awt.Font.BOLD));
                Component[] cells = {new JLabel(what), new JLabel(d.machine()), new JLabel(when),
                                     new JLabel(StatusModel.bytesText(Json.num(j, "bytes", 0))), res};
                c.gridy = row++;
                for (int i = 0; i < cells.length; i++) {
                    c.gridx = i;
                    c.gridwidth = 1;
                    c.weightx = i == 0 ? 1 : 0;
                    c.fill = i == 0 ? GridBagConstraints.HORIZONTAL : GridBagConstraints.NONE;
                    box.add(cells[i], c);
                }
                String err = Json.str(j, "error");
                if (!err.isEmpty()) {
                    javax.swing.JTextArea e = Ui.wrapped(L("What happened: %1", "Qué pasó: %1", err));
                    e.setForeground(Ui.colourOf("error"));
                    c.gridy = row++;
                    c.gridx = 0;
                    c.gridwidth = cells.length;
                    c.weightx = 1;
                    c.fill = GridBagConstraints.HORIZONTAL;
                    box.add(e, c);
                    c.gridwidth = 1;
                }
            }
            body.add(left(box));
        }
        JPanel holder = new StatusPage.WidthTracking(new BorderLayout());
        holder.add(body, BorderLayout.NORTH);
        scroll.setViewportView(holder);
        javax.swing.SwingUtilities.invokeLater(() -> scroll.getVerticalScrollBar().setValue(pos));
    }
}
