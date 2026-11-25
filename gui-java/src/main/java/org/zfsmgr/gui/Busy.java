package org.zfsmgr.gui;

import java.awt.Cursor;
import java.awt.Window;
import javax.swing.SwingUtilities;

/**
 * The wait cursor while the interface waits for the CLI: every request the person started
 * —creating a pool, a copy, a tree that opens— shows it on every window until its answer
 * arrives. The periodic reads that refresh the status on their own do not (CliService's
 * {@code requestQuiet}): they would make the cursor blink with nobody doing anything.
 */
final class Busy {
    private static int count;

    private Busy() {}

    static void begin() {
        onSwing(() -> {
            count++;
            apply();
        });
    }

    static void end() {
        onSwing(() -> {
            count = Math.max(0, count - 1);
            apply();
        });
    }

    static boolean on() {
        return count > 0;
    }

    private static void apply() {
        Cursor c = count > 0 ? Cursor.getPredefinedCursor(Cursor.WAIT_CURSOR) : Cursor.getDefaultCursor();
        for (Window w : Window.getWindows()) {
            if (w.isDisplayable()) {
                w.setCursor(c);
            }
        }
    }

    private static void onSwing(Runnable r) {
        if (SwingUtilities.isEventDispatchThread()) {
            r.run();
        } else {
            SwingUtilities.invokeLater(r);
        }
    }
}
