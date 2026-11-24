package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.Font;
import java.awt.GridBagConstraints;
import java.awt.GridBagLayout;
import java.awt.Insets;
import java.awt.event.ComponentAdapter;
import java.awt.event.ComponentEvent;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Consumer;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JCheckBox;
import javax.swing.JComboBox;
import javax.swing.JComponent;
import javax.swing.JDialog;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JSpinner;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.SpinnerNumberModel;
import javax.swing.Timer;

/**
 * «Administración» (advanced mode): the connections, the programs on each machine, whether each
 * daemon reaches every other by itself ({@code peers --check}), the security keys that open
 * ZFSMgr, and the language. (src/gui/adminpage.cpp.)
 */
public final class AdminPage extends JPanel {
    private final StateStore store;
    private final Consumer<String> say;
    private final Runnable addMachine;
    private final JScrollPane scroll = new JScrollPane();
    private List<Map<String, Object>> connections = List.of();
    private final Map<String, List<Map<String, Object>>> checks = new HashMap<>();
    private final Map<String, String> checkError = new HashMap<>();
    private List<Map<String, Object>> keys = List.of();
    private List<Map<String, Object>> daemons = List.of();
    private final Map<String, String> probeText = new LinkedHashMap<>();
    private boolean loading;

    public AdminPage(StateStore store, Consumer<String> say, Runnable addMachine) {
        super(new BorderLayout(0, 8));
        this.store = store;
        this.say = say;
        this.addMachine = addMachine;
        setBorder(BorderFactory.createEmptyBorder(10, 12, 10, 12));
        JLabel title = Ui.heading(L("Administration", "Administración"));
        title.setFont(title.getFont().deriveFont(title.getFont().getSize2D() * 1.3f));
        add(title, BorderLayout.NORTH);
        scroll.setBorder(null);
        scroll.getVerticalScrollBar().setUnitIncrement(16);
        add(scroll, BorderLayout.CENTER);
        addComponentListener(new ComponentAdapter() {
            @Override
            public void componentShown(ComponentEvent e) {
                load();
            }
        });
        render();
    }

    private static String url(String machine) {
        return "zfsm://" + machine;
    }

    /** Everything asked again: the connections, each machine's checks, the keys, the programs. */
    public void load() {
        if (loading) {
            return;
        }
        loading = true;
        store.general().request("ls zfsm://", r -> {
            connections = Json.objects(Json.firstObject(r.out()), "connections");
            render();
            for (Map<String, Object> c : connections) {
                if (!Json.bool(c, "connected", true)) {
                    continue;
                }
                String id = Json.str(c, "id");
                // From each machine, to every other it holds a key of: on its own service, so a
                // machine that takes long only delays its own row.
                store.forMachine(id).request("peers --check --on " + url(id), p -> {
                    if (p.ok()) {
                        checks.put(id, Json.objects(Json.firstObject(p.out()), "checks"));
                        checkError.remove(id);
                    } else {
                        checks.remove(id);
                        checkError.put(id, p.err().trim());
                    }
                    render();
                });
            }
            store.general().request("master-key", k -> {
                keys = Json.objects(Json.firstObject(k.out()), "keys");
                render();
            });
            store.general().request("install-daemon -a --check", d -> {
                daemons = Json.objects(Json.firstObject(d.out()), "items");
                loading = false;
                render();
            });
        });
    }

    private void act(String line, String done, int delayMs) {
        store.general().request(line, r -> {
            say.accept(r.ok() ? done : r.err().trim());
            store.refresh();
            // A daemon that restarts answers again a few seconds later: checked before, every
            // square towards it would read «does not get there».
            Timer t = new Timer(delayMs, e -> load());
            t.setRepeats(false);
            t.start();
        });
    }

    private static JButton small(String text, Runnable r) {
        JButton b = new JButton(text);
        b.addActionListener(e -> r.run());
        return b;
    }

    private static Component left(Component c) {
        if (c instanceof JComponent j) {
            j.setAlignmentX(Component.LEFT_ALIGNMENT);
        }
        return c;
    }

    private static JTextArea note(String text) {
        JTextArea t = Ui.wrapped(text);
        java.awt.Color c = javax.swing.UIManager.getColor("Label.disabledForeground");
        t.setForeground(c != null ? c : java.awt.Color.GRAY);
        t.setAlignmentX(LEFT_ALIGNMENT);
        return t;
    }

