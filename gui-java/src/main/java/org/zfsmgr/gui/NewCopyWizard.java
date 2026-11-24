package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.CardLayout;
import java.awt.Component;
import java.awt.Font;
import java.awt.Window;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.ButtonGroup;
import javax.swing.DefaultListModel;
import javax.swing.JButton;
import javax.swing.JDialog;
import javax.swing.JLabel;
import javax.swing.JList;
import javax.swing.JPanel;
import javax.swing.JRadioButton;
import javax.swing.JScrollPane;
import javax.swing.JSpinner;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.ListSelectionModel;
import javax.swing.SpinnerNumberModel;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;

/**
 * «Nueva copia»: what, where to, how, a review with the exact commands, and running them one by
 * one. The plan itself is {@link CopyPlan}, with its tests. (src/gui/newcopywizard.cpp.)
 */
public final class NewCopyWizard extends JDialog {
    private static final int WHAT = 0;
    private static final int WHERE = 1;
    private static final int HOW = 2;
    private static final int REVIEW = 3;
    private static final int RUNNING = 4;

    record Dataset(String machine, String machineName, String name, long used, boolean encrypted) {
        @Override
        public String toString() {
            return L("%1   on %2 · %3", "%1   en %2 · %3", name, machineName, StatusModel.bytesText(used));
        }
    }

    record Pool(String machine, String machineName, String name, String free) {}

    /** A line of a list: what it shows and what it stands for (null: not choosable). */
    record Item<T>(String text, T value) {
        @Override
        public String toString() {
            return text;
        }
    }

    private final StateStore store;
    private final Runnable created;
    private final List<Dataset> datasets = new ArrayList<>();
    private final List<Pool> pools = new ArrayList<>();
    /** machine id → (uid of another machine → the role its daemon has for it). */
    private final Map<String, Map<String, String>> reaches = new HashMap<>();
    private int pending;
    private String wantMachine = "";
    private String wantDataset = "";
    private String stamp = "";
    private int step;
    private List<String> toRun = List.of();
    private int running;

    private final CardLayout cards = new CardLayout();
    private final JPanel steps = new JPanel(cards);
    private final JLabel stepTitle = Ui.heading(" ");
    private final JTextField filter = new JTextField();
    private final DefaultListModel<Item<Dataset>> originModel = new DefaultListModel<>();
    private final JList<Item<Dataset>> origins = new JList<>(originModel);
    private final DefaultListModel<Item<Pool>> destModel = new DefaultListModel<>();
    private final JList<Item<Pool>> dests = new JList<>(destModel);
    private final JTextField name = new JTextField();
    private final JRadioButton once = new JRadioButton(L("Copy once, now", "Copiar una vez, ahora"));
    private final JRadioButton keepUp = new JRadioButton(L("Keep it up to date", "Mantenerla al día"), true);
    private final JRadioButton pull = new JRadioButton(
            L("The copies machine pulls it (recommended)", "La máquina de copias la trae (recomendado)"), true);
    private final JRadioButton push = new JRadioButton(L("The origin sends it", "El origen la envía"));
    private final JSpinner daily = spin(7);
    private final JSpinner weekly = spin(4);
    private final JSpinner monthly = spin(6);
    private final JSpinner yearly = spin(1);
    private final JTextArea summary = Ui.wrapped("");
    private final JTextArea commands = new JTextArea();
    private final DefaultListModel<String> progressModel = new DefaultListModel<>();
    private final JList<String> progress = new JList<>(progressModel);
    private final JButton back = new JButton(L("Back", "Atrás"));
    private final JButton cancel = new JButton(L("Cancel", "Cancelar"));
    private final JButton next = new JButton(L("Next", "Siguiente"));

    private static JSpinner spin(int v) {
        return new JSpinner(new SpinnerNumberModel(v, 0, 999, 1));
    }

    private static Component left(Component c) {
        if (c instanceof javax.swing.JComponent j) {
            j.setAlignmentX(Component.LEFT_ALIGNMENT);
        }
        return c;
    }

