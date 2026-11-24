package org.zfsmgr.gui;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

/** The copy plan cases of tests/gui_status_test.cpp. */
class CopyPlanTest {
    @BeforeAll
    static void english() {
        Text.setLang("en");
    }

    private static CopyPlan.Request pull() {
        CopyPlan.Request r = new CopyPlan.Request();
        r.originMachine = "unib";
        r.originDataset = "ppal/user/srvs";
        r.destMachine = "local";
        r.destDataset = "back/user/srvs";
        r.mode = CopyPlan.Mode.PULL;
        r.originHasSchedule = true;
        r.destRoleForOrigin = "full";
        return r;
    }

    @Test
    void pullBetweenMachinesThatKnowEachOther() {
        assertEquals(List.of("create user/srvs -p --on zfsm://local/back",
                "schedule --origin zfsm://unib/ppal/user/srvs --daily 7 --weekly 4 --monthly 6 --yearly 1 --on zfsm://local/back/user/srvs",
                "schedule run --on zfsm://local/back/user/srvs"), CopyPlan.commands(pull()));
    }

    @Test
    void pullWithPullOnlyCredentialsGetsTheFullOnes() {
        CopyPlan.Request r = pull();
        r.destRoleForOrigin = "pull";
        List<String> c = CopyPlan.commands(r);
        assertEquals(4, c.size());
        assertEquals("peers --push --role full --on zfsm://local", c.get(0));
    }

    @Test
    void pullToAMachineThatDoesNotKnowTheOrigin() {
        CopyPlan.Request r = pull();
        r.destRoleForOrigin = "";
        r.originHasSchedule = false;
        List<String> c = CopyPlan.commands(r);
        assertEquals(7, c.size());
        assertEquals("peers --push --role full --on zfsm://local", c.get(0));
        assertEquals("peers --listen 0.0.0.0 --on zfsm://unib", c.get(1));
        assertTrue(c.get(2).startsWith("schedule --daily 1 --weekly 1"));
        assertEquals("schedule run --on zfsm://unib/ppal/user/srvs", c.get(3));
    }

    @Test
    void push() {
        CopyPlan.Request r = pull();
        r.mode = CopyPlan.Mode.PUSH;
        r.originRoleForDest = "pull";
        List<String> c = CopyPlan.commands(r);
        assertEquals(3, c.size());
        assertEquals("peers --push --role full --on zfsm://unib", c.get(0));
        r.originRoleForDest = "full";
        assertEquals(List.of("schedule --daily 7 --weekly 4 --monthly 6 --yearly 1 --to zfsm://local/back/user/srvs --level --on zfsm://unib/ppal/user/srvs",
                "schedule run --on zfsm://unib/ppal/user/srvs"), CopyPlan.commands(r));
    }

    @Test
    void once() {
        CopyPlan.Request r = pull();
        r.mode = CopyPlan.Mode.ONCE;
        r.stamp = "20261122-1905";
        assertEquals(List.of("create @copia-20261122-1905 --on zfsm://unib/ppal/user/srvs",
                "send zfsm://local/back/user/srvs --from zfsm://unib/ppal/user/srvs@copia-20261122-1905"),
                CopyPlan.commands(r));
    }

    @Test
    void problems() {
        CopyPlan.Request bad = pull();
        bad.destDataset = "back";
        assertTrue(CopyPlan.commands(bad).isEmpty());
        assertFalse(CopyPlan.problem(bad).isEmpty(), "a copy needs pool/name");
        bad = pull();
        bad.destMachine = "unib";
        bad.destDataset = "ppal/user/srvs/dentro";
        assertFalse(CopyPlan.problem(bad).isEmpty(), "not inside the original");
        bad = pull();
        bad.destDataset = "back/con espacio";
        assertFalse(CopyPlan.problem(bad).isEmpty(), "no spaces in the name");
        bad = pull();
        bad.keep.daily = 0;
        bad.keep.weekly = 0;
        bad.keep.monthly = 0;
        bad.keep.yearly = 0;
        assertFalse(CopyPlan.problem(bad).isEmpty(), "it has to keep something");
    }
}