    /** A heading with, optionally, a button at its right. */
    private static JPanel head(String text, JComponent right) {
        JPanel p = new JPanel(new BorderLayout());
        p.setBorder(BorderFactory.createEmptyBorder(12, 0, 4, 0));
        p.add(Ui.heading(text), BorderLayout.WEST);
        if (right != null) {
            p.add(right, BorderLayout.EAST);
        }
        p.setAlignmentX(LEFT_ALIGNMENT);
        p.setMaximumSize(new java.awt.Dimension(Integer.MAX_VALUE, p.getPreferredSize().height));
        return p;
    }

    /** A card laid out as a grid; {@link #cell} puts things in it. */
    private static JPanel gridCard() {
        JPanel box = Ui.card();
        box.setLayout(new GridBagLayout());
        box.setAlignmentX(LEFT_ALIGNMENT);
        return box;
    }

    private static void cell(JPanel box, Component c, int x, int y, int width, double weight) {
        GridBagConstraints g = new GridBagConstraints();
        g.gridx = x;
        g.gridy = y;
        g.gridwidth = width;
        g.weightx = weight;
        g.anchor = GridBagConstraints.WEST;
        g.fill = weight > 0 || width > 1 ? GridBagConstraints.HORIZONTAL : GridBagConstraints.NONE;
        g.insets = new Insets(3, 0, 3, 10);
        box.add(c, g);
    }

    private void render() {
        int pos = scroll.getVerticalScrollBar().getValue();
        JPanel body = new JPanel();
        body.setLayout(new BoxLayout(body, BoxLayout.Y_AXIS));
        connectionsSection(body);
        programsSection(body);
        matrixSection(body);
        keysSection(body);
        languageSection(body);
        JPanel holder = new StatusPage.WidthTracking(new BorderLayout());
        holder.add(body, BorderLayout.NORTH);
        scroll.setViewportView(holder);
        javax.swing.SwingUtilities.invokeLater(() -> scroll.getVerticalScrollBar().setValue(pos));
    }

    // ── Connections.
    private void connectionsSection(JPanel body) {
        body.add(head(L("Connections", "Conexiones"), small(L("+ Add machine", "+ Añadir máquina"), addMachine)));
        JPanel box = gridCard();
        String[] heads = {L("Name", "Nombre"), L("Address", "Dirección"), L("System", "Sistema"), L("State", "Estado")};
        for (int c = 0; c < heads.length; c++) {
            cell(box, Ui.muted(heads[c]), c, 0, 1, c == 1 ? 1 : 0);
        }
        int row = 1;
        for (Map<String, Object> c : connections) {
            String id = Json.str(c, "id");
            String host = Json.str(c, "host");
            String user = Json.str(c, "user");
            long port = Json.lng(c, "port", 0);
            String addr = host.isEmpty() ? L("this machine", "esta máquina")
                    : (user.isEmpty() ? host : user + "@" + host) + (port > 0 && port != 22 ? ":" + port : "");
            cell(box, new JLabel(Json.str(c, "name", id)), 0, row, 1, 0);
            cell(box, new JLabel(addr), 1, row, 1, 1);
            cell(box, new JLabel(Json.str(c, "os")), 2, row, 1, 0);
            cell(box, new JLabel(Json.bool(c, "connected", true) ? L("connected", "conectada") : L("disconnected", "desconectada")),
                    3, row, 1, 0);
            cell(box, small(L("Test", "Probar"), () -> {
                probeText.put(id, L("Testing…", "Probando…"));
                render();
                store.forMachine(id).request("probe " + url(id), "text", false, r -> {
                    probeText.put(id, (r.out() + r.err()).trim());
                    render();
                });
            }), 4, row, 1, 0);
            cell(box, small(L("Edit…", "Editar…"), () -> editMachine(id)), 5, row, 1, 0);
            row++;
            if (probeText.containsKey(id)) {
                JTextArea p = new JTextArea(probeText.get(id));
                p.setEditable(false);
                p.setOpaque(false);
                p.setFont(new Font(Font.MONOSPACED, Font.PLAIN, p.getFont().getSize()));
                cell(box, p, 0, row++, 6, 1);
            }
        }
        body.add(box);
        body.add(note(L("«Test» checks that SSH gets there, which system there is and whether the sudo password works. The CLI does it; this only shows the answer.",
                "«Probar» comprueba que se llega por SSH, qué sistema hay y si la contraseña de sudo vale. Lo hace el CLI; la ventana solo enseña el resultado.")));
    }

