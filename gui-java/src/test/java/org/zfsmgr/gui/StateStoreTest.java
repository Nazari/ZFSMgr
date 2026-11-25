package org.zfsmgr.gui;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

class StateStoreTest {
    @Test
    void readsTheStateOfAJobAnswer() {
        String out = "{\"job\":[{\"field\":\"bytes\",\"value\":\"16830000\"},"
                + "{\"field\":\"error\",\"value\":null},{\"field\":\"state\",\"value\":\"done\"}]}";
        assertEquals("done", StateStore.jobField(out, "state"));
        assertEquals("", StateStore.jobField(out, "error"));
        assertEquals("running", StateStore.jobField("{\"job\":[{\"field\":\"state\",\"value\":\"running\"}]}", "state"));
    }

    @Test
    void anAnswerWithoutAStateGivesNothing() {
        assertEquals("", StateStore.jobField("", "state"));
        assertEquals("", StateStore.jobField("error: no such job", "state"));
        assertEquals("", StateStore.jobField("{\"job\":[{\"field\":\"bytes\",\"value\":\"1\"}]}", "state"));
    }
}
