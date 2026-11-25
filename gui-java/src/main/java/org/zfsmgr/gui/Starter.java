package org.zfsmgr.gui;

import static org.zfsmgr.gui.Text.L;

import java.awt.Color;
import java.util.Arrays;
import java.util.Map;
import javax.swing.BorderFactory;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JWindow;
import javax.swing.SwingUtilities;

/**
 * From nothing to the window. The general service is
 * started with an EMPTY first line (or the password given with --password-fd), and what it
 * answers decides the next step:
 *
 * <ul>
 *   <li>the session is someone else's: say whose and offer to close it ({@code --take-session});
 *   <li>keys are registered: the unlock window, touch or password;
 *   <li>no master password yet: the welcome, which creates it;
 *   <li>the password does not open: ask for it (again);
 *   <li>ready: the window.
 * </ul>
 *
 * The session lock and the presence are the general service's ({@code --session},
 * {@code --presence gui}): the interface has no native code of its own.
 */
public final class Starter {
    private final String cli;
    private CliService.Session sessionMode = CliService.Session.TAKE;
    /** What the current attempt was started with: needed again if it must be retried. */
    private char[] pending = new char[0];
    private boolean asked;
    private boolean firstRun;
    private JWindow opening;
    private UnlockDialog unlock;
    private MainWindow window;
    /** Instead of the window: the installer takes the open service (no presence of a GUI). */
    private final java.util.function.Consumer<CliService> onReady;
    private boolean handedOver;

    public Starter(String cli) {
        this(cli, null);
    }

    public Starter(String cli, java.util.function.Consumer<CliService> onReady) {
        this.cli = cli;
        this.onReady = onReady;
    }

    public void attempt(char[] master) {
        // Kept until the window opens: a busy session means starting again with the same one.
        char[] copy = master.clone();
        if (pending != master) {
            Arrays.fill(pending, '\0');
            Arrays.fill(master, '\0');
        }
        pending = copy;
        showOpening();
        final CliService[] self = new CliService[1];
        CliService s = new CliService(cli, Text.lang(), SwingUtilities::invokeLater, new CliService.Listener() {
            @Override
            public void ready(Map<String, Object> ev) {
                Timing.mark("service_ready");
                hideOpening();
                CliService svc = self[0];
                // No master password yet: the welcome creates it, and the service starts
                // again with it —the CLI records the first master it is given—.
                if (svc.noMaster() && pending.length == 0) {
                    svc.stop(1500);
                    char[] chosen = Dialogs.welcome();
                    if (chosen == null) {
                        System.exit(0);
                    }
                    firstRun = true;
                    attempt(chosen);
                    return;
                }
                if (unlock != null) {
                    unlock.dispose();
                    unlock = null;
                }
                Arrays.fill(pending, '\0');
                if (onReady != null) {
                    handedOver = true;
                    onReady.accept(svc);
                    return;
                }
                window = new MainWindow(cli, svc);
                Main.showWindow(window);
                if (firstRun) {
                    window.startFirstRun();
                }
            }

            @Override
            public void failed(String reason, boolean wrongPassword) {
                hideOpening();
                // Its owner now: a service that ends after `network seed` ends on purpose.
                if (handedOver) {
                    return;
                }
                if (window != null) {
                    window.serviceLost(reason);
                    return;
                }
                if (!wrongPassword) {
                    JOptionPane.showMessageDialog(null, L("The command line client could not start:\n%1",
                            "No se pudo arrancar el cliente de línea de órdenes:\n%1", reason), "ZFSMgr",
                            JOptionPane.ERROR_MESSAGE);
                    System.exit(1);
                }
                char[] again = Dialogs.master(asked);
                asked = true;
                if (again == null) {
                    System.exit(0);
                }
                attempt(again);
            }

            @Override
            public void sessionBusy(String text, boolean couldNotClose) {
                hideOpening();
                if (couldNotClose) {
                    JOptionPane.showMessageDialog(null, text, "ZFSMgr", JOptionPane.ERROR_MESSAGE);
                    System.exit(1);
                }
                Object[] options = {L("Close that session and open this one", "Cerrar esa sesión y abrir esta"),
                                    L("Leave", "Salir")};
                int r = JOptionPane.showOptionDialog(null, Ui.paragraph(text), "ZFSMgr",
                        JOptionPane.DEFAULT_OPTION, JOptionPane.WARNING_MESSAGE, null, options, options[1]);
                if (r != 0) {
                    System.exit(0);
                }
                sessionMode = CliService.Session.TAKE_BY_FORCE;
                attempt(pending);
            }

            @Override
            public void locked(boolean keyPresent) {
                hideOpening();
                CliService svc = self[0];
                unlock = new UnlockDialog(keyPresent, svc::unlockWithPassword, () -> System.exit(0));
                unlock.setVisible(true);
            }

            @Override
            public void touchWaiting() {
                if (unlock != null) {
                    unlock.setKeyPresent(true);
                }
            }

            @Override
            public void keyPresence(boolean present) {
                if (unlock != null) {
                    unlock.setKeyPresent(present);
                }
            }

            @Override
            public void unlockFailed(String reason, String code) {
                if (unlock == null) {
                    return;
                }
                if (reason.equals("password")) {
                    unlock.setPasswordWrong();
                } else {
                    unlock.setKeyFailed(code);
                }
            }

            @Override
            public void confirm(CliService service, CliService.Question q) {
                Dialogs.Answer a = Dialogs.question(window, q);
                service.answerConfirm(q.id(), a.yes(), a.typed());
            }

            @Override
            public void secret(CliService service, int id, String text) {
                service.answerSecret(id, Dialogs.secret(window, text));
            }

            @Override
            public void closeAsked(String host, String user) {
                if (window != null) {
                    window.closeAsked(host, user);
                }
            }
        });
        self[0] = s;
        s.setPresence(onReady == null);
        s.setSession(sessionMode);
        s.start(pending.clone(), false);
    }

    /** Something on screen while the service starts: on Windows it takes a couple of seconds. */
    private void showOpening() {
        if (opening != null) {
            return;
        }
        opening = new JWindow();
        JLabel t = new JLabel(L("Opening ZFSMgr…", "Abriendo ZFSMgr…"));
        t.setBorder(BorderFactory.createCompoundBorder(BorderFactory.createLineBorder(Color.GRAY),
                BorderFactory.createEmptyBorder(16, 28, 16, 28)));
        opening.getContentPane().add(t);
        opening.pack();
        opening.setLocationRelativeTo(null);
        opening.setVisible(true);
    }

    private void hideOpening() {
        if (opening != null) {
            opening.dispose();
            opening = null;
        }
    }
}