    // ── Programs: zfsmgr-agent and zfsmgr-cli on each machine, each in its own columns —in one
    // row, «up to date» for one and «update» for the other read as a contradiction—.
    private void programsSection(JPanel body) {
        int agents = 0;
        int clis = 0;
        for (Map<String, Object> d : daemons) {
            String a = Json.str(d, "code");
            if (a.equals("update") || a.equals("install")) {
                agents++;
            }
            if (Json.str(d, "cli_code").equals("update")) {
                clis++;
            }
        }
        JButton all = null;
        if (agents + clis > 0) {
            List<String> what = new ArrayList<>();
            if (agents > 0) {
                what.add(agents + " zfsmgr-agent");
            }
            if (clis > 0) {
                what.add(clis + " zfsmgr-cli");
            }
            all = small(L("Update %1", "Actualizar %1", String.join(L(" and ", " y "), what)),
                    () -> act("install-daemon -a", L("Updated.", "Actualizado."), 3000));
        }
        body.add(head(L("Programs", "Programas"), all));
        String cli = store.general().version();
        body.add(left(new JLabel(L("On this computer: zfsmgr-gui (Java), which runs zfsmgr-cli %1.",
                "En este ordenador: zfsmgr-gui (Java), que usa zfsmgr-cli %1.", cli.isEmpty() ? "?" : cli))));
        JPanel box = gridCard();
        String[] heads = {L("Machine", "Máquina"), "zfsmgr-agent", L("What would be done", "Qué se haría"), "zfsmgr-cli",
                          L("What would be done", "Qué se haría")};
        for (int c = 0; c < heads.length; c++) {
            cell(box, Ui.muted(heads[c]), c, 0, 1, c == 2 || c == 4 ? 1 : 0);
        }
        int row = 1;
        if (daemons.isEmpty()) {
            cell(box, Ui.muted(L("Asking…", "Preguntando…")), 0, row, 5, 1);
        }
        for (Map<String, Object> d : daemons) {
            String id = Json.str(d, "connection");
            cell(box, new JLabel(id), 0, row, 1, 0);
            cell(box, new JLabel(Json.str(d, "daemon")), 1, row, 1, 0);
            // The CLI's own words: already in the interface's language.
            cell(box, Ui.wrapped(Json.str(d, "action")), 2, row, 1, 1);
            cell(box, new JLabel(Json.str(d, "cli")), 3, row, 1, 0);
            cell(box, Ui.wrapped(Json.str(d, "cli_action")), 4, row, 1, 1);
            String a = Json.str(d, "code");
            if (a.equals("update") || a.equals("install")) {
                cell(box, small(L("Update its zfsmgr-agent", "Actualizar su zfsmgr-agent"),
                        () -> act("install-daemon --on " + url(id), L("Daemon of %1 updated.", "Daemon de %1 actualizado.", id), 3000)),
                        5, row, 1, 0);
            }
            row++;
        }
        body.add(box);
        body.add(note(L("zfsmgr-agent is the daemon that does the work on each machine; updating it restarts it, and it is checked first that it is not copying anything. zfsmgr-cli is the command line program; updating it restarts nothing. zfsmgr-gui, this window, is only on this computer.",
                "zfsmgr-agent es el daemon que hace el trabajo en cada máquina; actualizarlo lo reinicia, y antes se comprueba que no esté copiando nada. zfsmgr-cli es el programa de línea de órdenes; actualizarlo no reinicia nada. zfsmgr-gui, esta ventana, solo está en este ordenador.")));
    }

