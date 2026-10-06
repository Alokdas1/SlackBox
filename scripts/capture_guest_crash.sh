#!/bin/bash
# Capture a guest crash from Termux (no root required) or over adb.
#
# Why this exists: a guest process that dies surfaces in SlackBox as
#   guest_process_binder_died
# because a Binder death cannot distinguish a crash from a normal exit. The
# reason lives outside the container, in logcat: the native crash/tombstone
# buffer and the system's "Fatal signal" / ANR records. There is no way to
# reconstruct it after the fact -- the ring buffer rolls over -- so the buffers
# have to be cleared, streamed while you reproduce the death, then filtered.
#
# TIMED, NOT INTERACTIVE. The previous version waited on `read`, which is wrong
# when you are not at the terminal: on the device this was written for, the
# capture ran for about 2 seconds and stopped while the guest was still alive,
# and the abort 49 seconds later never appeared in the log. Default is 120s,
# which covers a guest that renders for ~50s and then dies. Pass a longer time
# for slow-to-crash guests.
#
# Usage:
#   scripts/capture_guest_crash.sh [seconds] [out-dir]
#   default: 120 seconds, /storage/emulated/0/Download/logs
#
#   scripts/capture_guest_crash.sh 300          # long guest
#   scripts/capture_guest_crash.sh 120 /tmp     # different output dir

set -uo pipefail

DURATION="${1:-120}"
OUT_DIR="${2:-/storage/emulated/0/Download/logs}"
STAMP="$(date +%Y%m%d_%H%M%S)"
RAW="$OUT_DIR/logcat_all_$STAMP.txt"
SUMMARY="$OUT_DIR/crash_summary_$STAMP.txt"

mkdir -p "$OUT_DIR" 2>/dev/null || true
if [ ! -d "$OUT_DIR" ]; then
    echo "cannot write to $OUT_DIR" >&2
    exit 1
fi

# Prefer adb when a device is attached (captures every process's log); fall
# back to Termux's own logcat, which sees the app-visible buffers. adb is only
# trusted if it actually links and runs -- Termux ships adb builds that fail on
# a missing libc++ symbol.
if command -v adb >/dev/null 2>&1 \
   && adb devices >/dev/null 2>&1 \
   && [ -n "$(adb devices 2>/dev/null | awk 'NR>1 && $2=="device" {print $1}')" ]; then
    LOGCAT=(adb logcat)
    SOURCE="adb"
else
    LOGCAT=(logcat)
    SOURCE="termux"
fi

echo "=== SlackBox guest crash capture ==="
echo "source:   $SOURCE"
echo "duration: ${DURATION}s"
echo "out:      $OUT_DIR"
echo

# Clear the ring buffer so the interesting records are not pushed out by the
# minutes of HWUI noise that precede a guest launch.
"${LOGCAT[@]}" -b all -c 2>/dev/null || true

echo "Capturing for ${DURATION}s. Launch the guest now and let it run."
echo

"${LOGCAT[@]}" -b all -v threadtime > "$RAW" 2>/dev/null &
CAPTURE_PID=$!

# Timed wait, not `read`. Sleeping the full duration is correct even if the
# guest dies early: the abort record and any tombstone flush land within a
# second or two of the death, and extra idle seconds cost nothing.
sleep "$DURATION"

kill "$CAPTURE_PID" 2>/dev/null || true
wait "$CAPTURE_PID" 2>/dev/null || true

# Crash/tombstone records plus the container's own tags, noise dropped.
grep -iE \
  'Fatal signal|SIGSEGV|SIGABRT|SIGILL|SIGBUS|beginning of crash|backtrace|DEBUG   |tombstone|AndroidRuntime|FATAL EXCEPTION|lowmemorykiller|Killing|naijun|blackbox|slackbox|guest_process|ApplicationExitInfo|ANR in|Abort message|libblackbox' \
  "$RAW" > "$SUMMARY" 2>/dev/null || true

echo
echo "=== summary ($SUMMARY) ==="
if [ -s "$SUMMARY" ]; then
    cat "$SUMMARY"
else
    echo "(no matching lines)"
    echo "The guest may not have died during the window, or logcat was empty."
    echo "Read the raw dump directly: $RAW"
fi

# The native handler's own output. It now prefers getExternalFilesDir, with
# Download/logs kept as a fallback, so check both.
echo
echo "=== native_crash_*.log ==="
FOUND=0
for dir in \
    "$OUT_DIR" \
    /storage/emulated/0/Android/data/top.niunaijun.blackbox/files/crash_logs \
    /storage/emulated/0/Android/data/top.niunaijun.blackboxa/files/crash_logs \
    "$HOME/../files/crash_logs"
do
    [ -d "$dir" ] || continue
    for f in "$dir"/native_crash_*.log; do
        [ -f "$f" ] || continue
        FOUND=1
        echo "--- $f"
        tail -20 "$f"
    done
done
[ "$FOUND" -eq 0 ] && echo "(none found in the usual roots)"

# Also try run-as, which works without root for a debuggable build.
if command -v run-as >/dev/null 2>&1; then
    for pkg in top.niunaijun.blackbox top.niunaijun.blackboxa; do
        OUT="$(run-as "$pkg" sh -c 'ls files/crash_logs/ 2>/dev/null' 2>/dev/null)"
        if [ -n "$OUT" ]; then
            echo
            echo "=== run-as $pkg files/crash_logs ==="
            echo "$OUT"
            for f in $(echo "$OUT" | grep '^native_crash_'); do
                echo "--- $pkg:$f"
                run-as "$pkg" cat "files/crash_logs/$f" 2>/dev/null | tail -20
            done
        fi
    done
fi

# Tombstones need root; only attempt it when a superuser binary is actually usable.
if command -v su >/dev/null 2>&1 && su -c 'true' >/dev/null 2>&1; then
    TOMB_OUT="$OUT_DIR/tombstones_$STAMP"
    mkdir -p "$TOMB_OUT"
    su -c "cp /data/tombstones/* '$TOMB_OUT/'" 2>/dev/null || true
    echo
    echo "tombstones copied to $TOMB_OUT"
else
    echo
    echo "note: no usable root, so /data/tombstones stays unreadable."
    echo "      The native_crash_*.log above is the substitute, and it carries"
    echo "      signal, fault address, pc and the module the pc is in."
fi

echo
echo "done."
