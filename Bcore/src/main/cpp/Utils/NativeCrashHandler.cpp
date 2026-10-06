#include "NativeCrashHandler.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <ucontext.h>
#include <unwind.h>

#include "Dobby/dobby.h"
#include "xdl.h"

// A guest death that arrives as a Binder death carries no reason. The platform
// exit reason says SIGNALED and hands back the signal number, but not the
// address or the library that faulted, so a native crash is unattributable.
// This handler runs in the crashing process and writes the one thing that makes
// it attributable without root: signal, fault address, program counter, and the
// library/offset the program counter falls in.
//
// Everything here must be async-signal-safe. No allocation, no stdio, no JNI.
// Output is formatted by hand and written with raw syscalls: the guest process
// has the container's filesystem redirect installed, and going through libc
// open(3) would let our own hook reroute a crash record into the sandbox.

namespace {
const int kHandledSignals[] = {SIGBUS, SIGSEGV, SIGILL, SIGABRT, SIGFPE, SIGTRAP};
const int kHandledCount = (int) (sizeof(kHandledSignals) / sizeof(kHandledSignals[0]));

struct sigaction g_previous[kHandledCount];
char g_internalPath[256];
char g_sharedPath[256];
char g_downloadPath[256];
char g_alternateStack[64 * 1024];
volatile sig_atomic_t g_internalReady = 0;
volatile sig_atomic_t g_sharedReady = 0;
volatile sig_atomic_t g_downloadReady = 0;
volatile sig_atomic_t g_reentered = 0;

// Single writer at a time. The re-arm thread and the crashing thread can both
// append, and without this the records interleave: an on-device run produced
// binary garbage spliced between native_crash_displaced lines. A test-and-set
// spin is used rather than a pthread mutex because mutexes are not
// async-signal-safe. It gives up rather than blocking, because losing a record
// to a stuck writer is worse than losing one line to a torn write.
volatile sig_atomic_t g_writeLock = 0;

static bool acquireWriteLock() {
    for (int spin = 0; spin < 4096; spin++) {
        if (__sync_lock_test_and_set(&g_writeLock, 1) == 0) {
            return true;
        }
    }
    return false;
}

static void releaseWriteLock() {
    __sync_lock_release(&g_writeLock);
}

void appendLiteral(char *out, size_t capacity, size_t *length, const char *text) {
    while (*text != '\0' && *length + 1 < capacity) {
        out[(*length)++] = *text++;
    }
}

void appendSigned(char *out, size_t capacity, size_t *length, long value) {
    char digits[24];
    int count = 0;
    unsigned long magnitude = value < 0 ? (unsigned long) (-value) : (unsigned long) value;
    if (value < 0) {
        appendLiteral(out, capacity, length, "-");
    }
    do {
        digits[count++] = (char) ('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0 && count < (int) sizeof(digits));
    while (count > 0 && *length + 1 < capacity) {
        out[(*length)++] = digits[--count];
    }
}

void appendHex(char *out, size_t capacity, size_t *length, uintptr_t value) {
    static const char kHexDigits[] = "0123456789abcdef";
    char digits[16];
    int count = 0;
    do {
        digits[count++] = kHexDigits[value & 0xFu];
        value >>= 4;
    } while (value != 0 && count < (int) sizeof(digits));
    while (count > 0 && *length + 1 < capacity) {
        out[(*length)++] = digits[--count];
    }
}

const char *signalName(int signalNumber) {
    switch (signalNumber) {
        case SIGBUS: return "SIGBUS";
        case SIGSEGV: return "SIGSEGV";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGFPE: return "SIGFPE";
        case SIGTRAP: return "SIGTRAP";
        default: return "SIGNAL";
    }
}

void buildCrashPath(char *out, size_t capacity, const char *directory) {
    size_t length = 0;
    appendLiteral(out, capacity, &length, directory);
    appendLiteral(out, capacity, &length, "/native_crash_");
    appendSigned(out, capacity, &length, (long) getpid());
    appendLiteral(out, capacity, &length, ".log");
    out[capacity - 1] = '\0';
}

// Raw syscalls on purpose -- see the file header. Never touches libc wrappers.
void appendRecordToFile(const char *path, volatile sig_atomic_t *ready,
                        const char *line, size_t length) {
    if (*ready == 0 || path[0] == '\0') {
        return;
    }
    long fd = syscall(__NR_openat, AT_FDCWD, path, O_CREAT | O_WRONLY | O_APPEND, 0600);
    if (fd >= 0) {
        syscall(__NR_write, fd, line, length);
        syscall(__NR_close, fd);
    }
}

// Every record goes to all three sinks under the write lock. The Download
// sink is the one the user can actually open without root: Android 30+ scoped
// storage makes Android/data/<pkg>/ opaque to Termux and to every file
// manager, so a log that only lands there is a log nobody can read.
void writeRecordToAllSinks(const char *line, size_t length) {
    if (!acquireWriteLock()) {
        // Fall through unlocked rather than dropping the record entirely.
        appendRecordToFile(g_internalPath, &g_internalReady, line, length);
        appendRecordToFile(g_sharedPath, &g_sharedReady, line, length);
        appendRecordToFile(g_downloadPath, &g_downloadReady, line, length);
        return;
    }
    appendRecordToFile(g_internalPath, &g_internalReady, line, length);
    appendRecordToFile(g_sharedPath, &g_sharedReady, line, length);
    appendRecordToFile(g_downloadPath, &g_downloadReady, line, length);
    releaseWriteLock();
}

// The guest may have its own crash reporter (Tencent's CrashSight, UE4 itself).
// Pass the fault through so their pipeline still sees it.
void chainToPrevious(int signalNumber, siginfo_t *info, void *context) {
    for (int i = 0; i < kHandledCount; i++) {
        if (kHandledSignals[i] != signalNumber) {
            continue;
        }
        const struct sigaction &previous = g_previous[i];
        if ((previous.sa_flags & SA_SIGINFO) != 0) {
            if (previous.sa_sigaction != nullptr) {
                previous.sa_sigaction(signalNumber, info, context);
            }
        } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
            previous.sa_handler(signalNumber);
        }
        break;
    }
    // If the foreign handler returned instead of ending the process, take the
    // default action: a synchronous fault would otherwise retry forever.
    signal(signalNumber, SIG_DFL);
    raise(signalNumber);
}

// Stack capture, shared by the signal handler and the abort() hook. Defined
// below with the rest of the abort interception; declared here because
// handleSignal() records a backtrace too.
const int kMaxTraceFrames = 32;
uintptr_t g_traceFrames[kMaxTraceFrames];
volatile sig_atomic_t g_traceFrameCount = 0;

int captureTraceFrames();
void appendRawFrameRecord(char *out, size_t capacity, const char *tag, uintptr_t pc);
void appendModuleRecord(char *out, size_t capacity, const char *tag, uintptr_t pc);
void commitRecord(const char *record);
void dumpProcMaps();
int walkInterruptedStack(uintptr_t *out, int maxFrames, uintptr_t framePointer,
                         uintptr_t linkRegister, uintptr_t anchorSp);
static void noteForeignHandler(int index, const struct sigaction *act);
static void installOurHandler(int signum);

void handleSignal(int signalNumber, siginfo_t *info, void *rawContext) {
    if (g_reentered) {
        _exit(128 + signalNumber);
    }
    g_reentered = 1;

    void *faultAddress = info != nullptr ? info->si_addr : nullptr;
    void *programCounter = nullptr;
    uintptr_t framePointer = 0;
    uintptr_t linkRegister = 0;
    uintptr_t stackPointer = 0;
    ucontext_t *context = (ucontext_t *) rawContext;
    if (context != nullptr) {
#if defined(__aarch64__)
        programCounter = (void *) context->uc_mcontext.pc;
        framePointer = (uintptr_t) context->uc_mcontext.regs[29];
        linkRegister = (uintptr_t) context->uc_mcontext.regs[30];
        stackPointer = (uintptr_t) context->uc_mcontext.sp;
#elif defined(__arm__)
        programCounter = (void *) context->uc_mcontext.arm_pc;
        framePointer = (uintptr_t) context->uc_mcontext.arm_fp;
        linkRegister = (uintptr_t) context->uc_mcontext.arm_lr;
        stackPointer = (uintptr_t) context->uc_mcontext.arm_sp;
#endif
    }

    char line[512];
    size_t length = 0;
    appendLiteral(line, sizeof(line), &length, "native_crash signal=");
    appendLiteral(line, sizeof(line), &length, signalName(signalNumber));
    appendLiteral(line, sizeof(line), &length, "(");
    appendSigned(line, sizeof(line), &length, signalNumber);
    appendLiteral(line, sizeof(line), &length, ") tid=");
    appendSigned(line, sizeof(line), &length, (long) gettid());
    appendLiteral(line, sizeof(line), &length, " fault_addr=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) faultAddress);
    appendLiteral(line, sizeof(line), &length, " pc=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) programCounter);
    appendLiteral(line, sizeof(line), &length, "\n");
    line[length < sizeof(line) ? length : sizeof(line) - 1] = '\0';

// Commit the async-signal-safe essentials before consulting the dynamic
    // linker. dladdr() is not async-signal-safe; if it faults while the loader
    // lock is held, a re-entry would otherwise _exit before writing any report.
    writeRecordToAllSinks(line, length);

    // Raw program counters first, still without the loader lock. On SIGBUS and
    // SIGSEGV the fault address in si_addr is the single most useful field and
    // it is already in the line above; the stack is what turns it into a file
    // and an offset.
    int frameCount = walkInterruptedStack(g_traceFrames, kMaxTraceFrames,
                                          framePointer, linkRegister, stackPointer);
    for (int i = 0; i < frameCount; i++) {
        char frameLine[256];
        appendRawFrameRecord(frameLine, sizeof(frameLine), "native_crash_frame", g_traceFrames[i]);
        commitRecord(frameLine);
    }

    if (programCounter != nullptr) {
        Dl_info symbol;
        memset(&symbol, 0, sizeof(symbol));
        {
            // Written unconditionally: on pid 17877 dladdr() failed for the
            // faulting pc (0x681f0dc8, an unmapped/JIT page) and the record was
            // skipped, so the one address that mattered went unrecorded.
            char pcRecord[512];
            appendModuleRecord(pcRecord, sizeof(pcRecord), "native_crash_pc", (uintptr_t) programCounter);
            commitRecord(pcRecord);
        }
        if (dladdr(programCounter, &symbol) != 0 && symbol.dli_fname != nullptr) {
            char moduleLine[512];
            size_t moduleLength = 0;
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength,
                          "native_crash_module signal=");
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength,
                          signalName(signalNumber));
            appendLiteral(moduleLine, sizeof(moduleLine), &moduleLength, " pc=0x");
            appendHex(moduleLine, sizeof(moduleLength), &moduleLength,
                      (uintptr_t) programCounter);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, " lib=");
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, symbol.dli_fname);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, "+0x");
            appendHex(moduleLine, sizeof(moduleLength), &moduleLength,
                      (uintptr_t) programCounter - (uintptr_t) symbol.dli_fbase);
            appendLiteral(moduleLine, sizeof(moduleLength), &moduleLength, "\n");
            moduleLine[moduleLength < sizeof(moduleLength) ? moduleLength : sizeof(moduleLength) - 1] = '\0';
            writeRecordToAllSinks(moduleLine, moduleLength);
            __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", moduleLine);
        }
    }

    for (int i = 0; i < frameCount; i++) {
        char symbolLine[512];
        appendModuleRecord(symbolLine, sizeof(symbolLine), "native_crash_symbol", g_traceFrames[i]);
        commitRecord(symbolLine);
    }

    dumpProcMaps();

    // The durable record is already written. Logcat is a best-effort extra
    // channel because Android logging is not async-signal-safe either.
    __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", line);

    chainToPrevious(signalNumber, info, rawContext);
}