    // ── Between machines: can the daemon on the left reach the one on top by itself, with the
    // credentials it holds? What a scheduled copy needs, tested for real (`peers --check`).
    private void matrixSection(JPanel body) {
        body.add(head(L("Between machines", "Entre máquinas"), null));
        body.add(note(L("Each square says whether the machine on the left can connect BY ITSELF to the one on top —what it needs to pull copies from it or to send them to it without going through this computer—. It needs two things: a key of the other one, and the other one listening on the network.",
                "Cada casilla dice si la máquina de la izquierda puede conectarse POR SU CUENTA a la de arriba: lo que necesita para traer copias de ella o enviárselas sin pasar por este ordenador. Le hacen falta dos cosas: una llave de la otra, y que la otra escuche en la red.")));
        body.add(note(L("Keys are full ones: with them a machine pulls copies from another and sends them to it. Adding a machine gives it the keys of all the others and gives it to all of them; «Give all the keys» does it again for everybody.",
                "Las llaves son completas: con ellas una máquina trae copias de otra y se las envía. Al añadir una máquina se le dan las de todas las demás y ella se les da a todas; «Dar todas las llaves» lo repite para todas.")));
        List<String> ids = new ArrayList<>();
        Map<String, String> names = new HashMap<>();
        for (Map<String, Object> c : connections) {
            if (Json.bool(c, "connected", true)) {
                String id = Json.str(c, "id");
                ids.add(id);
                names.put(id, Json.str(c, "name", id));
            }
        }
        JPanel box = gridCard();
        cell(box, new JLabel(L("from ↓  to →", "desde ↓  a →")), 0, 0, 1, 0);
        for (int c = 0; c < ids.size(); c++) {
            cell(box, Ui.heading(names.get(ids.get(c))), c + 1, 0, 1, 1);
        }
        for (int r = 0; r < ids.size(); r++) {
            String from = ids.get(r);
            cell(box, Ui.heading(names.get(from)), 0, r + 1, 1, 0);
            List<Map<String, Object>> mine = checks.get(from);
            for (int c = 0; c < ids.size(); c++) {
                cell(box, square(from, ids.get(c), mine, names), c + 1, r + 1, 1, 1);
            }
            // Keys for machines that are none of this client's: handed over by another one.
            if (mine != null) {
                List<String> stray = new ArrayList<>();
                for (Map<String, Object> o : mine) {
                    String conn = Json.str(o, "connection");
                    if (conn.isEmpty() || conn.equals("-")) {
                        stray.add(Json.str(o, "id"));
                    }
                }
                if (!stray.isEmpty()) {
                    cell(box, Ui.muted(L("Also has keys of %1, which is none of your machines.",
                            "Tiene además llaves de %1, que no es ninguna de tus máquinas.", String.join(", ", stray))),
                            ids.size() + 1, r + 1, 1, 0);
                }
            }
        }
        body.add(box);
        JButton all = new JButton(L("Give all the keys", "Dar todas las llaves"));
        all.setToolTipText(L("Every machine gets the full keys of all the others.", "Cada máquina recibe las llaves completas de todas las demás."));
        all.addActionListener(e -> {
            all.setEnabled(false);
            Keys.giveAll(store.general(), (given, failed) -> {
                say.accept(failed.isEmpty() ? L("Keys given to %1.", "Llaves entregadas a %1.", String.join(", ", given))
                        : L("Keys given to %1. Failed: %2", "Llaves entregadas a %1. Fallaron: %2",
                                given.isEmpty() ? "-" : String.join(", ", given), String.join("; ", failed)));
                checks.clear();
                checkError.clear();
                render();
                load();
            });
        });
        body.add(Ui.row(small(L("Check again", "Comprobar de nuevo"), () -> {
            checks.clear();
            checkError.clear();
            render();
            load();
        }), all));
    }

