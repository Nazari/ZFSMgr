package org.zfsmgr.gui;

import java.util.Locale;

/** The interface's language: Spanish or English ({@code L(en, es)}). */
public final class Text {
    private static String lang = Locale.getDefault().getLanguage().equals("es") ? "es" : "en";

    private Text() {}

    public static String lang() {
        return lang;
    }

    public static void setLang(String l) {
        lang = "es".equals(l) ? "es" : "en";
    }

    /** The text in the interface's language; {@code %1}, {@code %2}… are filled with {@code args}. */
    public static String L(String en, String es, Object... args) {
        String t = lang.equals("es") ? es : en;
        for (int i = args.length; i >= 1; i--) {
            t = t.replace("%" + i, String.valueOf(args[i - 1]));
        }
        return t;
    }
}