// Records that the write path works, before any crash has happened. If the file
// exists but never grows a crash line, the handler is not being invoked -- which
// points at a crash reporter installed after us, not at a broken write.
void writeArmProbe() {
    char line[384];
    size_t length = 0;
    appendLiteral(line, sizeof(line), &length, "native_handler_armed pid=");
    appendSigned(line, sizeof(line), &length, (long) getpid());
    appendLiteral(line, sizeof(line), &length, " internal=");
    appendLiteral(line, sizeof(line), &length, g_internalPath);
    appendLiteral(line, sizeof(line), &length, " shared=");
    appendLiteral(line, sizeof(line), &length, g_sharedPath);
    appendLiteral(line, sizeof(line), &length, " download=");
    appendLiteral(line, sizeof(line), &length, g_downloadPath[0] != '\0' ? g_downloadPath : "<none>");
    appendLiteral(line, sizeof(line), &length, "\n");
    writeRecordToAllSinks(line, length);
}

// Resolving one libc symbol through xdl, the same pattern VirtualSpoof uses.
// Kept as a helper because the abort hook below needs it too.
static void *resolve_libc(const char *symbol) {
    void *handle = xdl_open("libc.so", XDL_DEFAULT);
    if (!handle) {
        return nullptr;
    }
    void *target = xdl_dsym(handle, symbol, nullptr);
    xdl_close(handle);
    return target;
}

void armHandlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handleSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < kHandledCount; i++) {
        struct sigaction existing;
        memset(&existing, 0, sizeof(existing));
        if (sigaction(kHandledSignals[i], &action, &existing) != 0) {
            continue;
        }
        // Remember the newest foreign handler so we can chain to it. Our own
        // handler must never become the "previous" -- that would recurse forever.
        if (existing.sa_sigaction != handleSignal) {
            g_previous[i] = existing;
        }
    }
}

// Re-arm only when the disposition is no longer ours.
//
// The previous version re-armed unconditionally every 500ms. That is a fight
// neither side wins: the engine installs its reporter, our timer overwrites it,
// the engine reinstalls, and whichever call lands last owns SIGABRT when the
// process dies. Measured on com.proxima.dfm: the arm probe wrote at t+0.1s, the
// guest died by abort() at t+51.5s, and the crash file contained the arm line
// and nothing else -- our handler never ran, because the engine's reporter
// owned the disposition at that moment.
//
// Comparing before writing costs six sigaction calls only when something
// actually changed, and lets the engine's handler stand until it needs to be
// replaced. SIGABRT is additionally caught at the abort() call site below, so a
// self-abort is recorded regardless of who owns the signal disposition.
static bool ourHandlerIsInstalled(int signalNumber) {
    struct sigaction current;
    memset(&current, 0, sizeof(current));
    if (sigaction(signalNumber, nullptr, &current) != 0) {
        return false;
    }
    return current.sa_sigaction == handleSignal;
}