    public NewCopyWizard(Window owner, StateStore store, Runnable created) {
        super(owner, L("New copy", "Nueva copia"), ModalityType.APPLICATION_MODAL);
        this.store = store;
        this.created = created;
        setSize(760, 580);
        setLocationRelativeTo(owner);
        JPanel root = new JPanel(new BorderLayout(0, 8));
        root.setBorder(BorderFactory.createEmptyBorder(12, 14, 12, 14));
        stepTitle.setFont(stepTitle.getFont().deriveFont(stepTitle.getFont().getSize2D() * 1.2f));
        root.add(stepTitle, BorderLayout.NORTH);
        root.add(steps, BorderLayout.CENTER);
        setContentPane(root);

        // 1. What.
        {
            JPanel w = new JPanel(new BorderLayout(0, 6));
            JPanel top = new JPanel(new BorderLayout(0, 6));
            top.add(Ui.muted(L("Choose the dataset to copy. It is copied with everything inside it.",
                    "Elige el dataset de origen. Se copia con todo lo que tiene dentro.")), BorderLayout.NORTH);
            filter.putClientProperty("JTextField.placeholderText", L("Filter…", "Filtrar…"));
            top.add(filter, BorderLayout.SOUTH);
            w.add(top, BorderLayout.NORTH);
            origins.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
            w.add(new JScrollPane(origins), BorderLayout.CENTER);
            filter.getDocument().addDocumentListener(new DocumentListener() {
                @Override
                public void insertUpdate(DocumentEvent e) {
                    fillOrigins();
                }

                @Override
                public void removeUpdate(DocumentEvent e) {
                    fillOrigins();
                }

                @Override
                public void changedUpdate(DocumentEvent e) {
                    fillOrigins();
                }
            });
            origins.addListSelectionListener(e -> updateButtons());
            steps.add(w, String.valueOf(WHAT));
        }
        // 2. Where.
        {
            JPanel w = new JPanel(new BorderLayout(0, 6));
            w.add(Ui.muted(L("The machine that will keep the copy. Usually not the origin's.",
                    "La máquina que guardará la copia. Lo normal es una distinta del origen.")), BorderLayout.NORTH);
            dests.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
            w.add(new JScrollPane(dests), BorderLayout.CENTER);
            JPanel form = new JPanel(new BorderLayout(8, 0));
            form.add(new JLabel(L("Name of the copy", "Nombre de la copia")), BorderLayout.WEST);
            form.add(name, BorderLayout.CENTER);
            w.add(form, BorderLayout.SOUTH);
            dests.addListSelectionListener(e -> {
                if (e.getValueIsAdjusting()) {
                    return;
                }
                Pool p = chosenDest();
                Dataset o = chosenOrigin();
                if (p != null && o != null) {
                    // Same path under the destination's pool: ppal/user/srvs → back/user/srvs.
                    int slash = o.name().indexOf('/');
                    name.setText(p.name() + "/" + (slash < 0 ? o.name() : o.name().substring(slash + 1)));
                }
                updateButtons();
            });
            steps.add(w, String.valueOf(WHERE));
        }
        // 3. How.
        {
            JPanel w = new JPanel();
            w.setLayout(new BoxLayout(w, BoxLayout.Y_AXIS));
            ButtonGroup modes = new ButtonGroup();
            modes.add(once);
            modes.add(keepUp);
            w.add(left(once));
            w.add(left(Ui.muted("     " + L("A picture of how it is now. It does not update itself.",
                    "Una foto del estado actual. No se actualiza sola."))));
            w.add(left(keepUp));
            w.add(left(Ui.muted("     " + L("Snapshots are taken regularly and each one reaches the copy.",
                    "Se hacen snapshots periódicos y cada uno llega a la copia."))));
            w.add(Box.createVerticalStrut(12));
            JPanel keepBox = new JPanel();
            keepBox.setLayout(new BoxLayout(keepBox, BoxLayout.Y_AXIS));
            keepBox.add(left(Ui.heading(L("Who makes the copy", "Quién hace la copia"))));
            ButtonGroup who = new ButtonGroup();
            who.add(pull);
            who.add(push);
            keepBox.add(left(pull));
            keepBox.add(left(Ui.muted("     " + L("It asks the origin for what is new. From it the original cannot be deleted, even if someone gets into it.",
                    "Pide lo nuevo al origen. Desde ella no se puede borrar el original, aunque alguien entre en ella."))));
            keepBox.add(left(push));
            keepBox.add(left(Ui.muted("     " + L("The origin sends it after each snapshot. The copy keeps exactly what the origin keeps.",
                    "El origen la manda tras cada snapshot. La copia guarda exactamente lo mismo que el origen."))));
            keepBox.add(Box.createVerticalStrut(12));
            keepBox.add(left(Ui.heading(L("How many the copy keeps", "Cuántas guarda la copia"))));
            JPanel nums = new JPanel(new java.awt.FlowLayout(java.awt.FlowLayout.LEFT, 6, 4));
            Object[][] spins = {{L("Daily", "Diarias"), daily}, {L("Weekly", "Semanales"), weekly},
                                {L("Monthly", "Mensuales"), monthly}, {L("Yearly", "Anuales"), yearly}};
            for (Object[] s : spins) {
                nums.add(new JLabel((String) s[0]));
                nums.add((JSpinner) s[1]);
            }
            keepBox.add(left(nums));
            w.add(left(keepBox));
            keepUp.addItemListener(e -> setEnabledDeep(keepBox, keepUp.isSelected()));
            JPanel holder = new JPanel(new BorderLayout());
            holder.add(w, BorderLayout.NORTH);
            steps.add(holder, String.valueOf(HOW));
        }
        // 4. Review.
        {
            JPanel w = new JPanel(new BorderLayout(0, 8));
            JPanel top = new JPanel(new BorderLayout(0, 8));
            top.add(summary, BorderLayout.NORTH);
            top.add(Ui.heading(L("Commands that will run", "Órdenes que se ejecutarán")), BorderLayout.SOUTH);
            w.add(top, BorderLayout.NORTH);
            commands.setEditable(false);
            commands.setFont(new Font(Font.MONOSPACED, Font.PLAIN, commands.getFont().getSize()));
            w.add(new JScrollPane(commands), BorderLayout.CENTER);
            steps.add(w, String.valueOf(REVIEW));
        }
        // 5. Running.
        {
            JPanel w = new JPanel(new BorderLayout());
            w.add(new JScrollPane(progress), BorderLayout.CENTER);
            steps.add(w, String.valueOf(RUNNING));
        }

        JPanel buttons = new JPanel();
        buttons.setLayout(new BoxLayout(buttons, BoxLayout.X_AXIS));
        buttons.add(back);
        buttons.add(Box.createHorizontalGlue());
        buttons.add(cancel);
        buttons.add(Box.createHorizontalStrut(6));
        buttons.add(next);
        root.add(buttons, BorderLayout.SOUTH);
        getRootPane().setDefaultButton(next);
        cancel.addActionListener(e -> dispose());
        back.addActionListener(e -> goTo(step - 1));
        next.addActionListener(e -> {
            if (step == REVIEW) {
                run();
            } else if (step == RUNNING) {
                dispose();
            } else {
                goTo(step + 1);
            }
        });
        goTo(WHAT);
        load();
    }

