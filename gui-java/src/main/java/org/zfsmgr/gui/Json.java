package org.zfsmgr.gui;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The little JSON the protocol needs, with no library: objects are {@code Map<String,Object>}
 * (in order), arrays {@code List<Object>}, numbers {@code Long} or {@code Double}, plus
 * {@code String}, {@code Boolean} and {@code null}. And typed reads that never throw: a missing or
 * different field gives the default, as Qt's QJsonValue did.
 */
public final class Json {
    private Json() {}

    public static final class ParseException extends RuntimeException {
        ParseException(String what, int at) {
            super(what + " at " + at);
        }
    }

    public static Object parse(String text) {
        Parser p = new Parser(text);
        p.ws();
        Object v = p.value();
        p.ws();
        return v;
    }

    /** The FIRST object in a text: a warning that slipped onto stdout before it does not matter. */
    @SuppressWarnings("unchecked")
    public static Map<String, Object> firstObject(String text) {
        if (text == null) {
            return Map.of();
        }
        int start = text.indexOf('{');
        if (start < 0) {
            return Map.of();
        }
        try {
            Parser p = new Parser(text.substring(start));
            Object v = p.value();
            return v instanceof Map ? (Map<String, Object>) v : Map.of();
        } catch (ParseException e) {
            return Map.of();
        }
    }

    public static String str(Map<String, Object> o, String key) {
        return str(o, key, "");
    }

    public static String str(Map<String, Object> o, String key, String def) {
        Object v = o.get(key);
        return v instanceof String s ? s : def;
    }

    public static long num(Map<String, Object> o, String key, long def) {
        Object v = o.get(key);
        return v instanceof Number n ? n.longValue() : def;
    }

    /** A string, or a number or boolean written as one: what a table shows. */
    public static String text(Map<String, Object> o, String key) {
        Object v = o.get(key);
        if (v instanceof String s) {
            return s;
        }
        if (v instanceof Double d && d == Math.rint(d) && !d.isInfinite()) {
            return String.valueOf(d.longValue());
        }
        return v == null ? "" : String.valueOf(v);
    }

    /** A whole number, or one written as text («"creation": "1770200629"»). */
    public static long lng(Map<String, Object> o, String key, long def) {
        Object v = o.get(key);
        if (v instanceof Number n) {
            return n.longValue();
        }
        if (v instanceof String s) {
            try {
                return Long.parseLong(s.trim());
            } catch (NumberFormatException e) {
                return def;
            }
        }
        return def;
    }

    /** A number, or a number written as text («"rate": "159.18"», as `jobs` gives it). */
    public static double dbl(Map<String, Object> o, String key, double def) {
        Object v = o.get(key);
        if (v instanceof Number n) {
            return n.doubleValue();
        }
        if (v instanceof String s) {
            try {
                return Double.parseDouble(s.trim());
            } catch (NumberFormatException e) {
                return def;
            }
        }
        return def;
    }

    public static boolean bool(Map<String, Object> o, String key, boolean def) {
        Object v = o.get(key);
        return v instanceof Boolean b ? b : def;
    }

    @SuppressWarnings("unchecked")
    public static Map<String, Object> obj(Map<String, Object> o, String key) {
        Object v = o.get(key);
        return v instanceof Map ? (Map<String, Object>) v : Map.of();
    }

    /** The objects of an array; whatever is not an object is skipped. */
    @SuppressWarnings("unchecked")
    public static List<Map<String, Object>> objects(Map<String, Object> o, String key) {
        Object v = o.get(key);
        List<Map<String, Object>> out = new ArrayList<>();
        if (v instanceof List<?> l) {
            for (Object x : l) {
                if (x instanceof Map) {
                    out.add((Map<String, Object>) x);
                }
            }
        }
        return out;
    }

    public static List<String> strings(Map<String, Object> o, String key) {
        Object v = o.get(key);
        List<String> out = new ArrayList<>();
        if (v instanceof List<?> l) {
            for (Object x : l) {
                if (x instanceof String s) {
                    out.add(s);
                }
            }
        }
        return out;
    }

    /** An object written on one line, keys in the order given: {@code write("id", 7, "line", "ls")}. */
    public static String write(Object... keyValues) {
        StringBuilder b = new StringBuilder("{");
        for (int i = 0; i + 1 < keyValues.length; i += 2) {
            if (i > 0) {
                b.append(',');
            }
            quote(b, String.valueOf(keyValues[i]));
            b.append(':');
            value(b, keyValues[i + 1]);
        }
        return b.append('}').toString();
    }

