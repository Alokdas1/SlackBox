package top.niunaijun.blackbox.utils;

import org.junit.Test;

import java.util.concurrent.atomic.AtomicBoolean;

import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

public class CrashMonitorTest {
    @Test
    public void reportsThenForwardsUncaughtGuestException() {
        Thread crashedThread = Thread.currentThread();
        RuntimeException failure = new RuntimeException("guest activity failed");
        AtomicBoolean reported = new AtomicBoolean();
        AtomicBoolean forwarded = new AtomicBoolean();
        Thread.UncaughtExceptionHandler platformHandler = (thread, error) -> {
            assertSame(crashedThread, thread);
            assertSame(failure, error);
            forwarded.set(true);
        };

        CrashMonitor.reportAndForward(platformHandler, crashedThread, failure,
                (thread, error) -> reported.set(thread == crashedThread && error == failure));

        assertTrue(reported.get());
        assertTrue(forwarded.get());
    }

    @Test
    public void forwardsEvenWhenDiagnosticReporterFails() {
        AtomicBoolean forwarded = new AtomicBoolean();
        Thread.UncaughtExceptionHandler platformHandler = (thread, error) -> forwarded.set(true);

        try {
            CrashMonitor.reportAndForward(platformHandler, Thread.currentThread(),
                    new RuntimeException("guest crash"), (thread, error) -> {
                        throw new IllegalStateException("diagnostic write failed");
                    });
        } catch (IllegalStateException expected) {
            assertTrue(forwarded.get());
            return;
        }
        throw new AssertionError("Reporter failure must propagate after forwarding");
    }

    @Test
    public void guestEventLineEscapesFieldsAndIncludesInstanceContext() {
        String line = CrashMonitor.formatEventLine("java_crash", "game.package",
                "game.package:render", 7, "game.package.MainActivity", "bad \"surface\"\nstate");

        assertTrue(line.startsWith("{\"time_ms\":"));
        assertTrue(line.contains("\"event\":\"java_crash\""));
        assertTrue(line.contains("\"package\":\"game.package\""));
        assertTrue(line.contains("\"process\":\"game.package:render\""));
        assertTrue(line.contains("\"instance\":7"));
        assertTrue(line.contains("bad \\\"surface\\\"\\nstate"));
        assertTrue(line.endsWith("}\n"));
    }
}