    private static void setEnabledDeep(Component c, boolean on) {
        c.setEnabled(on);
        if (c instanceof java.awt.Container k) {
            for (Component x : k.getComponents()) {
                setEnabledDeep(x, on);
            }
        }
    }

    /** Opens with this dataset chosen, when it is found («Copy…» from Machines). */
    public void preselect(String machine, String dataset) {
        wantMachine = machine;
        wantDataset = dataset;
        fillOrigins();
    }

    private void load() {
        for (StatusModel.MachineView m : store.machines()) {
            if (!m.reachable()) {
                continue;
            }
            final String id = m.id;
            final String mname = m.name;
            CliService s = store.forMachine(id);
            pending++;
            s.request("peers zfsm://" + id, r -> {
                Map<String, String> roles = new HashMap<>();
                for (Map<String, Object> o : Json.objects(Json.firstObject(r.out()), "peers")) {
                    String role = Json.str(o, "role");
                    roles.put(Json.str(o, "uid"), role.isEmpty() || role.equals("-") ? "known" : role);
                }
                reaches.put(id, roles);
                pending--;
            });
            pending++;
            s.request("ls zfsm://" + id, r -> {
                pending--;
                for (Map<String, Object> p : Json.objects(Json.firstObject(r.out()), "pools")) {
                    String pool = Json.str(p, "name");
                    if (!Json.bool(p, "imported", true) || pool.isEmpty()) {
                        continue;
                    }
                    pools.add(new Pool(id, mname, pool, Json.str(p, "free")));
                    pending++;
                    s.request("ls -r zfsm://" + id + "/" + pool, rr -> {
                        pending--;
                        for (Map<String, Object> d : Json.objects(Json.firstObject(rr.out()), "entries")) {
                            if (!Json.str(d, "type").equals("dataset")) {
                                continue;
                            }
                            String enc = Json.str(d, "encryption");
                            datasets.add(new Dataset(id, mname, Json.str(d, "name"), Json.num(d, "used", 0),
                                    !enc.isEmpty() && !enc.equals("off") && !enc.equals("-")));
                        }
                        fillOrigins();
                    });
                }
                fillOrigins();
            });
        }
    }