    private static void value(StringBuilder b, Object v) {
        if (v == null) {
            b.append("null");
        } else if (v instanceof String s) {
            quote(b, s);
        } else if (v instanceof Boolean || v instanceof Number) {
            b.append(v);
        } else {
            quote(b, v.toString());
        }
    }

    static void quote(StringBuilder b, String s) {
        b.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"' -> b.append("\\\"");
                case '\\' -> b.append("\\\\");
                case '\n' -> b.append("\\n");
                case '\r' -> b.append("\\r");
                case '\t' -> b.append("\\t");
                default -> {
                    if (c < 0x20) {
                        b.append(String.format("\\u%04x", (int) c));
                    } else {
                        b.append(c);
                    }
                }
            }
        }
        b.append('"');
    }

    private static final class Parser {
        private final String s;
        private int i;

        Parser(String s) {
            this.s = s;
        }

        void ws() {
            while (i < s.length() && Character.isWhitespace(s.charAt(i))) {
                i++;
            }
        }

        Object value() {
            ws();
            if (i >= s.length()) {
                throw new ParseException("unexpected end", i);
            }
            char c = s.charAt(i);
            return switch (c) {
                case '{' -> object();
                case '[' -> array();
                case '"' -> string();
                case 't' -> literal("true", Boolean.TRUE);
                case 'f' -> literal("false", Boolean.FALSE);
                case 'n' -> literal("null", null);
                default -> number();
            };
        }

        Object literal(String word, Object v) {
            if (!s.startsWith(word, i)) {
                throw new ParseException("bad literal", i);
            }
            i += word.length();
            return v;
        }

        Map<String, Object> object() {
            Map<String, Object> m = new LinkedHashMap<>();
            i++;
            ws();
            if (i < s.length() && s.charAt(i) == '}') {
                i++;
                return m;
            }
            while (true) {
                ws();
                if (i >= s.length() || s.charAt(i) != '"') {
                    throw new ParseException("expected a key", i);
                }
                String k = string();
                ws();
                expect(':');
                m.put(k, value());
                ws();
                if (i < s.length() && s.charAt(i) == ',') {
                    i++;
                    continue;
                }
                expect('}');
                return m;
            }
        }

        List<Object> array() {
            List<Object> l = new ArrayList<>();
            i++;
            ws();
            if (i < s.length() && s.charAt(i) == ']') {
                i++;
                return l;
            }
            while (true) {
                l.add(value());
                ws();
                if (i < s.length() && s.charAt(i) == ',') {
                    i++;
                    continue;
                }
                expect(']');
                return l;
            }
        }

        void expect(char c) {
            if (i >= s.length() || s.charAt(i) != c) {
                throw new ParseException("expected '" + c + "'", i);
            }
            i++;
        }

        String string() {
            i++;
            StringBuilder b = new StringBuilder();
            while (i < s.length()) {
                char c = s.charAt(i++);
                if (c == '"') {
                    return b.toString();
                }
                if (c != '\\') {
                    b.append(c);
                    continue;
                }
                if (i >= s.length()) {
                    break;
                }
                char e = s.charAt(i++);
                switch (e) {
                    case 'n' -> b.append('\n');
                    case 't' -> b.append('\t');
                    case 'r' -> b.append('\r');
                    case 'b' -> b.append('\b');
                    case 'f' -> b.append('\f');
                    case 'u' -> {
                        if (i + 4 > s.length()) {
                            throw new ParseException("bad escape", i);
                        }
                        b.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                        i += 4;
                    }
                    default -> b.append(e);
                }
            }
            throw new ParseException("unterminated string", i);
        }

        Object number() {
            int start = i;
            while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) {
                i++;
            }
            String t = s.substring(start, i);
            if (t.isEmpty()) {
                throw new ParseException("unexpected character", i);
            }
            try {
                if (t.contains(".") || t.contains("e") || t.contains("E")) {
                    return Double.parseDouble(t);
                }
                return Long.parseLong(t);
            } catch (NumberFormatException ex) {
                throw new ParseException("bad number", start);
            }
        }
    }
}
