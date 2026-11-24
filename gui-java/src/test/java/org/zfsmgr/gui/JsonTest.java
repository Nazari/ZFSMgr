package org.zfsmgr.gui;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class JsonTest {
    @Test
    void parsesWhatTheServiceSays() {
        Map<String, Object> o = Json.firstObject(
                "aviso suelto\n{\"type\":\"result\",\"id\":7,\"rc\":0,\"stdout\":\"a\\nb \\u00e1\","
                + "\"rate\":94.1,\"ok\":true,\"none\":null,\"list\":[1,\"x\",{\"k\":\"v\"}]}");
        assertEquals("result", Json.str(o, "type"));
        assertEquals(7, Json.num(o, "id", 0));
        assertEquals("a\nb á", Json.str(o, "stdout"));
        assertEquals(94.1, Json.dbl(o, "rate", 0), 1e-9);
        assertTrue(Json.bool(o, "ok", false));
        assertEquals(1, Json.objects(o, "list").size());
        assertEquals("", Json.str(o, "missing"));
        assertEquals(-1, Json.num(o, "stdout", -1), "a field of another type gives the default");
    }

    @Test
    void anythingButAnObjectIsEmpty() {
        assertTrue(Json.firstObject("no json at all").isEmpty());
        assertTrue(Json.firstObject("{\"broken\":").isEmpty());
        assertTrue(Json.firstObject(null).isEmpty());
    }

    @Test
    void writesRequestsOnOneLine() {
        String w = Json.write("id", 3, "line", "ls \"x\"\n", "answer", true, "secret", null);
        assertEquals("{\"id\":3,\"line\":\"ls \\\"x\\\"\\n\",\"answer\":true,\"secret\":null}", w);
        Map<String, Object> back = Json.firstObject(w);
        assertEquals("ls \"x\"\n", Json.str(back, "line"));
        assertEquals(List.of(), Json.strings(back, "nothing"));
    }
}
