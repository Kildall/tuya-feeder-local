# Troubleshooting

## Quick check (on the feeder)

```sh
uptime; netstat -lnt | grep -E '6668|8556'; cat /tmp/vhook/boot.log; tail -15 /tmp/vhook/log
```

| Symptom | Cause | Fix |
|---|---|---|
| no `/tmp/vhook/log` | wrapper not used | see `boot.log`: disabled, library missing, or both parts off? |
| `boot.log` says `disabled` | 3 bad boots in a row | fix the cause, then `rm /mnt/config/hook/disable; echo 0 > /mnt/config/hook/boots; reboot` |
| reboots about every minute | the camera encoder stalled (vendor watchdog) | the hook disables itself after 3; read `boot.log` and the log |
| no port 8556 | `CAMERA=0`, or library not loaded | `cat /tmp/bin/ak_tuya_ipc` |
| go2rtc shows no video | another client holds the single connection | stop other viewers or test tools |
| video stutters | weak Wi-Fi: frames dropped until the next keyframe | check `dropped=` in `/tmp/vhook/stats` |
| browser black, VLC fine | browser can't decode H.265 | use go2rtc's H.264 stream |
| `offline assist: off, code ... differs` | different firmware | Part 2 can't run on this build |
| `offline assist: not needed, cloud came online` | the device reaches the internet | fix the block rule ([network.md](network.md)) |
| flags set, `6668 still closed`, `nw=1` | firewall **drops** instead of rejecting | switch to REJECT, then `rm -f /tmp/vhook/assist_count; killall ak_tuya_ipc` |
| 6668 open but tuya-local unavailable | wrong key or protocol, or another client connected | protocol 3.2; close the Smart Life app on the LAN |

## Settings

Choose parts permanently in `/mnt/config/hook/vhook.conf` (`CAMERA=0|1`, `OFFLINE=0|1`). For a one-off test, add `export NAME=value` lines to `/tmp/bin/ak_tuya_ipc` and run `killall ak_tuya_ipc`.

| Variable | Default | Meaning |
|---|---|---|
| `VHOOK_CHN` | 0 | encoder channel to stream (1 = sub-stream, -1 = none) |
| `VHOOK_PORT` | 8556 | stream port |
| `VHOOK_OFFLINE` | 1 | Part 2 on/off |
| `VHOOK_OFFLINE_DELAY` | 45 | seconds before the offline assist acts (minimum 20) |

`VHOOK_LAN_KICK`, `VHOOK_LAN_INIT` and `VHOOK_SDK_LOG` are research diagnostics. They're off by default and weren't needed in the end.

## Restarting without a reboot

BusyBox's shell caches where it found `ak_tuya_ipc`. The respawn loop that was started before the wrapper existed keeps launching the original. Restart the loop once with a fresh environment ([install.md § 4](install.md#4-test-without-installing-optional)); after that, `killall ak_tuya_ipc` reloads the library. At boot this isn't an issue.

## Seeing the firmware's own output

The program prints to the serial console. To capture it briefly (it's kept in RAM, so don't leave it running):
```sh
sed -i 's#^exec /usr/sbin/ak_tuya_ipc "$@"$#exec /usr/sbin/ak_tuya_ipc "$@" >/tmp/vhook/tuya.log 2>\&1#' /tmp/bin/ak_tuya_ipc && killall ak_tuya_ipc
# later: grep -iE 'server time|err' /tmp/vhook/tuya.log
# undo:  sed -i 's# >/tmp/vhook/tuya.log 2>&1##' /tmp/bin/ak_tuya_ipc && rm /tmp/vhook/tuya.log && killall ak_tuya_ipc
```

## Finding where a thread is stuck

This is how the `baidu.com` problem was found:
```sh
P=$(pgrep ak_tuya_ipc); for t in /proc/$P/task/*; do echo "$(cat $t/comm) | $(cat $t/wchan)"; done
```
`lpc_task` is the local-control thread, and `mq_cntl_ta` the cloud client (stuck in `connect` offline, which is expected). For more detail, dump a thread's stack from `/proc/$P/mem` (the stack pointer is in `/proc/$P/task/<tid>/syscall`), copy it to a PC with `scripts/feeder_xfer.py receive`, and look for return addresses into the program.
