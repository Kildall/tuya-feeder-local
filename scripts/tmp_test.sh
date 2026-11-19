#!/bin/sh
# vhook boot hook (the loader shared by both parts). Installed at /mnt/config/tmp_test.sh on the feeder.
# The stock start_tuya_ipc.sh runs this file at every boot, before tuya_daemon.sh starts ak_tuya_ipc.
# It puts a wrapper in /tmp/bin (searched before /usr/sbin) that preloads libvhook.so into ak_tuya_ipc.
#
# Parts, chosen in /mnt/config/hook/vhook.conf (optional; both default to 1):
#   CAMERA=1    Part 1: serve the main camera channel as raw H.265 on TCP 8556
#   OFFLINE=1   Part 2: release the firmware's cloud waits so it works with no internet
#
# Off switch:        touch /mnt/config/hook/disable   (then reboot)
# Remove entirely:   rm /mnt/config/tmp_test.sh; rm -r /mnt/config/hook   (then reboot)
# Crash-loop guard:  every boot adds 1 to /mnt/config/hook/boots; after 5 minutes with the hook
#                    loaded it goes back to 0. At 3 (three boots in a row that never got there)
#                    the hook disables itself and the feeder runs stock.

H=/mnt/config/hook
LIB=$H/libvhook.so
PORT=8556
MAX_BAD_BOOTS=3
CAMERA=1
OFFLINE=1
[ -f "$H/vhook.conf" ] && . "$H/vhook.conf"
if [ "$CAMERA" = 1 ]; then CHN=0; else CHN=-1; fi
if [ "$OFFLINE" = 1 ]; then OFFLINE=1; else OFFLINE=0; fi

mkdir -p /tmp/vhook
log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> /tmp/vhook/boot.log; }

if [ "$CAMERA" != 1 ] && [ "$OFFLINE" != 1 ]; then
    log "both parts off in $H/vhook.conf: running stock"
    exit 0
fi
if [ -e "$H/disable" ]; then
    log "disabled ($H/disable exists): running stock"
    exit 0
fi
if [ ! -f "$LIB" ]; then
    log "missing $LIB: running stock"
    exit 0
fi

boots=0
[ -f "$H/boots" ] && boots=$(cat "$H/boots" 2>/dev/null)
case "$boots" in ''|*[!0-9]*) boots=0 ;; esac
if [ "$boots" -ge "$MAX_BAD_BOOTS" ]; then
    echo "$boots bad boots in a row" > "$H/disable"
    log "$boots boots without a healthy hook: disabling, running stock"
    exit 0
fi
echo $((boots + 1)) > "$H/boots"

mkdir -p /tmp/bin
cat > /tmp/bin/ak_tuya_ipc <<EOF
#!/bin/sh
export LD_PRELOAD=$LIB
export VHOOK_CHN=$CHN
export VHOOK_PORT=$PORT
export VHOOK_OFFLINE=$OFFLINE
exec /usr/sbin/ak_tuya_ipc "\$@"
EOF
chmod 755 /tmp/bin/ak_tuya_ipc
log "wrapper installed: camera=$CAMERA offline=$OFFLINE (boot $((boots + 1)) of max $MAX_BAD_BOOTS before auto-disable)"

# After 5 minutes, mark this boot healthy if ak_tuya_ipc is running with the hook loaded.
(
    sleep 300
    set -- $(pgrep ak_tuya_ipc)
    if [ -n "$1" ] && grep -q libvhook "/proc/$1/maps" 2>/dev/null; then
        echo 0 > "$H/boots"
        log "healthy after 5 min (pid $1): boot counter reset"
    else
        log "hook not loaded after 5 min: boot counter left at $(cat $H/boots)"
    fi
) </dev/null >/dev/null 2>&1 &

exit 0
