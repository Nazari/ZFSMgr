package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.BorderLayout;
import java.awt.Font;
import java.util.function.Consumer;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JDialog;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JPasswordField;

/**
 * The start window when FIDO2 keys are registered: «touch your key» and the master password at
 * once, whichever comes first. The CLI's service does the work (it is locked: «Cerrado» in
 * docs/protocolo_serve.md); this shows what it says and hands it the password.
 */
public final class UnlockDialog extends JDialog {
    private final JLabel key = new JLabel();
    private final JLabel keyNote = Ui.muted(" ");
    private final JPasswordField password = new JPasswordField(24);
    private final JButton enter = new JButton(L("Enter", "Entrar"));
    private final JLabel error = new JLabel(" ");

    public UnlockDialog(boolean keyPresent, Consumer<char[]> onPassword, Runnable onClose) {
        super((java.awt.Frame) null, "ZFSMgr", false);
        JPanel p = new JPanel();
        p.setLayout(new BoxLayout(p, BoxLayout.Y_AXIS));
        p.setBorder(BorderFactory.createEmptyBorder(14, 16, 14, 16));
        key.setFont(key.getFont().deriveFont(Font.BOLD, key.getFont().getSize2D() * 1.3f));
        p.add(left(key));
        p.add(left(keyNote));
        p.add(Box.createVerticalStrut(12));
        p.add(left(new JLabel(L("Or type the master password:", "O escribe la contraseña maestra:"))));
        JPanel row = new JPanel(new BorderLayout(6, 0));
        row.add(password, BorderLayout.CENTER);
        row.add(enter, BorderLayout.EAST);
        p.add(left(row));
        error.setForeground(Ui.colourOf("error"));
        p.add(left(error));
        getContentPane().add(p);
        Runnable send = () -> {
            if (password.getPassword().length == 0) {
                return;
            }
            error.setText(" ");
            password.setEnabled(false);
            enter.setEnabled(false);
            onPassword.accept(password.getPassword());
        };
        enter.addActionListener(e -> send.run());
        password.addActionListener(e -> send.run());
        getRootPane().setDefaultButton(enter);
        setDefaultCloseOperation(DISPOSE_ON_CLOSE);
        addWindowListener(new java.awt.event.WindowAdapter() {
            @Override
            public void windowClosing(java.awt.event.WindowEvent e) {
                onClose.run();
            }
        });
        setKeyPresent(keyPresent);
        pack();
        setSize(Math.max(getWidth(), 460), getHeight());
        setLocationRelativeTo(null);
    }

    private static java.awt.Component left(javax.swing.JComponent c) {
        c.setAlignmentX(LEFT_ALIGNMENT);
        return c;
    }

    public void setKeyPresent(boolean present) {
        if (present) {
            key.setText(L("Touch your key to get in", "Toca tu llave para entrar"));
            keyNote.setText(L("It blinks while it waits.", "Parpadea mientras espera."));
        } else {
            key.setText(L("Plug in your key and touch it", "Conecta tu llave y tócala"));
            keyNote.setText(L("None is connected now; it is noticed as soon as it is.",
                    "Ahora no hay ninguna conectada; se nota en cuanto la conectes."));
        }
    }

    public void setPasswordWrong() {
        password.setEnabled(true);
        enter.setEnabled(true);
        password.setText("");
        password.requestFocusInWindow();
        error.setText(L("That is not the master password.", "Esa no es la contraseña maestra."));
    }

    public void setKeyFailed(String code) {
        keyNote.setText(switch (code) {
            case "not_registered" -> L("This key is not registered here: use the master password, or another key.",
                    "Esta llave no está registrada aquí: usa la contraseña maestra u otra llave.");
            case "pin_required" -> L("This key asks for its PIN, which is not supported at start yet: use the master password.",
                    "Esta llave pide su PIN, que al arrancar todavía no se admite: usa la contraseña maestra.");
            default -> L("The key failed; touch it again or use the master password.",
                    "La llave falló; tócala otra vez o usa la contraseña maestra.");
        });
    }
}