// Backstop only. The sigaction hook below keeps us outermost for anything that
// goes through the public entry point; this catches installers that reach past
// it (__sigaction, or a raw rt_sigaction), and re-installs without re-logging an
// owner we have already recorded.
static void ensureHandlersArmed() {
    for (int i = 0; i < kHandledCount; i++) {
        if (ourHandlerIsInstalled(kHandledSignals[i])) {
            continue;
        }
        struct sigaction current;
        memset(&current, 0, sizeof(current));
        if (sigaction(kHandledSignals[i], nullptr, &current) == 0) {
            noteForeignHandler(i, &current);
        }
        installOurHandler(kHandledSignals[i]);
    }
}

// --- abort() interception ---------------------------------------------------
//
// A game that decides to kill itself does not usually raise a signal by hand; it
// calls abort(). That lands on the disposition currently installed for SIGABRT,
// which by then belongs to the engine's reporter, not to us. Hooking abort()
// makes the record unconditional: it is the last point before the process dies
// and no other library can take it from us.
//
// Measured on com.proxima.dfm at t+45.6s: the guest's own crash reporter caught
// SIGBUS(7), and its own termination path called abort(). The kernel still
// reported the original fatal signal, so the exit reason stays SIGNALED/7 while
// the abort hook is the only place the fault is visible. That is why this hook
// unwinds a real stack instead of logging one return address -- the frame above
// abort() is inside libc (fault_addr came back 0x0 and no module line was
// emitted), so a single pc names libc and nothing else.
//
// Everything below runs with the same async-signal-safe constraints as the
// signal handler: no allocation, no stdio, raw syscalls only. The raw program
// counters are committed to disk before dladdr() is consulted, because dladdr
// takes the loader lock and can fault if that lock is already held.

