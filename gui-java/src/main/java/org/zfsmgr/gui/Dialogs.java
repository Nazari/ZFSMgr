package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.Component;
import java.awt.Font;
import java.util.Arrays;
import java.util.List;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JComponent;
import javax.swing.JDialog;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JPasswordField;
import javax.swing.JTextField;
import javax.swing.event.DocumentEvent;
import javax.swing.event.DocumentListener;

/** The modal dialogs: the CLI's question, a secret, the master password, the welcome. */
public final class Dialogs {
    private Dialogs() {}

    private static Component left(JComponent c) {
        c.setAlignmentX(Component.LEFT_ALIGNMENT);
        return c;
    }

    private static DocumentListener onChange(Runnable r) {
        return new DocumentListener() {
            @Override
            public void insertUpdate(DocumentEvent e) {
                r.run();
            }

            @Override
            public void removeUpdate(DocumentEvent e) {
                r.run();
            }

            @Override
            public void changedUpdate(DocumentEvent e) {
                r.run();
            }
        };
    }

    public record Answer(boolean yes, String typed) {}

    /**
     * The CLI's {@code confirm}, shown as it is written: what is lost, what is kept, the notes, and
     * the word to type when it asks for one. The safe choice is the default. (src/gui/questiondialog.cpp.)
     */
    public static Answer question(Component parent, CliService.Question q) {
        JDialog d = new JDialog(javax.swing.SwingUtilities.getWindowAncestor(parent), q.title(),
                java.awt.Dialog.ModalityType.APPLICATION_MODAL);
        JPanel p = new JPanel();
        p.setLayout(new BoxLayout(p, BoxLayout.Y_AXIS));
        p.setBorder(BorderFactory.createEmptyBorder(14, 16, 14, 16));
        JLabel title = new JLabel("<html>" + Ui.escape(q.title()) + "</html>");
        title.setFont(title.getFont().deriveFont(Font.BOLD, title.getFont().getSize2D() * 1.3f));
        p.add(left(title));
        if (q.irreversible()) {
            JLabel w = new JLabel(L("This cannot be undone.", "Esto no se puede deshacer."));
            w.setForeground(Ui.colourOf("error"));
            w.setFont(w.getFont().deriveFont(Font.BOLD));
            p.add(left(w));
        }
        addList(p, q.lose(), q.irreversible() ? L("Lost for good:", "Se pierde para siempre:") : L("Lost:", "Se pierde:"));
        addList(p, q.keep(), L("Kept:", "Se conserva:"));
        for (String n : q.notes()) {
            p.add(Box.createVerticalStrut(6));
            p.add(left(Ui.wrapped(n)));
        }
        JButton yes = new JButton(q.irreversible() ? L("Do it for good", "Hacerlo para siempre") : L("Continue", "Seguir"));
        JButton no = new JButton(L("Cancel", "Cancelar"));
        if (q.irreversible()) {
            yes.setBackground(Ui.colourOf("error"));
            yes.setForeground(java.awt.Color.WHITE);
        }
        JTextField word = new JTextField(20);
        if (!q.typeToConfirm().isEmpty()) {
            p.add(Box.createVerticalStrut(8));
            p.add(left(new JLabel("<html>" + L("To confirm, type %1:", "Para confirmar, escribe %1:",
                    "<b>" + Ui.escape(q.typeToConfirm()) + "</b>") + "</html>")));
            p.add(left(word));
            yes.setEnabled(false);
            word.getDocument().addDocumentListener(onChange(
                    () -> yes.setEnabled(word.getText().trim().equals(q.typeToConfirm()))));
        }
        JPanel buttons = new JPanel();
        buttons.setLayout(new BoxLayout(buttons, BoxLayout.X_AXIS));
        buttons.add(Box.createHorizontalGlue());
        buttons.add(no);
        buttons.add(Box.createHorizontalStrut(8));
        buttons.add(yes);
        p.add(Box.createVerticalStrut(12));
        p.add(left(buttons));
        final boolean[] said = {false};
        yes.addActionListener(e -> {
            said[0] = true;
            d.dispose();
        });
        no.addActionListener(e -> d.dispose());
        // Enter does not destroy anything: the default is «Cancel» unless a word is typed.
        d.getRootPane().setDefaultButton(q.typeToConfirm().isEmpty() ? no : yes);
        d.getContentPane().add(p);
        d.pack();
        d.setSize(Math.max(d.getWidth(), 500), d.getHeight());
        d.setLocationRelativeTo(parent);
        d.setVisible(true);
        return new Answer(said[0], said[0] ? word.getText().trim() : "");
    }

