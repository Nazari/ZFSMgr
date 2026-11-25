package org.zfsmgr.gui;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

class StateStoreTest {
    @Test
    void readsTheStateOfAJobAnswer() {
        String out = "{\"job\":[{\"field\":\"bytes\",\"value\":\"16830000\"},"
                + "{\"field\":\"error\",\"value\":null},{\"field\":\"state\",\"value\":\"done\"}]}";
        assertEquals("done", StateStore.jobState(out));
        assertEquals("running", StateStore.jobState("{\"job\":[{\"field\":\"state\",\"value\":\"running\"}]}"));
    }

    @Test
    void anAnswerWithoutAStateGivesNothing() {
        assertEquals("", StateStore.jobState(""));
        assertEquals("", StateStore.jobState("error: no such job"));
        assertEquals("", StateStore.jobState("{\"job\":[{\"field\":\"bytes\",\"value\":\"1\"}]}"));
    }
}