static void (*orig_abort)(void) = nullptr;

_Unwind_Reason_Code collectTraceFrame(struct _Unwind_Context *context, void *argument) {
    (void) argument;
    if (g_traceFrameCount >= kMaxTraceFrames) {
        return _URC_END_OF_STACK;
    }
    uintptr_t pc = (uintptr_t) _Unwind_GetIP(context);
    if (pc == 0) {
        return _URC_END_OF_STACK;
    }
    g_traceFrames[g_traceFrameCount++] = pc;
    return _URC_NO_REASON;
}

int captureTraceFrames() {
    g_traceFrameCount = 0;
    _Unwind_Backtrace(collectTraceFrame, nullptr);
    return (int) g_traceFrameCount;
}

// --- stack walking across a signal boundary ----------------------------------
//
// _Unwind_Backtrace is the wrong tool inside a signal handler here, for two
// reasons, both observed on com.proxima.dfm pid 17877:
//
//  1. It cannot cross the signal frame. That run returned exactly two frames --
//     libblackbox.so+0x2778c (the handler trampoline) and then [vdso] with
//     sym=__kernel_rt_sigreturn -- and stopped. The faulting context was never
//     reached, so the backtrace named our own code and told us nothing.
//
//  2. Running it there is not safe. It faults while reading a stack that is
//     already broken, which re-enters the handler and hits the g_reentered guard
//     -> _exit(128 + 11). Android then reported reason=EXIT_SELF status=139 and
//     the guest crash-looped immediately instead of dying once at ~48s.
//
// The interrupted context is the one place where the guest's real stack is
// available, so read it directly: seed from the ucontext frame/link registers,
// then walk the frame chain by hand. Every pointer is range-checked before it
// is dereferenced, so a corrupt stack ends the walk instead of faulting.
const uintptr_t kUserAddressLimit = 0x0000FFFFFFFFFFFFULL; // 48-bit user VA

static bool plausibleCodeAddress(uintptr_t pc) {
    return pc != 0 && pc < kUserAddressLimit && (pc & 0x3) == 0;
}

int walkInterruptedStack(uintptr_t *out, int maxFrames, uintptr_t framePointer,
                         uintptr_t linkRegister, uintptr_t anchorSp) {
    (void) framePointer;
    (void) anchorSp;
    // Only the link register. An earlier version also followed the frame-pointer
    // chain, and returned one frame per run: libart.so+0x270fc4 on one run, an
    // unresolved address on the next, then a stop either way.
    //
    // One frame is the honest ceiling, because Android's arm64 code is built
    // without frame pointers. x29 is an ordinary callee-saved register holding
    // whatever the faulting code last stored there, so it is not a frame chain
    // at all. Range-checking that value against a plausible stack range passes
    // often enough to be dangerous, and dereferencing it inside a signal handler
    // is precisely how a guest fault turns into EXIT_SELF status=139 instead of
    // a recorded crash. Take the one frame that needs no dereference and leave
    // attribution to native_maps_<pid>.log.
    int count = 0;
    if (plausibleCodeAddress(linkRegister) && count < maxFrames) {
        out[count++] = linkRegister;
    }
    return count;
}

static bool addressIsInLibc(uintptr_t pc) {
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr((void *) pc, &info) == 0 || info.dli_fname == nullptr) {
        return false;
    }
    return strstr(info.dli_fname, "/libc.so") != nullptr;
}

// First frame that is not libc. The frame directly above abort() belongs to
// libc's own terminate path (__libc_fatal, __fortify_fail, __stack_chk_fail),
// so naming it would only ever produce "libc.so". Returns the caller pc itself
// when the whole visible stack is libc, so the record still carries something.
static void *firstNonLibcFrame(const uintptr_t *frames, int count) {
    for (int i = 0; i < count; i++) {
        if (!addressIsInLibc(frames[i])) {
            return (void *) frames[i];
        }
    }
    if (count > 0) {
        return (void *) frames[0];
    }
    return nullptr;
}

