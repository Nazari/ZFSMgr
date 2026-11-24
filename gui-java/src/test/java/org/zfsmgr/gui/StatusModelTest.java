package org.zfsmgr.gui;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

/** The same cases as tests/gui_status_test.cpp, for the status part. */
class StatusModelTest {
    @BeforeAll
    static void english() {
        Text.setLang("en");
    }

    static final String LS = "{\"connections\":[{\"id\":\"local\",\"name\":\"Local\",\"connected\":true},"
            + "{\"id\":\"unib\",\"name\":\"unib\",\"connected\":true,\"uid\":\"u1\"},"
            + "{\"id\":\"old\",\"name\":\"old\",\"connected\":false}]}";

    static String overview(String conn, String poolStatus, String warnings) {
        return "{\"machines\":[{\"connection\":\"" + conn + "\",\"reachable\":true,"
                + "\"pools\":[{\"name\":\"p\",\"status\":\"" + poolStatus + "\",\"capacity\":70,\"size\":\"2T\"}],"
                + "\"schedules\":[{\"dataset\":\"p/a\",\"kind\":\"pull\",\"state\":\"ok\",\"keep\":{\"daily\":7,\"yearly\":1}},"
                + "{\"dataset\":\"p/b\",\"kind\":\"local\",\"state\":\"ok\"}],"
                + "\"jobs\":[{\"state\":\"running\"},{\"state\":\"queued\"}]}],"
                + "\"warnings\":" + warnings + "}";
    }

    @Test
    void summaryCountsWhatAnswers() {
        List<StatusModel.MachineView> ms = StatusModel.machinesFromLs(LS);
        assertEquals(3, ms.size());
        assertEquals("u1", ms.get(1).uid);
        assertFalse(ms.get(2).connected);
        StatusModel.applyOverview(ms.get(0), overview("local", "ok", "[]"));
        StatusModel.applyOverview(ms.get(1), overview("unib", "warning", "[]"));
        StatusModel.Summary s = StatusModel.summarize(ms);
        assertEquals(2, s.machines(), "the disconnected one does not count");
        assertEquals(2, s.answering());
        assertEquals(2, s.pools());
        assertEquals(1, s.poolsWithProblems());
        assertEquals(2, s.copies(), "a local schedule is not a copy");
        assertEquals(2, s.jobsRunning());
    }

    @Test
    void noJsonIsAFailureNotAFineMachine() {
        StatusModel.MachineView m = new StatusModel.MachineView();
        StatusModel.applyOverview(m, "not json");
        assertTrue(m.loaded);
        assertFalse(m.reachable());
        assertFalse(m.failure.isEmpty());
    }

    @Test
    void attentionErrorsFirstAndOneSessionOnce() {
        List<StatusModel.MachineView> ms = StatusModel.machinesFromLs(LS);
        String session = "{\"level\":\"warning\",\"subject\":\"session\",\"session\":\"abc\",\"host\":\"fc16b\",\"text\":\"open elsewhere\"}";
        StatusModel.applyOverview(ms.get(0), overview("local", "ok", "[" + session + "]"));
        StatusModel.applyOverview(ms.get(1), overview("unib", "ok", "[" + session
                + ",{\"level\":\"error\",\"subject\":\"pool p\",\"text\":\"broken\"}]"));
        List<StatusModel.Attention> a = StatusModel.attention(ms);
        assertEquals(2, a.size(), "the same session from two machines is shown once");
        assertTrue(a.get(0).error());
        assertEquals("broken", a.get(0).text());
        assertEquals("fc16b", a.get(1).sessionHost());
    }

    @Test
    void textsOfTimeAndKeep() {
        assertEquals("never", StatusModel.ago(0, 100));
        assertEquals("2 h ago", StatusModel.ago(1000, 1000 + 2 * 3600));
        assertEquals("7 daily, 1 yearly", StatusModel.keepText(Map.of("daily", 7L, "yearly", 1L)));
        assertEquals("—", StatusModel.keepText(Map.of()));
        Text.setLang("es");
        assertEquals("hace 3 min", StatusModel.ago(1000, 1180));
        assertEquals("1 diaria", StatusModel.keepText(Map.of("daily", 1L)));
        Text.setLang("en");
    }
}
