package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.FlowLayout;
import java.util.ArrayList;
import java.util.List;
import java.util.function.Consumer;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JSplitPane;
import javax.swing.JTable;
import javax.swing.ListSelectionModel;
import javax.swing.table.AbstractTableModel;
import javax.swing.table.DefaultTableCellRenderer;

/**
 * «Copias»: every dataset copied to another machine, sent by its origin or pulled by the machine
 * that keeps the copy, and what can be done with each.
 */
public final class CopiesPage extends JPanel {
    private final StateStore store;
    private final Consumer<String> say;
    private final Runnable newCopy;
    private List<StatusModel.ScheduleRow> rows = new ArrayList<>();
    private String selectedKey = "";
    private boolean filling;
    private final Model model = new Model();
    private final JTable table = new JTable(model);
    private final JPanel detailCard = Ui.card();
    /** The detail's lines, at the top of its card: a text area in a box layout takes all the height. */
    private final JPanel detail = new JPanel();

    /** The two ends of a copy: «zfsm://unib/ppal/user/srvs» split, whichever holds the schedule. */
    record Ends(String originMachine, String originDataset, String copyMachine, String copyDataset) {}

    static String[] splitUrl(String url) {
        String rest = url.startsWith("zfsm://") ? url.substring(7) : url;
        int slash = rest.indexOf('/');
        return slash < 0 ? new String[] {rest, ""} : new String[] {rest.substring(0, slash), rest.substring(slash + 1)};
    }

    static Ends endsOf(StatusModel.ScheduleRow r) {
        String[] peer = splitUrl(r.peer());
        return r.kind().equals("pull") ? new Ends(peer[0], peer[1], r.machine(), r.dataset())
                                       : new Ends(r.machine(), r.dataset(), peer[0], peer[1]);
    }

    private static String keyOf(StatusModel.ScheduleRow r) {
        return r.machineId() + "|" + r.dataset();
    }

    private final class Model extends AbstractTableModel {
        private final String[] heads = {L("What", "Qué"), L("From → to", "De → a"), L("How", "Cómo"),
                                        L("Keeps", "Guarda"), L("Last", "Última"), L("State", "Estado")};

        @Override
        public int getRowCount() {
            return rows.size();
        }

        @Override
        public int getColumnCount() {
            return heads.length;
        }

        @Override
        public String getColumnName(int c) {
            return heads[c];
        }

        @Override
        public Object getValueAt(int i, int c) {
            StatusModel.ScheduleRow r = rows.get(i);
            Ends e = endsOf(r);
            return switch (c) {
                case 0 -> e.originDataset();
                case 1 -> e.originMachine() + " → " + e.copyMachine();
                case 2 -> r.kind().equals("pull") ? L("%1 pulls it", "la trae %1", r.machine())
                                                  : L("%1 sends it", "la envía %1", r.machine());
                case 3 -> StatusModel.keepText(r.keep());
                case 4 -> r.state().equals("paused") ? "—"
                        : StatusModel.ago(r.lastLevel(), System.currentTimeMillis() / 1000);
                default -> r.state();
            };
        }
    }