void appendModuleRecord(char *out, size_t capacity, const char *tag,
                               uintptr_t pc) {
    size_t length = 0;
    appendLiteral(out, capacity, &length, tag);
    appendLiteral(out, capacity, &length, " pc=0x");
    appendHex(out, capacity, &length, pc);
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr((void *) pc, &info) != 0 && info.dli_fname != nullptr) {
        appendLiteral(out, capacity, &length, " lib=");
        appendLiteral(out, capacity, &length, info.dli_fname);
        appendLiteral(out, capacity, &length, "+0x");
        appendHex(out, capacity, &length, pc - (uintptr_t) info.dli_fbase);
        const char *symbol = info.dli_sname != nullptr ? info.dli_sname : "?";
        appendLiteral(out, capacity, &length, " sym=");
        appendLiteral(out, capacity, &length, symbol);
    } else {
        // dladdr failing is itself a finding: it means the address is outside
        // every loaded object, which is what an unmapped or JIT page looks like.
        appendLiteral(out, capacity, &length, " lib=<unresolved> pc_is_unmapped=1");
    }
    appendLiteral(out, capacity, &length, "\n");
    // Terminate at the append length, not at capacity-1. commitRecord() measures
    // with strlen(), and terminating at the far end made every record carry the
    // previous call's leftover stack bytes -- an on-device run produced repeated
    // signal= lines and a truncated tail that way.
    out[length < capacity ? length : capacity - 1] = '\0';
}

void appendRawFrameRecord(char *out, size_t capacity, const char *tag,
                                 uintptr_t pc) {
    size_t length = 0;
    appendLiteral(out, capacity, &length, tag);
    appendLiteral(out, capacity, &length, " pc=0x");
    appendHex(out, capacity, &length, pc);
    appendLiteral(out, capacity, &length, "\n");
    out[length < capacity ? length : capacity - 1] = '\0';
}

void commitRecord(const char *record) {
    size_t length = strlen(record);
    writeRecordToAllSinks(record, length);
    __android_log_write(ANDROID_LOG_ERROR, "SLACKBOX_NATIVE_CRASH", record);
}