    private void fillOrigins() {
        Dataset was = chosenOrigin();
        String keep = was != null ? was.machine() + was.name() : "";
        if (keep.isEmpty() && !wantDataset.isEmpty()) {
            keep = wantMachine + wantDataset;
        }
        String f = filter.getText().trim().toLowerCase();
        originModel.clear();
        int chosen = -1;
        for (Dataset d : datasets) {
            if (!f.isEmpty() && !d.name().toLowerCase().contains(f) && !d.machineName().toLowerCase().contains(f)) {
                continue;
            }
            originModel.addElement(new Item<>(d.toString(), d));
            if ((d.machine() + d.name()).equals(keep)) {
                chosen = originModel.size() - 1;
            }
        }
        if (pending > 0) {
            originModel.addElement(new Item<>(L("Asking the machines…", "Preguntando a las máquinas…"), null));
        }
        if (chosen >= 0) {
            origins.setSelectedIndex(chosen);
            origins.ensureIndexIsVisible(chosen);
        }
        updateButtons();
    }

    private void fillDestinations() {
        Dataset o = chosenOrigin();
        Pool was = chosenDest();
        String keep = was != null ? was.machine() + was.name() : "";
        destModel.clear();
        int chosen = -1;
        for (Pool p : pools) {
            String text = L("%1 · pool %2 · %3 free", "%1 · pool %2 · quedan %3", p.machineName(), p.name(), p.free());
            // Another machine first: a copy on the same machine does not survive losing it.
            if (o != null && p.machine().equals(o.machine())) {
                text += L("  (same machine as the origin)", "  (la misma máquina del origen)");
            }
            destModel.addElement(new Item<>(text, p));
            if ((p.machine() + p.name()).equals(keep)) {
                chosen = destModel.size() - 1;
            }
        }
        if (chosen >= 0) {
            dests.setSelectedIndex(chosen);
        }
    }

    private Dataset chosenOrigin() {
        Item<Dataset> it = origins.getSelectedValue();
        return it == null ? null : it.value();
    }

    private Pool chosenDest() {
        Item<Pool> it = dests.getSelectedValue();
        return it == null ? null : it.value();
    }

    CopyPlan.Request request() {
        CopyPlan.Request r = new CopyPlan.Request();
        Dataset o = chosenOrigin();
        Pool p = chosenDest();
        if (o == null || p == null) {
            return r;
        }
        r.originMachine = o.machine();
        r.originDataset = o.name();
        r.destMachine = p.machine();
        r.destDataset = name.getText().trim();
        r.mode = once.isSelected() ? CopyPlan.Mode.ONCE : pull.isSelected() ? CopyPlan.Mode.PULL : CopyPlan.Mode.PUSH;
        r.keep.daily = (Integer) daily.getValue();
        r.keep.weekly = (Integer) weekly.getValue();
        r.keep.monthly = (Integer) monthly.getValue();
        r.keep.yearly = (Integer) yearly.getValue();
        r.stamp = stamp;
        String originUid = "";
        String destUid = "";
        for (StatusModel.MachineView m : store.machines()) {
            if (m.id.equals(o.machine())) {
                originUid = m.uid;
            }
            if (m.id.equals(p.machine())) {
                destUid = m.uid;
            }
        }
        r.destRoleForOrigin = originUid.isEmpty() ? "" : reaches.getOrDefault(p.machine(), Map.of()).getOrDefault(originUid, "");
        r.originRoleForDest = destUid.isEmpty() ? "" : reaches.getOrDefault(o.machine(), Map.of()).getOrDefault(destUid, "");
        for (StatusModel.ScheduleRow s : StatusModel.scheduleRows(store.machines())) {
            if (s.machineId().equals(o.machine()) && s.dataset().equals(o.name())) {
                r.originHasSchedule = true;
            }
        }
        return r;
    }

