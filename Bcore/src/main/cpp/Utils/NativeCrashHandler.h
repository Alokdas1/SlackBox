#ifndef SLACKBOX_NATIVE_CRASH_HANDLER_H
#define SLACKBOX_NATIVE_CRASH_HANDLER_H

namespace NativeCrashHandler {
    // Installs async-signal-safe handlers for the fatal signals a guest process
    // can die from, and wraps sigaction() so the handler stays outermost even
    // when the guest's own crash reporters install theirs.
    //
    // Each record is appended to all three of these, so the same crash is
    // available wherever the reader has access:
    //   <internalDirectory>/native_crash_<pid>.log   host app private data
    //   <sharedDirectory>/native_crash_<pid>.log     getExternalFilesDir
    //   <downloadDirectory>/native_crash_<pid>.log   Download/logs
    //
    // The Download sink is the one that matters in practice. Android 30+ scoped
    // storage makes Android/data/<pkg>/ opaque to Termux and to every file
    // manager, so a log that only lands under getExternalFilesDir is a log
    // nobody can open without root. It requires MANAGE_EXTERNAL_STORAGE, and
    // the arm probe reports the resolved path so a silent failure is visible.
    //
    // Writes bypass the libc wrappers with raw syscalls so the container's own
    // path redirect cannot swallow the record. Any directory may be null.
    void install(const char *internalDirectory, const char *sharedDirectory,
                 const char *downloadDirectory);
}

#endif
