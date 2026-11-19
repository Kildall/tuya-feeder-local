# The loader (shared by both parts)

Both parts are one small library (`libvhook.so`) preloaded into the stock `ak_tuya_ipc`. Four things in the vendor firmware make that possible without flashing:

1. **A boot hook:** `start_tuya_ipc.sh` runs `/mnt/config/tmp_test.sh` at every boot if it exists. `/mnt/config` is writable and survives reboots.
2. **The search path:** `/tmp/bin` comes before `/usr/sbin`, so a wrapper at `/tmp/bin/ak_tuya_ipc` runs instead of the real program, sets `LD_PRELOAD`, and starts the real one.
3. **A respawn loop:** `tuya_daemon.sh` restarts the program through the same wrapper whenever it exits.
4. **Preloading:** the system's dynamic loader supports `LD_PRELOAD`, which `scripts/build.sh` checks.

## What the boot hook does

- Reads `/mnt/config/hook/vhook.conf` (optional). `CAMERA=1|0` and `OFFLINE=1|0` both default to 1; with both 0 it does nothing.
- Exits, leaving the device stock, if `/mnt/config/hook/disable` exists or the library is missing.
- Counts boots. After **3 boots in a row** without 5 healthy minutes, it creates `disable` and the feeder runs stock.
- Writes the wrapper into `/tmp/bin` (recreated at every boot).
- Logs to `/tmp/vhook/boot.log`.

## Files on the feeder

| Path | |
|---|---|
| `/mnt/config/tmp_test.sh` | boot hook |
| `/mnt/config/hook/libvhook.so` | library |
| `/mnt/config/hook/vhook.conf` | optional part selection |
| `/mnt/config/hook/boots`, `disable` | boot-loop guard |
| `/tmp/vhook/` | logs (RAM, cleared at reboot) |

## Why it can't brick the feeder

- **Feeding** runs on a separate microcontroller, and the library never touches its link.
- **If the program crash-loops,** the vendor's video watchdog reboots the device, and after 3 such boots the hook turns itself off.
- **Telnet** starts before the hook, so the two files can always be deleted.
