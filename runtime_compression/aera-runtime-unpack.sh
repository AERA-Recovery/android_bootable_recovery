#!/sbin/busybox sh
# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0
# Invoked synchronously by init before device HAL startup. No /data dependency.
BB=/sbin/busybox
PAYLOAD=/aera-runtime.tar.xz

log() {
    echo "AERA runtime: $*" > /dev/kmsg 2>/dev/null || true
}

# Compatibility with a recovery ramdisk repacked into ordinary files.
if [ ! -f "$PAYLOAD" ]; then
    if [ -x /system/bin/recovery ]; then
        log "uncompressed runtime is already available"
        exit 0
    fi
    log "ERROR: runtime payload and recovery executable are missing"
    exit 1
fi

start=$($BB awk '{printf "%.0f", $1 * 1000}' /proc/uptime 2>/dev/null)
if ! $BB tar -xJpf "$PAYLOAD" -C /; then
    log "ERROR: runtime extraction failed; bootstrap ADB is retained"
    exit 1
fi
if [ ! -x /system/bin/recovery ]; then
    log "ERROR: unpacked recovery executable is missing or not executable"
    exit 1
fi
end=$($BB awk '{printf "%.0f", $1 * 1000}' /proc/uptime 2>/dev/null)
if [ -n "$start" ] && [ -n "$end" ]; then
    log "unpacked successfully in $((end-start)) ms"
else
    log "unpacked successfully"
fi
# Keep the archive available: Keep AERA / ramdisk repacking uses it verbatim.
exit 0
