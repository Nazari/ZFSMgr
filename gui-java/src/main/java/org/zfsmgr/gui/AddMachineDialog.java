package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Window;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JCheckBox;
import javax.swing.JDialog;
import javax.swing.JFileChooser;
import javax.swing.JPanel;
import javax.swing.JSpinner;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.SpinnerNumberModel;

/**
 * «Añadir una máquina»: how to reach it by SSH; the CLI asks the machine its system and the
 * password, if it needs one, as a secret. Then it is checked, its daemon offered, and when that
 * answers every machine gets the keys of all the others.
 */
public final class AddMachineDialog extends JDialog {
    private final StateStore store;
    private final Runnable added;
    private final JTextField id = new JTextField(20);
    private final JTextField host = new JTextField(20);
    private final JTextField user = new JTextField(20);
    private final JSpinner port = new JSpinner(new SpinnerNumberModel(22, 1, 65535, 1));
    private final JTextField key = new JTextField(20);
    private final JCheckBox sudo = new JCheckBox(L("Needs sudo to manage ZFS", "Necesita sudo para manejar ZFS"));
    private final JTextArea result = Ui.wrapped("");
    private final JButton install = new JButton(L("Install the daemon…", "Instalar el daemon…"));
    private final JButton go = new JButton(L("Add", "Añadir"));
    private String created = "";
    private boolean keysGiven;

    public AddMachineDialog(Window owner, StateStore store, Runnable added) {
        super(owner, L("Add a machine", "Añadir una máquina"), ModalityType.APPLICATION_MODAL);
        this.store = store;
        this.added = added;
        JPanel root = new JPanel(new BorderLayout(0, 10));
        root.setBorder(BorderFactory.createEmptyBorder(12, 14, 12, 14));
        root.add(Ui.paragraph(L("How to reach it by SSH. Its system is asked to the machine itself; the password, if it needs one, is asked next and stored encrypted.",
                "Cómo se llega a ella por SSH. El sistema se le pregunta a la propia máquina; la contraseña, si hace falta, se pide después y se guarda cifrada.")),
                BorderLayout.NORTH);
        JPanel middle = new JPanel();
        middle.setLayout(new BoxLayout(middle, BoxLayout.Y_AXIS));
        id.putClientProperty("JTextField.placeholderText", L("e.g. backup", "p. ej. copias"));
        key.putClientProperty("JTextField.placeholderText", L("empty: with a password", "vacío: con contraseña"));
        JButton browse = new JButton(L("Choose…", "Elegir…"));
        browse.addActionListener(e -> {
            JFileChooser fc = new JFileChooser(new java.io.File(System.getProperty("user.home"), ".ssh"));
            fc.setFileHidingEnabled(false);
            if (fc.showOpenDialog(this) == JFileChooser.APPROVE_OPTION) {
                key.setText(fc.getSelectedFile().getPath());
            }
        });
        JPanel keyRow = new JPanel(new BorderLayout(6, 0));
        keyRow.add(key, BorderLayout.CENTER);
        keyRow.add(browse, BorderLayout.EAST);
        Ui.Form form = new Ui.Form();
        form.row(L("Name", "Nombre"), id).row(L("Host", "Máquina (host)"), host).row(L("User", "Usuario"), user)
                .row(L("Port", "Puerto"), port).row(L("SSH key", "Clave SSH"), keyRow).row("", sudo);
        middle.add(form);
        middle.add(Box.createVerticalStrut(8));
        result.setAlignmentX(LEFT_ALIGNMENT);
        middle.add(result);
        root.add(middle, BorderLayout.CENTER);

        JPanel buttons = new JPanel();
        buttons.setLayout(new BoxLayout(buttons, BoxLayout.X_AXIS));
        install.setVisible(false);
        install.addActionListener(e -> {
            install.setEnabled(false);
            result.setText(L("Installing the daemon…", "Instalando el daemon…"));
            store.general().request("install-daemon --on zfsm://" + created, r -> {
                install.setEnabled(true);
                if (!r.ok()) {
                    result.setText(r.err().trim());
                    return;
                }
                probe();
            });
        });
        buttons.add(install);
        buttons.add(Box.createHorizontalGlue());
        JButton close = new JButton(L("Close", "Cerrar"));
        close.addActionListener(e -> dispose());
        buttons.add(close);
        buttons.add(Box.createHorizontalStrut(6));
        go.addActionListener(e -> create());
        buttons.add(go);
        root.add(buttons, BorderLayout.SOUTH);
        getRootPane().setDefaultButton(go);
        setContentPane(root);
        pack();
        setSize(Math.max(getWidth(), 560), getHeight() + 80);
        setLocationRelativeTo(owner);
    }

