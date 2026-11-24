package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Font;
import java.awt.Window;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JComboBox;
import javax.swing.JDialog;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JTable;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;
import javax.swing.table.DefaultTableModel;

/**
 * «Crear un pool»: a name, how its disks are arranged, the free disks of the machine, and the
 * exact command. zpool decides the rules; the CLI lists what is lost and asks for the name.
 * (src/gui/createpooldialog.cpp.)
 */
public final class CreatePoolDialog extends JDialog {
    private final String machine;
    private final JTextField name = new JTextField(20);
    private final JComboBox<NewCopyWizard.Item<String>> layout = new JComboBox<>();
    private final JComboBox<NewCopyWizard.Item<String>> optsAs = new JComboBox<>();
    private final DefaultTableModel disks = new DefaultTableModel(
            new String[] {"", L("Disk", "Disco"), L("Size", "Tamaño")}, 0) {
        @Override
        public Class<?> getColumnClass(int c) {
            return c == 0 ? Boolean.class : String.class;
        }

        @Override
        public boolean isCellEditable(int r, int c) {
            return c == 0 && getValueAt(r, 1) != null && !String.valueOf(getValueAt(r, 2)).isEmpty();
        }
    };
    private final JLabel command = new JLabel(" ");
    private final JTextArea result = Ui.wrapped("");

    public CreatePoolDialog(Window owner, StateStore store, String machine, Runnable created) {
        super(owner, L("Create a pool on %1", "Crear un pool en %1", machine), ModalityType.APPLICATION_MODAL);
        this.machine = machine;
        JPanel root = new JPanel(new BorderLayout(0, 8));
        root.setBorder(BorderFactory.createEmptyBorder(12, 14, 12, 14));
        String[][] layouts = {
            {"", L("Separate disks: all the space, no redundancy", "Discos sueltos: todo el espacio, sin redundancia")},
            {"mirror", L("Mirror: every disk holds everything; survives all but one failing",
                         "Espejo: cada disco lo guarda todo; aguanta que fallen todos menos uno")},
            {"raidz1", L("raidz1: survives one disk failing (3 disks or more)", "raidz1: aguanta que falle un disco (3 discos o más)")},
            {"raidz2", L("raidz2: survives two failing (4 or more)", "raidz2: aguanta que fallen dos (4 o más)")},
            {"raidz3", L("raidz3: survives three failing (5 or more)", "raidz3: aguanta que fallen tres (5 o más)")}};
        for (String[] l : layouts) {
            layout.addItem(new NewCopyWizard.Item<>(l[1], l[0]));
        }
        optsAs.addItem(new NewCopyWizard.Item<>(L("none", "ninguno"), ""));
        for (StatusModel.MachineView m : store.machines()) {
            for (Map<String, Object> p : Json.objects(m.overview, "pools")) {
                String pn = Json.str(p, "name");
                optsAs.addItem(new NewCopyWizard.Item<>(pn + " (" + m.name + ")", "zfsm://" + m.id + "/" + pn));
            }
        }
        Ui.Form form = new Ui.Form();
        form.row(L("Name", "Nombre"), name).row(L("How they are arranged", "Cómo se reparten"), layout)
                .row(L("Options like", "Opciones como las de"), optsAs);
        JPanel top = new JPanel();
        top.setLayout(new BoxLayout(top, BoxLayout.Y_AXIS));
        top.add(form);
        top.add(Box.createVerticalStrut(8));
        JLabel h = Ui.heading(L("Free disks", "Discos libres"));
        h.setAlignmentX(LEFT_ALIGNMENT);
        top.add(h);
        JLabel lost = Ui.muted(L("What is on the disks chosen is LOST.", "Lo que haya en los discos elegidos SE PIERDE."));
        lost.setAlignmentX(LEFT_ALIGNMENT);
        top.add(lost);
        root.add(top, BorderLayout.NORTH);
        JTable table = new JTable(disks);
        table.getColumnModel().getColumn(0).setMaxWidth(40);
        root.add(new JScrollPane(table), BorderLayout.CENTER);

        JPanel bottom = new JPanel();
        bottom.setLayout(new BoxLayout(bottom, BoxLayout.Y_AXIS));
        command.setFont(new Font(Font.MONOSPACED, Font.PLAIN, command.getFont().getSize()));
        command.setAlignmentX(LEFT_ALIGNMENT);
        bottom.add(command);
        result.setAlignmentX(LEFT_ALIGNMENT);
        bottom.add(result);
        JLabel rules = Ui.muted(L("The rules —how many disks each arrangement needs— are zpool's: the window only shows its answer.",
                "Las reglas —cuántos discos hacen falta— las decide zpool: la ventana solo enseña su respuesta."));
        rules.setAlignmentX(LEFT_ALIGNMENT);
        bottom.add(rules);
        JPanel buttons = new JPanel();
        buttons.setLayout(new BoxLayout(buttons, BoxLayout.X_AXIS));
        buttons.setAlignmentX(LEFT_ALIGNMENT);
        buttons.add(Box.createHorizontalGlue());
        JButton close = new JButton(L("Close", "Cerrar"));
        close.addActionListener(e -> dispose());
        buttons.add(close);
        buttons.add(Box.createHorizontalStrut(6));
        JButton go = new JButton(L("Create the pool…", "Crear el pool…"));
        go.addActionListener(e -> {
            String cmd = command();
            if (cmd.isEmpty()) {
                return;
            }
            go.setEnabled(false);
            result.setText(L("Creating…", "Creando…"));
            // The CLI lists what is lost and asks for the pool's name before writing.
            store.general().request(cmd, r -> {
                go.setEnabled(true);
                if (!r.ok()) {
                    result.setText(r.err().trim());
                    return;
                }
                created.run();
                dispose();
            });
        });
        buttons.add(go);
        bottom.add(Box.createVerticalStrut(6));
        bottom.add(buttons);
        root.add(bottom, BorderLayout.SOUTH);
        setContentPane(root);

        name.getDocument().addDocumentListener(new DocumentListener() {
            @Override
            public void insertUpdate(DocumentEvent e) {
                update();
            }

            @Override
            public void removeUpdate(DocumentEvent e) {
                update();
            }

            @Override
            public void changedUpdate(DocumentEvent e) {
                update();
            }
        });
        layout.addActionListener(e -> update());
        optsAs.addActionListener(e -> update());
        disks.addTableModelListener(e -> update());

        disks.addRow(new Object[] {false, L("Asking for the disks…", "Preguntando por los discos…"), ""});
        store.forMachine(machine).request("devices zfsm://" + machine, r -> {
            disks.setRowCount(0);
            List<Map<String, Object>> all = Json.objects(Json.firstObject(r.out()), "devices");
            // Free: claimed by no pool, not in use, no filesystem, no mountpoint, and —for a
            // whole disk— no partition of it in use either.
            List<String> busyParents = new ArrayList<>();
            for (Map<String, Object> d : all) {
                if (busy(d)) {
                    busyParents.add(Json.str(d, "parent"));
                }
            }
            for (Map<String, Object> d : all) {
                String path = Json.str(d, "path");
                if (busy(d) || busyParents.contains(path)) {
                    continue;
                }
                // The stable name (by-id) when there is one: /dev/sdb changes between boots.
                List<String> aliases = Json.strings(d, "aliases");
                String stable = aliases.isEmpty() ? path : aliases.get(0);
                disks.addRow(new Object[] {false, stable, StatusModel.bytesText(Json.lng(d, "size", 0))});
            }
            if (disks.getRowCount() == 0) {
                disks.addRow(new Object[] {false, L("No free disk on this machine.", "No hay ningún disco libre en esta máquina."), ""});
            }
            update();
        });
        update();
        setSize(640, 540);
        setLocationRelativeTo(owner);
    }

