package org.zfsmgr.gui;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * The spike's measurements: milliseconds since the JVM started for each milestone, and the
 * memory of this process (resident, and Java heap in use). One line each on stderr,
 * «[timing] …», so they can be compared across machines.
 */
public final class Timing {
    private static final long START = ManagementFactory.getRuntimeMXBean().getStartTime();

    private Timing() {}

    public static void mark(String what) {
        System.err.printf("[timing] %-22s %6d ms%n", what, System.currentTimeMillis() - START);
    }

    public static void memory(String what) {
        Runtime rt = Runtime.getRuntime();
        long heap = (rt.totalMemory() - rt.freeMemory()) / (1024 * 1024);
        System.err.printf("[timing] %-22s rss %s MiB, heap %d MiB%n", what, rssMiB(), heap);
    }

    /** The resident size: /proc on Linux, `ps` on macOS and FreeBSD, `tasklist` on Windows. */
    static String rssMiB() {
        long pid = ProcessHandle.current().pid();
        try {
            Path status = Path.of("/proc/self/status");
            if (Files.isReadable(status)) {
                for (String l : Files.readAllLines(status)) {
                    if (l.startsWith("VmRSS:")) {
                        return String.valueOf(Long.parseLong(l.replaceAll("[^0-9]", "")) / 1024);
                    }
                }
            }
            boolean windows = System.getProperty("os.name").toLowerCase().contains("win");
            Process p = new ProcessBuilder(windows
                    ? new String[] {"tasklist", "/FI", "PID eq " + pid, "/FO", "CSV", "/NH"}
                    : new String[] {"ps", "-o", "rss=", "-p", String.valueOf(pid)})
                    .redirectErrorStream(true).start();
            String out;
            try (BufferedReader r = new BufferedReader(
                    new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                out = r.lines().reduce("", (a, b) -> a + b);
            }
            if (windows) {
                // "java.exe","1234","Console","1","123.456 K"
                String[] f = out.split("\",\"");
                String k = f[f.length - 1].replaceAll("[^0-9]", "");
                return String.valueOf(Long.parseLong(k) / 1024);
            }
            return String.valueOf(Long.parseLong(out.trim()) / 1024);
        } catch (Exception e) {
            return "?";
        }
    }
}