    public CopiesPage(StateStore store, Consumer<String> say, Runnable newCopy) {
        super(new BorderLayout(0, 8));
        this.store = store;
        this.say = say;
        this.newCopy = newCopy;
        setBorder(BorderFactory.createEmptyBorder(10, 12, 10, 12));

        JPanel top = new JPanel(new BorderLayout());
        JLabel title = Ui.heading(L("Copies", "Copias"));
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.3f));
        top.add(title, BorderLayout.WEST);
        JButton fresh = new JButton(L("New copy", "Nueva copia"));
        fresh.addActionListener(e -> newCopy.run());
        top.add(fresh, BorderLayout.EAST);
        JPanel head = new JPanel(new BorderLayout(0, 6));
        head.add(top, BorderLayout.NORTH);
        head.add(Ui.wrapped(L("Each row is a dataset copied to another machine. Some are SENT by the origin; others are PULLED by the machine that keeps the copy, which is safer: from it the original cannot be deleted.",
                "Cada fila es un dataset que se copia a otra máquina. Unas las ENVÍA la máquina de origen; otras las TRAE la máquina de copias, que es más seguro: desde ella no se puede borrar el original.")),
                BorderLayout.CENTER);
        add(head, BorderLayout.NORTH);

        table.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        table.setFillsViewportHeight(true);
        table.getColumnModel().getColumn(5).setCellRenderer(new DefaultTableCellRenderer() {
            @Override
            public Component getTableCellRendererComponent(JTable t, Object v, boolean sel, boolean focus, int r, int c) {
                String state = String.valueOf(v);
                Component comp = super.getTableCellRendererComponent(t, Ui.stateWord(state), sel, focus, r, c);
                if (!sel) {
                    comp.setForeground(Ui.colourOf(state));
                }
                return comp;
            }
        });
        table.getSelectionModel().addListSelectionListener(e -> {
            if (e.getValueIsAdjusting() || filling) {
                return;
            }
            int i = table.getSelectedRow();
            if (i >= 0 && i < rows.size()) {
                selectedKey = keyOf(rows.get(i));
            }
            showDetail();
        });
        JScrollPane tableScroll = new JScrollPane(table);
        detailCard.setLayout(new BorderLayout());
        detailCard.add(detail, BorderLayout.NORTH);
        JScrollPane detailScroll = new JScrollPane(detailCard);
        detailScroll.setBorder(null);
        JSplitPane split = new JSplitPane(JSplitPane.VERTICAL_SPLIT, tableScroll, detailScroll);
        // The table as tall as its rows need, up to half; the detail, with its buttons, the rest.
        split.setResizeWeight(0.3);
        table.setPreferredScrollableViewportSize(new java.awt.Dimension(600, table.getRowHeight() * 6));
        int[] widths = {220, 120, 120, 260, 90, 90};
        for (int c = 0; c < widths.length; c++) {
            table.getColumnModel().getColumn(c).setPreferredWidth(widths[c]);
        }
        split.setBorder(null);
        add(split, BorderLayout.CENTER);

        store.onChanged(this::render);
        render();
    }

    private void render() {
        List<StatusModel.ScheduleRow> fresh = new ArrayList<>();
        for (StatusModel.ScheduleRow r : StatusModel.scheduleRows(store.machines())) {
            if (!r.kind().equals("local")) {
                fresh.add(r);
            }
        }
        filling = true;
        rows = fresh;
        model.fireTableDataChanged();
        int selected = -1;
        for (int i = 0; i < rows.size(); i++) {
            if (keyOf(rows.get(i)).equals(selectedKey)) {
                selected = i;
            }
        }
        if (selected < 0 && !rows.isEmpty() && selectedKey.isEmpty()) {
            selected = 0;
            selectedKey = keyOf(rows.get(0));
        }
        if (selected >= 0) {
            table.setRowSelectionInterval(selected, selected);
        }
        filling = false;
        showDetail();
    }

    private static Component left(Component c) {
        if (c instanceof javax.swing.JComponent j) {
            j.setAlignmentX(Component.LEFT_ALIGNMENT);
        }
        return c;
    }

    private void showDetail() {
        detail.removeAll();
        detail.setLayout(new BoxLayout(detail, BoxLayout.Y_AXIS));
        int i = table.getSelectedRow();
        if (i < 0 || i >= rows.size()) {
            detail.add(left(Ui.muted(rows.isEmpty()
                    ? L("No copies yet. «New copy» sets one up.", "Todavía no hay copias. «Nueva copia» prepara una.")
                    : L("Choose a copy.", "Elige una copia."))));
            detail.revalidate();
            detail.repaint();
            return;
        }
        StatusModel.ScheduleRow r = rows.get(i);
        Ends e = endsOf(r);
        JPanel head = new JPanel();
        head.setLayout(new BoxLayout(head, BoxLayout.X_AXIS));
        head.add(Ui.heading(e.originDataset()));
        head.add(Box.createHorizontalStrut(10));
        head.add(Ui.stateLabel(r.state()));
        detail.add(left(head));
        detail.add(left(new JLabel(L("Origin: %1 on %2", "Origen: %1 en %2", e.originDataset(), e.originMachine()))));
        detail.add(left(new JLabel(L("Copy: %1 on %2, keeps %3", "Copia: %1 en %2, guarda %3", e.copyDataset(),
                e.copyMachine(), StatusModel.keepText(r.keep())))));
        detail.add(left(Ui.wrapped(r.kind().equals("pull")
                ? L("%1 asks %2 for what is new. From %1 the original cannot be deleted, even if someone gets into it.",
                    "%1 pide lo nuevo a %2. Desde %1 no se puede borrar el original, aunque alguien entre en ella.",
                    e.copyMachine(), e.originMachine())
                : L("%1 sends it after each snapshot. The copy keeps exactly what the origin keeps.",
                    "%1 la manda tras cada snapshot. La copia guarda exactamente lo mismo que el origen.",
                    e.originMachine()))));
        if (!r.lastSnapshot().isEmpty()) {
            detail.add(left(Ui.muted(L("Last snapshot: %1", "Último snapshot: %1", r.lastSnapshot()))));
        }
        if (!r.error().isEmpty()) {
            javax.swing.JTextArea err = Ui.wrapped(r.error());
            err.setForeground(Ui.colourOf("error"));
            detail.add(left(err));
        }

        // The actions run on the machine that HOLDS the schedule: the one that pulls, or the
        // origin that sends.
        String on = "zfsm://" + r.machineId() + "/" + r.dataset();
        boolean paused = r.state().equals("paused");
        JPanel buttons = new JPanel(new FlowLayout(FlowLayout.LEFT, 0, 6));
        JButton now = new JButton(L("Copy now", "Copiar ahora"));
        now.setEnabled(!paused && r.job().isEmpty());
        now.addActionListener(ev -> act("schedule run --on " + on,
                L("Copying: follow it in Jobs.", "Copiando: síguelo en Trabajos.")));
        JButton pause = new JButton(paused ? L("Resume", "Reanudar") : L("Pause", "Pausar"));
        pause.addActionListener(ev -> {
            if (!paused) {
                act("schedule --off --on " + on, L("Paused: nothing is copied until it is resumed.",
                        "En pausa: no se copia nada hasta reanudarla."));
                return;
            }
            // Setting any value switches it on again (see `help schedule`): its own daily one.
            act("schedule --daily " + Json.num(r.keep(), "daily", 0) + " --on " + on, L("Resumed.", "Reanudada."));
        });
        JButton change = new JButton(L("Change…", "Cambiar…"));
        change.addActionListener(ev -> {
            // For a pushed copy these are the origin's numbers too: it keeps what the origin keeps.
            String flags = Dialogs.askKeep(this, L("What the copy keeps", "Lo que guarda la copia"),
                    r.kind().equals("push") ? L("The origin sends it: these are also the snapshots the origin keeps.",
                            "La envía el origen: son también los snapshots que guarda el origen.") : "",
                    r.keep());
            if (flags != null) {
                act("schedule " + flags + " --on " + on, L("Changed.", "Cambiado."));
            }
        });
        JButton stopIt = new JButton(L("Stop copying…", "Dejar de copiar…"));
        // Pulled: the copier's schedule goes (the CLI asks first); what was copied stays.
        // Pushed: only the levelling goes; the origin keeps taking its snapshots.
        stopIt.addActionListener(ev -> act(r.kind().equals("pull") ? "schedule --clear --on " + on
                                                                   : "schedule --no-level --no-to --on " + on,
                L("It is no longer copied. What was copied stays where it is.",
                  "Ya no se copia. Lo copiado se queda donde está.")));
        for (JButton b : new JButton[] {now, pause, change, stopIt}) {
            buttons.add(b);
            buttons.add(Box.createHorizontalStrut(6));
        }
        detail.add(left(buttons));
        detail.revalidate();
        detail.repaint();
    }

    private void act(String line, String done) {
        store.general().request(line, res -> {
            say.accept(res.ok() ? done : res.err().trim());
            store.refresh();
        });
    }
}