    /** One square of the matrix: what {@code from} gets when it goes to {@code to}, and the way out. */
    private JComponent square(String from, String to, List<Map<String, Object>> mine, Map<String, String> names) {
        JPanel cell = new JPanel();
        cell.setLayout(new BoxLayout(cell, BoxLayout.Y_AXIS));
        java.awt.Color line = javax.swing.UIManager.getColor("Component.borderColor");
        cell.setBorder(BorderFactory.createCompoundBorder(BorderFactory.createLineBorder(line != null ? line : java.awt.Color.LIGHT_GRAY),
                BorderFactory.createEmptyBorder(4, 6, 4, 6)));
        if (from.equals(to)) {
            cell.add(left(Ui.muted("—")));
            return cell;
        }
        if (mine == null) {
            String why = checkError.getOrDefault(from, "");
            JLabel x = Ui.muted(why.isEmpty() ? L("… checking", "… comprobando") : L("? could not ask it", "? no se le pudo preguntar"));
            x.setToolTipText(why.isEmpty() ? null : why);
            cell.add(left(x));
            return cell;
        }
        Map<String, Object> chk = Map.of();
        for (Map<String, Object> o : mine) {
            if (Json.str(o, "connection").equals(to)) {
                chk = o;
            }
        }
        String state = chk.isEmpty() ? "no_credentials" : Json.str(chk, "state");
        String role = Json.str(chk, "role");
        String detail = Json.str(chk, "detail");
        boolean pullOnly = state.equals("ok") && role.equals("pull");
        String text;
        String look;
        if (pullOnly) {
            look = "warning";
            text = L("▲ reaches it, but only to pull", "▲ llega, pero solo para traer");
        } else if (state.equals("ok")) {
            look = "ok";
            text = L("● reaches it", "● llega");
        } else if (state.equals("no_credentials")) {
            look = "pending";
            text = L("○ has no key", "○ sin llave");
        } else if (state.equals("unreachable")) {
            look = "error";
            text = L("✕ does not get there", "✕ no llega");
        } else if (state.equals("refused")) {
            look = "error";
            text = L("✕ its key is refused", "✕ le rechazan la llave");
        } else {
            look = "warning";
            text = L("▲ unclear", "▲ no está claro");
        }
        JLabel t = new JLabel(text);
        t.setForeground(Ui.colourOf(look));
        t.setToolTipText(detail.isEmpty() ? null : detail);
        cell.add(left(t));
        // Why it does not get there. The daemon on the left looks for the one on top at the
        // address it was given with the keys (`host` of its check): if this computer now has
        // another one, that one is stale. If not, and some other machine does reach the one on
        // top, it does listen on the network: the left one is on a network that does not see that
        // address. Only when nobody reaches it is «it answers only on itself» the likely cause.
        String nowAt = "";
        boolean isLocal = false;
        for (Map<String, Object> c : connections) {
            if (Json.str(c, "id").equals(to)) {
                nowAt = Json.str(c, "host");
                isLocal = Json.str(c, "type").equalsIgnoreCase("LOCAL");
            }
        }
        String lookedAt = Json.str(chk, "host");
        boolean stale = !isLocal && !nowAt.isEmpty() && !lookedAt.isEmpty() && !lookedAt.equalsIgnoreCase(nowAt);
        boolean othersReach = false;
        for (Map.Entry<String, List<Map<String, Object>>> e : checks.entrySet()) {
            if (e.getKey().equals(from) || e.getKey().equals(to)) {
                continue;
            }
            for (Map<String, Object> o : e.getValue()) {
                if (Json.str(o, "connection").equals(to) && Json.str(o, "state").equals("ok")) {
                    othersReach = true;
                }
            }
        }
        Runnable giveKeys = () -> act("peers --push --role full --on " + url(from), L("Keys given.", "Llaves entregadas."), 0);
        if (state.equals("unreachable") && stale) {
            t.setText(L("✕ looks for it at %1", "✕ la busca en %1", lookedAt));
            t.setToolTipText("<html>" + Ui.escape(L("%1 still has the address it was given with the keys (%2); it is %3 now. Giving it keys again gives it the new one.",
                    "%1 sigue con la dirección que recibió con las llaves (%2); ahora es %3. Darle llaves de nuevo le da la nueva.",
                    names.get(from), lookedAt, nowAt)) + "<br><br>" + Ui.escape(detail) + "</html>");
            cell.add(left(small(L("Give it the new address", "Darle la dirección nueva"), giveKeys)));
        } else if (state.equals("unreachable") && othersReach) {
            t.setText(L("✕ does not get there from here", "✕ no llega desde aquí"));
            t.setToolTipText("<html>" + Ui.escape(L("%1 listens on the network —other machines reach it—, but %2 does not get to %3: they are probably on different networks, or a firewall stands between. Give %1 an address %2 can reach (Tailscale, a VPN) and then keys again.",
                    "%1 escucha en la red —otras máquinas llegan a ella—, pero %2 no llega a %3: lo normal es que estén en redes distintas o que haya un cortafuegos en medio. Dale a %1 una dirección a la que %2 llegue (Tailscale, una VPN) y después llaves de nuevo.",
                    names.get(to), names.get(from), lookedAt)) + "<br><br>" + Ui.escape(detail) + "</html>");
        } else if (state.equals("unreachable")) {
            // Nobody gets there: most likely the one on top answers only on itself.
            JButton b = small(L("Open %1 to the network", "Abrir %1 a la red", names.get(to)),
                    () -> act("peers --listen 0.0.0.0 --on " + url(to), L("It listens on the network now.", "Ya escucha en la red."), 8000));
            b.setToolTipText(L("Its daemon restarts.", "Su daemon se reinicia."));
            cell.add(left(b));
        } else if (pullOnly) {
            cell.add(left(small(L("Give it full keys", "Darle llaves completas"), giveKeys)));
        } else if (!state.equals("ok")) {
            cell.add(left(small(state.equals("refused") ? L("Give keys again", "Darle llaves de nuevo") : L("Give keys", "Darle llaves"),
                    giveKeys)));
        }
        return cell;
    }

