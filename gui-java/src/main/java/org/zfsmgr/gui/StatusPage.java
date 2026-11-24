package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.Dimension;
import java.awt.Font;
import java.awt.GridLayout;
import java.util.List;
import java.util.Map;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.Timer;

/** «Estado»: how everything is, at a glance. (src/gui/statuspage.cpp.) */
public final class StatusPage extends JPanel {
    private final StateStore store;
    private final JLabel ago = Ui.muted(" ");
    private final JScrollPane scroll = new JScrollPane();

    public StatusPage(StateStore store) {
        super(new BorderLayout(0, 8));
        this.store = store;
        setBorder(javax.swing.BorderFactory.createEmptyBorder(10, 12, 10, 12));
        JPanel top = new JPanel();
        top.setLayout(new BoxLayout(top, BoxLayout.X_AXIS));
        JLabel title = Ui.heading(L("Status", "Estado"));
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.3f));
        top.add(title);
        top.add(Box.createHorizontalGlue());
        top.add(ago);
        top.add(Box.createHorizontalStrut(10));
        JButton refresh = new JButton(L("Refresh", "Actualizar"));
        refresh.addActionListener(e -> store.refresh());
        top.add(refresh);
        add(top, BorderLayout.NORTH);
        scroll.setBorder(null);
        scroll.getVerticalScrollBar().setUnitIncrement(16);
        add(scroll, BorderLayout.CENTER);
        store.onChanged(this::render);
        store.onRefreshing(() -> ago.setText(L("Updating…", "Actualizando…")));
        new Timer(10_000, e -> updateAgo()).start();
        render();
    }

    /**
     * As wide as the window, never wider: long texts wrap instead of pushing the page sideways
     * (a long warning hid three of the four tiles off the right edge).
     */
    static final class WidthTracking extends JPanel implements javax.swing.Scrollable {
        WidthTracking(java.awt.LayoutManager l) {
            super(l);
        }

        @Override
        public Dimension getPreferredScrollableViewportSize() {
            return getPreferredSize();
        }

        @Override
        public int getScrollableUnitIncrement(java.awt.Rectangle r, int o, int d) {
            return 16;
        }

        @Override
        public int getScrollableBlockIncrement(java.awt.Rectangle r, int o, int d) {
            return r.height;
        }

        @Override
        public boolean getScrollableTracksViewportWidth() {
            return true;
        }

        @Override
        public boolean getScrollableTracksViewportHeight() {
            return false;
        }
    }

    private void updateAgo() {
        if (store.updatedAt() > 0) {
            ago.setText(L("Updated %1", "Actualizado %1",
                    StatusModel.ago(store.updatedAt(), System.currentTimeMillis() / 1000)));
        }
    }

    private static JPanel tile(String title, String big, String small, String state) {
        JPanel f = Ui.card();
        f.setLayout(new BoxLayout(f, BoxLayout.Y_AXIS));
        f.add(Ui.muted(title));
        JLabel n = new JLabel(big);
        n.setFont(n.getFont().deriveFont(Font.BOLD, n.getFont().getSize2D() * 1.8f));
        n.setForeground(Ui.colourOf(state));
        f.add(n);
        f.add(Ui.muted(small));
        return f;
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
        updateAgo();
        List<StatusModel.MachineView> machines = store.machines();
        if (store.listFailed()) {
            body.add(left(Ui.wrapped(L("Could not list the machines: %1",
                    "No se pudieron listar las máquinas: %1", store.listError()))));
        } else if (machines.isEmpty()) {
            body.add(left(Ui.muted(L("Asking…", "Preguntando…"))));
        } else {
            fill(body, machines);
        }
        JPanel holder = new WidthTracking(new BorderLayout());
        holder.add(body, BorderLayout.NORTH);
        scroll.setViewportView(holder);
        javax.swing.SwingUtilities.invokeLater(() -> scroll.getVerticalScrollBar().setValue(pos));
    }

    private void fill(JPanel body, List<StatusModel.MachineView> machines) {
        StatusModel.Summary s = StatusModel.summarize(machines);
        JPanel tiles = new JPanel(new GridLayout(1, 4, 10, 0));
        tiles.add(tile(L("Machines", "Máquinas"), s.answering() + " / " + s.machines(),
                L("answer", "responden"), s.answering() == s.machines() ? "ok" : "error"));
        tiles.add(tile(L("Pools", "Pools"),
                String.valueOf(s.poolsWithProblems() == 0 ? s.pools() : s.poolsWithProblems()),
                s.poolsWithProblems() == 0 ? L("all fine", "todos bien")
                                           : L("need a look", "necesitan atención"),
                s.poolsWithProblems() == 0 ? "ok" : "warning"));
        tiles.add(tile(L("Copies", "Copias"), s.copiesOk() + " / " + s.copies(),
                s.copiesFailing() > 0 ? L("up to date · %1 failing", "al día · %1 fallan", s.copiesFailing())
                : s.copiesPending() > 0 ? L("up to date · %1 pending", "al día · %1 pendientes", s.copiesPending())
                : L("up to date", "al día"),
                s.copiesFailing() > 0 ? "error" : s.copiesPending() > 0 ? "pending" : "ok"));
        tiles.add(tile(L("Jobs", "Trabajos"), String.valueOf(s.jobsRunning()),
                L("copying now", "copiando ahora"), "ok"));
        tiles.setMaximumSize(new Dimension(Integer.MAX_VALUE, tiles.getPreferredSize().height));
        body.add(left(tiles));
        body.add(Box.createVerticalStrut(10));

        List<StatusModel.Attention> att = StatusModel.attention(machines);
        if (!att.isEmpty()) {
            JPanel box = Ui.card();
            box.setLayout(new BoxLayout(box, BoxLayout.Y_AXIS));
            box.add(left(Ui.heading(att.size() == 1
                    ? L("1 thing needs your attention", "1 cosa necesita tu atención")
                    : L("%1 things need your attention", "%1 cosas necesitan tu atención", att.size()))));
            for (StatusModel.Attention a : att) {
                javax.swing.JTextArea l = Ui.wrapped((a.error() ? "✕ " : "▲ ") + a.text());
                l.setForeground(Ui.colourOf(a.error() ? "error" : "warning"));
                box.add(left(l));
            }
            body.add(left(box));
            body.add(Box.createVerticalStrut(10));
        }

        body.add(left(Ui.heading(L("Machines and pools", "Máquinas y pools"))));
        body.add(Box.createVerticalStrut(4));
        JPanel grid = new JPanel(new GridLayout(0, 3, 10, 10));
        for (StatusModel.MachineView m : machines) {
            JPanel c = Ui.card();
            c.setLayout(new BoxLayout(c, BoxLayout.Y_AXIS));
            c.add(left(Ui.heading(m.name)));
            if (!m.connected) {
                c.add(left(Ui.muted(L("Disconnected", "Desconectada"))));
            } else if (!m.loaded) {
                c.add(left(Ui.stateLabel("")));
            } else if (!m.reachable()) {
                c.add(left(Ui.stateLabel("error")));
                c.add(left(Ui.wrapped(m.failure.isEmpty() ? Json.str(m.overview, "reason") : m.failure)));
            } else {
                List<Map<String, Object>> pools = Json.objects(m.overview, "pools");
                if (pools.isEmpty()) {
                    c.add(left(Ui.muted(L("No pools", "Sin pools"))));
                }
                for (Map<String, Object> p : pools) {
                    JPanel line = new JPanel();
                    line.setLayout(new BoxLayout(line, BoxLayout.X_AXIS));
                    long cap = Json.num(p, "capacity", -1);
                    line.add(new JLabel(Json.str(p, "name") + "  "));
                    line.add(Ui.muted(cap >= 0 ? L("%1 % of %2", "%1 % de %2", cap, Json.str(p, "size"))
                                               : Json.str(p, "size")));
                    line.add(Box.createHorizontalGlue());
                    line.add(Ui.stateLabel(Json.str(p, "status")));
                    c.add(left(line));
                }
            }
            grid.add(c);
        }
        body.add(left(grid));

        List<StatusModel.ScheduleRow> sch = StatusModel.scheduleRows(machines);
        if (!sch.isEmpty()) {
            body.add(Box.createVerticalStrut(10));
            body.add(left(Ui.heading(L("Scheduled copies", "Copias programadas"))));
            JPanel box = Ui.card();
            box.setLayout(new BoxLayout(box, BoxLayout.Y_AXIS));
            long now = System.currentTimeMillis() / 1000;
            for (StatusModel.ScheduleRow r : sch) {
                JPanel line = new JPanel();
                line.setLayout(new BoxLayout(line, BoxLayout.X_AXIS));
                String what = switch (r.kind()) {
                    case "pull" -> L("%1 pulls it from %2", "%1 la trae de %2", r.machine(), r.peer());
                    case "push" -> L("%1 sends it to %2", "%1 la envía a %2", r.machine(), r.peer());
                    default -> L("%1, snapshots only", "%1, solo snapshots", r.machine());
                };
                line.add(new JLabel(r.dataset() + "  "));
                line.add(Ui.muted(what + " · " + StatusModel.keepText(r.keep()) + " · "
                        + StatusModel.ago(r.lastLevel(), now)));
                line.add(Box.createHorizontalGlue());
                line.add(Ui.stateLabel(r.state()));
                box.add(left(line));
            }
            body.add(left(box));
        }
    }
}