    private void goTo(int to) {
        step = Math.max(WHAT, Math.min(RUNNING, to));
        if (step == WHERE) {
            fillDestinations();
        }
        if (step == REVIEW) {
            stamp = LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyyMMdd-HHmm"));
            review();
        }
        cards.show(steps, String.valueOf(step));
        String[] titles = {L("What do you want to copy?", "¿Qué quieres copiar?"), L("Where to?", "¿Adónde?"),
                           L("How?", "¿Cómo?"), L("Review and confirm", "Revisa y confirma"),
                           L("Setting up the copy", "Preparando la copia")};
        stepTitle.setText(titles[step]);
        back.setEnabled(step > WHAT && step < RUNNING);
        next.setText(step == REVIEW ? L("Create the copy", "Crear la copia") : L("Next", "Siguiente"));
        updateButtons();
    }

    private void updateButtons() {
        if (step == RUNNING) {
            return;
        }
        next.setEnabled(step == WHAT ? chosenOrigin() != null
                : step == WHERE ? chosenDest() != null
                : step == REVIEW ? CopyPlan.problem(request()).isEmpty()
                : true);
    }

    private void review() {
        CopyPlan.Request r = request();
        String problem = CopyPlan.problem(r);
        if (!problem.isEmpty()) {
            summary.setText(problem);
            commands.setText("");
            return;
        }
        Dataset o = chosenOrigin();
        Pool p = chosenDest();
        StringBuilder text = new StringBuilder(L("Copy %1 from %2 to %3 as %4.", "Copiar %1 de %2 a %3 como %4.",
                r.originDataset, o.machineName(), p.machineName(), r.destDataset)).append(' ');
        switch (r.mode) {
            case ONCE -> text.append(L("Once, now: it will not update itself.", "Una vez, ahora: no se actualizará sola."));
            case PULL -> text.append(L("%1 will pull what is new and keep %2.", "%1 traerá lo nuevo y guardará %2.",
                    p.machineName(), StatusModel.keepText(Map.of("daily", r.keep.daily, "weekly", r.keep.weekly,
                            "monthly", r.keep.monthly, "yearly", r.keep.yearly))));
            case PUSH -> text.append(L("%1 will send it after each snapshot.", "%1 la enviará tras cada snapshot.",
                    o.machineName()));
        }
        text.append("\n\n").append(L("It will take about %1; %2 has %3 free.", "Ocupará unos %1; en %2 quedan %3 libres.",
                StatusModel.bytesText(o.used()), p.machineName(), p.free()));
        if (o.encrypted()) {
            text.append('\n').append(L("The origin is encrypted: the copy will be too, and %1 will not need the key.",
                    "El origen está cifrado: la copia también lo estará y %1 no necesitará la clave.", p.machineName()));
        }
        summary.setText(text.toString());
        commands.setText(String.join("\n", CopyPlan.commands(r)));
    }

    private void run() {
        toRun = CopyPlan.commands(request());
        if (toRun.isEmpty()) {
            return;
        }
        progressModel.clear();
        for (String c : toRun) {
            progressModel.addElement("…  " + c);
        }
        goTo(RUNNING);
        next.setEnabled(false);
        cancel.setEnabled(false);
        running = -1;
        runNext();
    }

    private void finished() {
        next.setText(L("Close", "Cerrar"));
        next.setEnabled(true);
        cancel.setEnabled(false);
        created.run();
    }

    private void runNext() {
        running++;
        if (running >= toRun.size()) {
            progressModel.addElement(L("The copy is set up. Follow it in Copies and Jobs.",
                    "La copia está preparada. Síguela en Copias y Trabajos."));
            finished();
            return;
        }
        final int i = running;
        store.general().request(toRun.get(i), r -> {
            if (r.ok()) {
                progressModel.set(i, "✓  " + toRun.get(i));
                runNext();
                return;
            }
            progressModel.set(i, "✕  " + toRun.get(i));
            progressModel.addElement(r.err().trim().isEmpty() ? L("It failed.", "Falló.") : r.err().trim());
            progressModel.addElement(L("Nothing after it was run. What did run stays done.",
                    "No se ha ejecutado nada de lo que venía después. Lo ya hecho se queda hecho."));
            finished();
        });
    }
}
