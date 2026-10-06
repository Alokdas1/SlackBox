#!/bin/bash
# Capture a guest crash from Termux (no root required) or over adb.
#
# Why this exists: a guest process that dies surfaces in SlackBox as
#   guest_process_binder_died / cause=unknown
# because a Binder death cannot distinguish a crash from a normal exit. The
# reason lives outside the container, in logcat: the native crash/tombstone
# buffer and the system's "Fatal signal" / ANR records. There is no way to
# reconstruct it after the fact -- the ring buffer rolls over -- so the buffers
# have to be cleared, streamed while you reproduce the death, then filtered.
#
# Usage:  scripts/capture_guest_crash.sh [out-dir]
#   default out-dir: /storage/emulated/0/Download/logs
set -euo pipefail

OUT_DIR="${1:-/storage/emulated/0/Download/logs}"
STAMP="$(date +%Y%m%d_%H%M%S)"
RAW="$OUT_DIR/logcat_all_$STAMP.txt"
SUMMARY="$OUT_DIR/crash_summary_$STAMP.txt"

mkdir -p "$OUT_DIR"

# Prefer adb when a device is attached (captures every process's log); fall
# back to Termux's own logcat, which sees the app-visible buffers. adb is only
# trusted if it actually links and runs -- Termux ships adb builds that fail on
# a missing libc++ symbol.
if command -v adb >/dev/null 2>&1 \
   && adb devices >/dev/null 2>&1 \
   && [ -n "$(adb devices 2>/dev/null | awk 'NR>1 && $2=="device" {print $1}')" ]; then
    LOGCAT=(adb logcat)
else
    LOGCAT=(logcat)
fi

echo "=== SlackBox guest crash capture ==="
echo "raw:     $RAW"
echo "summary: $SUMMARY"
echo

"${LOGCAT[@]}" -b all -c

echo "Now open SlackBox, launch the guest app, and play until it crashes."
echo "When the guest dies, return here and press Enter."
"${LOGCAT[@]}" -b all -v threadtime > "$RAW" 2>/dev/null &
CAPTURE_PID=$!

# Wait on the operator, not a timer: the crash moment is not predictable.
read -r _

# Give the stream a beat to flush the tail (the tombstone) before we cut it.
sleep 1

kill "$CAPTURE_PID" 2>/dev/null || true
wait "$CAPTURE_PID" 2>/dev/null || true

# Crash/tombstone records plus the guest's own tags, with the noise dropped.
grep -iE \
  'Fatal signal|SIGSEGV|SIGABRT|SIGILL|SIGBUS|beginning of crash|DEBUG|tombstone|AndroidRuntime|FATAL EXCEPTION|lowmemorykiller|Killing|proxima|blackbox|guest_process|ApplicationExitInfo|ANR in' \
  "$RAW" > "$SUMMARY" 2>/dev/null || true

echo
echo "=== summary ($SUMMARY) ==="
cat "$SUMMARY" 2>/dev/null || echo "(no matching lines -- the death may have been silent; read the raw dump)"

# Tombstones need root; only attempt it when a superuser binary is actually usable.
if command -v su >/dev/null 2>&1 && su -c 'true' >/dev/null 2>&1; then
    TOMB_OUT="$OUT_DIR/tombstones_$STAMP"
    mkdir -p "$TOMB_OUT"
    su -c "cp /data/tombstones/* '$TOMB_OUT'/" || true
    echo "tombstones copied to $TOMB_OUT"
else
    echo "note: no usable root, so /data/tombstones stays unreadable (permission denied)."
fi

echo "done."