// Snapshot of the address space at the moment of death. Without it, frames that
// dladdr cannot resolve cannot be attributed after the process is gone.
void dumpProcMaps() {
    long fd = syscall(__NR_openat, AT_FDCWD, "/proc/self/maps", O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    char buffer[4096];
    const char *sinks[3];
    sinks[0] = g_internalReady ? g_internalPath : nullptr;
    sinks[1] = g_sharedReady ? g_sharedPath : nullptr;
    sinks[2] = g_downloadReady ? g_downloadPath : nullptr;

    for (int s = 0; s < 3; s++) {
        if (sinks[s] == nullptr) {
            continue;
        }
        char mapPath[256];
        size_t pathLength = 0;
        appendLiteral(mapPath, sizeof(mapPath), &pathLength, sinks[s]);
        // native_crash_1234.log -> native_maps_1234.log
        const char *lastSlash = strrchr(mapPath, '/');
        pathLength = lastSlash != nullptr ? (size_t) (lastSlash - mapPath) + 1 : 0;
        appendLiteral(mapPath, sizeof(mapPath), &pathLength, "native_maps_");
        appendSigned(mapPath, sizeof(mapPath), &pathLength, (long) getpid());
        appendLiteral(mapPath, sizeof(mapPath), &pathLength, ".log\n");

        long out = syscall(__NR_openat, AT_FDCWD, mapPath,
                           O_CREAT | O_WRONLY | O_TRUNC, 0600);
        if (out < 0) {
            continue;
        }
        // Rewind: /proc/self/maps is a seq_file, one read pass only.
        syscall(__NR_lseek, fd, 0, SEEK_SET, nullptr);
        for (;;) {
            long got = syscall(__NR_read, fd, buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            syscall(__NR_write, out, buffer, (size_t) got);
        }
        syscall(__NR_close, out);
    }
    syscall(__NR_close, fd);
}

static void my_abort() {
    int frameCount = captureTraceFrames();
    uintptr_t callerPc = (uintptr_t) __builtin_return_address(0);
    void *reported = frameCount > 0 ? firstNonLibcFrame(g_traceFrames, frameCount)
                                    : (void *) callerPc;

    // 1. Essentials, async-signal-safe, no loader lock.
    char line[512];
    size_t length = 0;
    appendLiteral(line, sizeof(line), &length, "native_crash signal=SIGABRT(6) tid=");
    appendSigned(line, sizeof(line), &length, (long) gettid());
    appendLiteral(line, sizeof(line), &length, " source=abort()");
    appendLiteral(line, sizeof(line), &length, " frames=");
    appendSigned(line, sizeof(line), &length, frameCount);
    appendLiteral(line, sizeof(line), &length, " fault_addr=0x");
    appendHex(line, sizeof(line), &length, (uintptr_t) reported);
    appendLiteral(line, sizeof(line), &length, " pc=0x");
    appendHex(line, sizeof(line), &length, callerPc);
    appendLiteral(line, sizeof(line), &length, "\n");
    line[length < sizeof(line) ? length : sizeof(line) - 1] = '\0';
    commitRecord(line);

    // 2. Raw program counters, still no loader lock. These survive even if the
    //    resolution pass below faults.
    for (int i = 0; i < frameCount; i++) {
        char frameLine[256];
        appendRawFrameRecord(frameLine, sizeof(frameLine), "native_crash_frame", g_traceFrames[i]);
        commitRecord(frameLine);
    }

    // 3. Resolution pass. Loader lock from here on.
    if (reported != nullptr) {
        char moduleLine[512];
        appendModuleRecord(moduleLine, sizeof(moduleLine),
                           "native_crash_module signal=SIGABRT(6)", (uintptr_t) reported);
        commitRecord(moduleLine);
    }
    for (int i = 0; i < frameCount; i++) {
        char symbolLine[512];
        appendModuleRecord(symbolLine, sizeof(symbolLine), "native_crash_symbol", g_traceFrames[i]);
        commitRecord(symbolLine);
    }

    dumpProcMaps();

    if (orig_abort != nullptr) {
        orig_abort();
    }
    // abort() is not allowed to return, but if our replacement did, leave no
    // way for the caller to continue past a deliberate self-termination.
    _exit(134);
}

static void installAbortHook() {
    void *target = resolve_libc("abort");
    if (target == nullptr) {
        return;
    }
    if (DobbyHook(target, (void *) my_abort, (void **) &orig_abort) != 0) {
        return;
    }
}

// --- sigaction() interception ------------------------------------------------
//
// Measured on com.proxima.dfm (pid 27316): the guest's own crash reporters
// displaced our fatal-signal handlers twelve times in a single run, alternating
// between libCrashSight.so+0x1c2e8 (Tencent) and libcrashlytics-common.so
// +0x9d7c0 (Google). Our one-second re-arm took the disposition back, they took
// it again, and the crash landed in whichever window they happened to hold --
// so the crash file held the arm line and nothing else. A timer cannot win that.
//
// Rather than fight for the disposition, wrap it. The installer still gets what
// it asked for and still believes it owns the signal; we remember their handler
// and put ours back on top. handleSignal() writes the record and then chains to
// them, so their reporter behaves exactly as before. Nothing about the guest
// changes -- we only observe before it runs.

static int (*orig_sigaction)(int, const struct sigaction *, struct sigaction *) = nullptr;

static int indexOfSignal(int signum) {
    for (int i = 0; i < kHandledCount; i++) {
        if (kHandledSignals[i] == signum) {
            return i;
        }
    }
    return -1;
}

// Deduplicates: both the sigaction hook and the re-arm backstop funnel through
// here, and without this the file filled with repeats of the same owner.
static void noteForeignHandler(int index, const struct sigaction *act) {
    if (index < 0 || act == nullptr) {
        return;
    }
    if (g_previous[index].sa_sigaction == act->sa_sigaction) {
        return;
    }
    g_previous[index] = *act;

    char line[512];
    appendModuleRecord(line, sizeof(line), "native_crash_displaced",
                       (uintptr_t) act->sa_sigaction);
    size_t length = strlen(line);
    while (length > 0 && line[length - 1] != '\n') {
        line[--length] = '\0';
    }
    size_t used = length;
    appendLiteral(line, sizeof(line), &used, " signal=");
    appendLiteral(line, sizeof(line), &used, signalName(kHandledSignals[index]));
    appendLiteral(line, sizeof(line), &used, " disposition=wrapped_owned_by_us=1\n");
    line[used < sizeof(line) ? used : sizeof(line) - 1] = '\0';
    commitRecord(line);
}

static void installOurHandler(int signum) {
    if (orig_sigaction == nullptr) {
        return;
    }
    struct sigaction ours;
    memset(&ours, 0, sizeof(ours));
    ours.sa_sigaction = handleSignal;
    ours.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&ours.sa_mask);
    orig_sigaction(signum, &ours, nullptr);
}

static int my_sigaction(int signum, const struct sigaction *act,
                        struct sigaction *oldact) {
    if (orig_sigaction == nullptr) {
        if (oldact != nullptr) {
            memset(oldact, 0, sizeof(*oldact));
        }
        return -1;
    }
    // Forward first, so *oldact holds what the caller would have seen.
    int result = orig_sigaction(signum, act, oldact);
    if (result != 0 || act == nullptr) {
        return result;
    }
    int index = indexOfSignal(signum);
    if (index < 0) {
        return result;
    }
    if ((act->sa_flags & SA_SIGINFO) == 0) {
        return result; // not a SA_SIGINFO handler; nothing to wrap
    }
    if (act->sa_sigaction == handleSignal) {
        return result; // our own re-install
    }
    noteForeignHandler(index, act);
    installOurHandler(signum);
    return result;
}

static void installSigactionHook() {
    void *target = resolve_libc("sigaction");
    if (target == nullptr) {
        return;
    }
    DobbyHook(target, (void *) my_sigaction, (void **) &orig_sigaction);
}

// A game engine and its crash reporter install their own fatal-signal handlers
// well after the guest binds, silently replacing ours. This thread notices and
// re-arms, but only for the signals whose disposition actually drifted.
void *rearmLoop(void *) {
    for (;;) {
        struct timespec pause;
        pause.tv_sec = 1;
        pause.tv_nsec = 0;
        nanosleep(&pause, nullptr);
        ensureHandlersArmed();
    }
    return nullptr;
}
} // namespace