    // ── Opening ZFSMgr: the master password, and the FIDO2 security keys (any brand) registered to
    // open it besides it (`master-key`). Only with a master password: a key opens what it does.
    private void keysSection(JPanel body) {
        if (keys.isEmpty()) {
            return;
        }
        JButton add = small(L("Add a security key…", "Añadir una llave de seguridad…"), this::addKey);
        add.setToolTipText(L("A FIDO2 security key: touching it opens ZFSMgr instead of typing the master password, which keeps working.",
                "Una llave de seguridad FIDO2: tocarla abre ZFSMgr en vez de escribir la contraseña maestra, que sigue sirviendo."));
        body.add(head(L("Opening ZFSMgr", "Abrir ZFSMgr"), add));
        JPanel box = gridCard();
        int row = 0;
        for (Map<String, Object> k : keys) {
            boolean password = Json.str(k, "type").equals("password");
            String name = Json.str(k, "label");
            cell(box, new JLabel(password ? L("Master password", "Contraseña maestra") : name), 0, row, 1, 0);
            cell(box, Ui.muted(password ? L("always works", "sirve siempre") : L("key: touch it at start", "llave: tocarla al arrancar")),
                    1, row, 1, 1);
            if (!password) {
                String slot = Json.text(k, "slot");
                JButton remove = small(L("Remove…", "Quitar…"),
                        () -> act("master-key remove " + slot, L("Key %1 removed.", "Llave %1 quitada.", name), 0));
                remove.setToolTipText(L("It will no longer open ZFSMgr. Nothing is deleted on the key.",
                        "Dejará de abrir ZFSMgr. En la llave no se borra nada."));
                cell(box, remove, 2, row, 1, 0);
            }
            row++;
        }
        body.add(box);
    }

    // Registering asks for two touches —one to create the credential, one to read its secret— and
    // takes as long as the person does: a note says so while it waits, and goes when it is done.
    // A PIN, if the key wants one, comes as a secret question like any other.
    private void addKey() {
        // Any FIDO2 key with hmac-secret will do, not one brand: empty, the CLI names it after the
        // model the key reports.
        Object name = JOptionPane.showInputDialog(this, L("Plug the key in. What should it be called? (empty: its model)",
                "Conecta la llave. ¿Cómo quieres llamarla? (vacío: su modelo)"), L("Add a security key", "Añadir una llave de seguridad"),
                JOptionPane.PLAIN_MESSAGE, null, null, "");
        if (name == null) {
            return;
        }
        JOptionPane pane = new JOptionPane(Ui.paragraph(L("Touch the key TWICE when it blinks: once to register it and once to read its secret.",
                "Toca la llave DOS veces cuando parpadee: una para registrarla y otra para leer su secreto.")),
                JOptionPane.INFORMATION_MESSAGE, JOptionPane.DEFAULT_OPTION, null, new Object[] {}, null);
        JDialog note = pane.createDialog(this, L("Add a security key", "Añadir una llave de seguridad"));
        note.setModal(false);
        note.setVisible(true);
        String line = "master-key add" + (name.toString().trim().isEmpty() ? "" : " --label " + Ui.quoted(name.toString().trim()));
        store.general().request(line, r -> {
            note.dispose();
            say.accept(r.ok() ? L("Key added: from now on it opens ZFSMgr.", "Llave añadida: desde ahora abre ZFSMgr.") : r.err().trim());
            if (!r.ok()) {
                Ui.showText(this, L("Add a security key", "Añadir una llave de seguridad"), r.err().trim());
            }
            load();
        });
    }