    private static void addList(JPanel p, List<String> items, String heading) {
        if (items.isEmpty()) {
            return;
        }
        p.add(Box.createVerticalStrut(6));
        p.add(left(new JLabel(heading)));
        for (String i : items) {
            p.add(left(Ui.wrapped("  • " + i)));
        }
    }

    /** A secret the CLI asks for; null if the person does not give it. */
    public static char[] secret(Component parent, String text) {
        JPasswordField f = new JPasswordField(24);
        int r = JOptionPane.showConfirmDialog(parent, new Object[] {text, f}, "ZFSMgr",
                JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE);
        return r == JOptionPane.OK_OPTION ? f.getPassword() : null;
    }

    /** The master password; null if cancelled. */
    public static char[] master(boolean again) {
        JPasswordField f = new JPasswordField(24);
        String text = again ? L("That is not the master password. Try again:", "Esa no es la contraseña maestra. Prueba otra vez:")
                            : L("Master password (the one that encrypts the stored passwords):",
                                "Contraseña maestra (la que cifra las contraseñas guardadas):");
        Timing.mark("password_dialog");
        int r = JOptionPane.showConfirmDialog(null, new Object[] {text, f}, "ZFSMgr",
                JOptionPane.OK_CANCEL_OPTION, JOptionPane.PLAIN_MESSAGE);
        if (r != JOptionPane.OK_OPTION) {
            return null;
        }
        Timing.mark("password_given");
        return f.getPassword();
    }

    /**
     * The first run on this computer, or a configuration that never had a master password: it is
     * chosen here, typed twice. Null if the person leaves. (main.cpp, welcome.)
     */
    public static char[] welcome() {
        JPasswordField one = new JPasswordField(22);
        JPasswordField two = new JPasswordField(22);
        JLabel why = new JLabel(" ");
        why.setForeground(Ui.colourOf("error"));
        JPanel p = new JPanel();
        p.setLayout(new BoxLayout(p, BoxLayout.Y_AXIS));
        JLabel head = new JLabel(L("Welcome to ZFSMgr", "Bienvenido a ZFSMgr"));
        head.setFont(head.getFont().deriveFont(Font.BOLD));
        p.add(left(head));
        JLabel text = new JLabel("<html><div style='width:360px'>" + L(
                "This is the first time on this computer. Choose a master password: it encrypts the passwords of your machines, which ZFSMgr keeps here, and it will be asked for every time it starts. It is not stored anywhere: if it is forgotten, those passwords have to be entered again.",
                "Es la primera vez en este ordenador. Elige una contraseña maestra: cifra las contraseñas de tus máquinas, que ZFSMgr guarda aquí, y se pedirá cada vez que arranque. No se guarda en ningún sitio: si se olvida, hay que volver a dar esas contraseñas.")
                + "</div></html>");
        p.add(left(text));
        p.add(Box.createVerticalStrut(8));
        p.add(left(new JLabel(L("Master password", "Contraseña maestra"))));
        p.add(left(one));
        p.add(left(new JLabel(L("Again", "Repítela"))));
        p.add(left(two));
        p.add(left(why));
        while (true) {
            int r = JOptionPane.showConfirmDialog(null, p, "ZFSMgr", JOptionPane.OK_CANCEL_OPTION,
                    JOptionPane.PLAIN_MESSAGE);
            if (r != JOptionPane.OK_OPTION) {
                return null;
            }
            char[] a = one.getPassword();
            char[] b = two.getPassword();
            if (a.length == 0) {
                why.setText(L("Type it twice.", "Escríbela dos veces."));
            } else if (!Arrays.equals(a, b)) {
                why.setText(L("The two are not the same.", "Las dos no coinciden."));
            } else {
                Arrays.fill(b, '\0');
                return a;
            }
            Arrays.fill(a, '\0');
            Arrays.fill(b, '\0');
        }
    }
}