    private static boolean busy(Map<String, Object> d) {
        return Json.bool(d, "inuse", false) || !Json.str(d, "mountpoint").isEmpty()
                || !Json.str(d, "pool").isEmpty() || !Json.str(d, "fstype").isEmpty();
    }

    private String command() {
        String n = name.getText().trim();
        List<String> chosen = new ArrayList<>();
        for (int i = 0; i < disks.getRowCount(); i++) {
            if (Boolean.TRUE.equals(disks.getValueAt(i, 0)) && !String.valueOf(disks.getValueAt(i, 2)).isEmpty()) {
                chosen.add(String.valueOf(disks.getValueAt(i, 1)));
            }
        }
        if (!n.matches("[A-Za-z][A-Za-z0-9_.:-]*") || chosen.isEmpty()) {
            return "";
        }
        StringBuilder cmd = new StringBuilder("create " + n);
        @SuppressWarnings("unchecked")
        NewCopyWizard.Item<String> l = (NewCopyWizard.Item<String>) layout.getSelectedItem();
        if (l != null && !l.value().isEmpty()) {
            cmd.append(' ').append(l.value());
        }
        cmd.append(' ').append(String.join(" ", chosen));
        @SuppressWarnings("unchecked")
        NewCopyWizard.Item<String> like = (NewCopyWizard.Item<String>) optsAs.getSelectedItem();
        if (like != null && !like.value().isEmpty()) {
            cmd.append(" --opts-as ").append(like.value());
        }
        return cmd.append(" --on zfsm://").append(machine).toString();
    }

    private void update() {
        String cmd = command();
        command.setText(cmd.isEmpty() ? L("Give it a name and choose its disks.", "Dale un nombre y elige sus discos.") : cmd);
    }
}