void NativeCrashHandler::install(const char *internalDirectory, const char *sharedDirectory,
                                 const char *downloadDirectory) {
    if (internalDirectory != nullptr) {
        buildCrashPath(g_internalPath, sizeof(g_internalPath), internalDirectory);
        g_internalReady = 1;
    }
    if (sharedDirectory != nullptr) {
        buildCrashPath(g_sharedPath, sizeof(g_sharedPath), sharedDirectory);
        g_sharedReady = 1;
    }
    if (downloadDirectory != nullptr) {
        buildCrashPath(g_downloadPath, sizeof(g_downloadPath), downloadDirectory);
        g_downloadReady = 1;
    }

    stack_t alternate; // run the handler off a dedicated stack (stack-overflow SIGSEGV)
    memset(&alternate, 0, sizeof(alternate));
    alternate.ss_sp = g_alternateStack;
    alternate.ss_size = sizeof(g_alternateStack);
    alternate.ss_flags = 0;
    sigaltstack(&alternate, nullptr);

    writeArmProbe();
    armHandlers();
    // Order matters: sigaction is wrapped before abort so a reporter that
    // installs handlers during its own init is already captured.
    installSigactionHook();
    installAbortHook();

    pthread_t watchdog;
    if (pthread_create(&watchdog, nullptr, rearmLoop, nullptr) == 0) {
        pthread_detach(watchdog);
    }
}