    private String problem() {
        if (!id.getText().trim().matches("[A-Za-z0-9][A-Za-z0-9._-]*")) {
            return L("The name: letters, digits, «.», «_» or «-», without spaces.",
                    "El nombre: letras, cifras, «.», «_» o «-», sin espacios.");
        }
        if (host.getText().trim().isEmpty() || host.getText().trim().contains(" ")) {
            return L("The host, without spaces.", "La máquina (host), sin espacios.");
        }
        if (user.getText().trim().isEmpty() || user.getText().trim().contains(" ")) {
            return L("The user, without spaces.", "El usuario, sin espacios.");
        }
        return "";
    }

    private void create() {
        String why = problem();
        if (!why.isEmpty()) {
            result.setText(why);
            return;
        }
        String name = id.getText().trim();
        StringBuilder line = new StringBuilder("create " + name + " --type SSH --host " + host.getText().trim()
                + " --user " + user.getText().trim());
        int p = (Integer) port.getValue();
        if (p != 22) {
            line.append(" --port ").append(p);
        }
        if (!key.getText().trim().isEmpty()) {
            line.append(" --key ").append(Ui.quoted(key.getText().trim()));
        }
        if (sudo.isSelected()) {
            line.append(" --sudo");
        }
        go.setEnabled(false);
        result.setText(L("Asking the machine which system it is…", "Preguntando a la máquina qué sistema es…"));
        store.general().request(line.toString(), r -> {
            if (!r.ok()) {
                go.setEnabled(true);
                result.setText(r.err().trim());
                return;
            }
            created = name;
            for (javax.swing.JComponent c : new javax.swing.JComponent[] {id, host, user, key, port, sudo}) {
                c.setEnabled(false);
            }
            added.run();
            probe();
        });
    }

    private void probe() {
        result.setText(L("Checking it…", "Comprobándola…"));
        store.general().request("probe zfsm://" + created, r -> {
            Map<String, Object> p = Json.firstObject(r.out());
            List<String> lines = new ArrayList<>();
            if (Json.bool(p, "reachable", false)) {
                lines.add(L("Added: %1, OpenZFS %2.", "Añadida: %1, OpenZFS %2.", Json.str(p, "os_detail"),
                        Json.str(p, "openzfs_version")));
            }
            for (Map<String, Object> w : Json.objects(p, "warnings")) {
                lines.add((Json.str(w, "level").equals("error") ? "✕ " : "▲ ") + Json.str(w, "text"));
            }
            if (Json.str(p, "status").equals("ok")) {
                lines.add(L("Everything works.", "Todo funciona."));
            }
            if (p.isEmpty() && !r.err().trim().isEmpty()) {
                lines.add(r.err().trim());
            }
            String before = String.join("\n", lines);
            result.setText(before);
            String daemon = Json.str(p, "daemon");
            install.setVisible(Json.bool(p, "reachable", false)
                    && (daemon.equals("unreachable") || daemon.equals("outdated")));
            // Its daemon answers: the keys, both ways, once.
            boolean answers = daemon.equals("ok") || daemon.equals("latest") || daemon.equals("ahead");
            if (answers && !keysGiven) {
                keysGiven = true;
                result.setText(before + "\n" + L("Giving it the keys of the others, and its key to them…",
                        "Dándole las llaves de las demás, y la suya a ellas…"));
                Keys.giveAll(store.general(), (given, failed) -> {
                    StringBuilder keys = new StringBuilder(L("Keys given to %1.", "Llaves entregadas a %1.",
                            given.isEmpty() ? "-" : String.join(", ", given)));
                    for (String f : failed) {
                        keys.append("\n✕ ").append(f);
                    }
                    result.setText(before + "\n" + keys);
                });
            }
            pack();
        });
    }
}