    // ── Language: the system's unless one is chosen; it applies on the next start, since every
    // text —the CLI's too— is chosen once.
    private void languageSection(JPanel body) {
        body.add(head(L("Language", "Idioma"), null));
        java.util.prefs.Preferences prefs = java.util.prefs.Preferences.userNodeForPackage(MainWindow.class);
        JComboBox<NewCopyWizard.Item<String>> lang = new JComboBox<>();
        lang.addItem(new NewCopyWizard.Item<>(L("The system's", "El del sistema"), ""));
        lang.addItem(new NewCopyWizard.Item<>("Español", "es"));
        lang.addItem(new NewCopyWizard.Item<>("English", "en"));
        String now = prefs.get("lang", "");
        for (int i = 0; i < lang.getItemCount(); i++) {
            if (lang.getItemAt(i).value().equals(now)) {
                lang.setSelectedIndex(i);
            }
        }
        lang.addActionListener(e -> {
            @SuppressWarnings("unchecked")
            NewCopyWizard.Item<String> it = (NewCopyWizard.Item<String>) lang.getSelectedItem();
            prefs.put("lang", it == null ? "" : it.value());
            say.accept(L("The language changes the next time ZFSMgr is opened.", "El idioma cambia la próxima vez que se abra ZFSMgr."));
        });
        body.add(Ui.row(lang));
    }

    private void editMachine(String id) {
        Map<String, Object> c = Map.of();
        for (Map<String, Object> x : connections) {
            if (Json.str(x, "id").equals(id)) {
                c = x;
            }
        }
        boolean local = Json.str(c, "host").isEmpty();
        JTextField name = new JTextField(Json.str(c, "name"), 24);
        JTextField host = new JTextField(Json.str(c, "host"), 24);
        JTextField user = new JTextField(Json.str(c, "user"), 24);
        long p0 = Json.lng(c, "port", 22);
        JSpinner port = new JSpinner(new SpinnerNumberModel((int) (p0 > 0 ? p0 : 22), 1, 65535, 1));
        JTextField key = new JTextField(24);
        key.putClientProperty("JTextField.placeholderText", L("empty: keep as it is", "vacío: dejar como está"));
        JCheckBox sudo = new JCheckBox(L("Needs sudo", "Necesita sudo"), Json.bool(c, "sudo", false));
        Ui.Form form = new Ui.Form();
        form.row(L("Name shown", "Nombre visible"), name);
        if (!local) {
            form.row(L("Host", "Máquina (host)"), host).row(L("User", "Usuario"), user).row(L("Port", "Puerto"), port)
                    .row(L("SSH key", "Clave SSH"), key);
        }
        form.row("", sudo);
        JPanel p = new JPanel(new BorderLayout(0, 8));
        p.add(form, BorderLayout.NORTH);
        p.add(Ui.paragraph(L("The password is asked next: leave it empty to keep the stored one.",
                "La contraseña se pregunta después: déjala vacía para conservar la guardada.")), BorderLayout.CENTER);
        if (JOptionPane.showConfirmDialog(this, p, L("Edit %1", "Editar %1", id), JOptionPane.OK_CANCEL_OPTION,
                JOptionPane.PLAIN_MESSAGE) != JOptionPane.OK_OPTION) {
            return;
        }
        // Only what changed goes on the line.
        StringBuilder line = new StringBuilder("edit " + url(id));
        if (!name.getText().trim().equals(Json.str(c, "name"))) {
            line.append(" --name ").append(Ui.quoted(name.getText().trim()));
        }
        if (!local) {
            if (!host.getText().trim().equals(Json.str(c, "host"))) {
                line.append(" --host ").append(Ui.quoted(host.getText().trim()));
            }
            if (!user.getText().trim().equals(Json.str(c, "user"))) {
                line.append(" --user ").append(Ui.quoted(user.getText().trim()));
            }
            if (((Integer) port.getValue()) != Json.lng(c, "port", 22)) {
                line.append(" --port ").append(port.getValue());
            }
            if (!key.getText().trim().isEmpty()) {
                line.append(" --key ").append(Ui.quoted(key.getText().trim()));
            }
        }
        if (sudo.isSelected() != Json.bool(c, "sudo", false)) {
            line.append(sudo.isSelected() ? " --sudo" : " --no-sudo");
        }
        act(line.toString(), L("%1 changed.", "%1 cambiada.", id), 0);
    }
}
