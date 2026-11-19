# Install

About an hour the first time. Steps 1–5 apply to both parts, and steps 6–7 to Part 2 only. Until step 5, a reboot undoes everything. How the boot hook works: [loader.md](loader.md).

You need:
- The feeder on your LAN, **still online**. For Part 2, also obtain its **local key** first (see [network.md](network.md#before-you-block-it)).
- A Linux machine or WSL with `python3`, `curl`, `tar`, `bzip2`, and optionally `ffprobe` and `unsquashfs` (or 7-Zip).
- A telnet client.

In the commands below, `FEEDER` is the feeder's IP address.

## 1. Get a shell

The device runs BusyBox `telnetd` on port 23 with a root account (`ipc login:`). The login is the vendor's factory default. This repo doesn't publish it; owners of this model have discussed it in tuya-local's GitHub threads for `v330l_petfeeder`. If you can't log in, stop here.

```sh
telnet FEEDER
```

Confirm you have the same firmware build. Everything below is read-only:

```sh
cat /proc/cpuinfo | grep Hardware          # AK39EV330
cat /usr/ipcam/bak/jiake_version.txt       # version=68.00.10
md5sum /usr/sbin/ak_tuya_ipc /usr/lib/libmpi_venc.so
# 520b3b67df9c22afa8e03d636a6911ea  /usr/sbin/ak_tuya_ipc
# e2233f446479efb5430e1fd4cedeeebd  /usr/lib/libmpi_venc.so
cat /proc/mtd                              # 7 partitions, mtd0–mtd6
```

If `ak_tuya_ipc` differs, Part 2 won't run (its code checks fail). Part 1 may still work, since it only relies on the encoder library's interface.

## 2. Back up the flash

The backup is read-only and takes about a minute. Keep it somewhere private: `mtd6_CONFIG` holds your Wi-Fi password and the device's Tuya keys.

PC:
```sh
python3 scripts/feeder_xfer.py --host FEEDER backup ./backup
```
Feeder:
```sh
for p in 0 1 2 3 4 5 6; do echo "mtd$p"; nc -l -p 9000 < /dev/mtd$p; done; echo BACKUP_DONE
```
Every line on the PC should end in `OK`. To check the system image, unpack it; the build checks in step 3 use it:
```sh
unsquashfs -d rootfs backup/mtd5_ROOTFS.bin     # or: 7z x -orootfs backup/mtd5_ROOTFS.bin
```

## 3. Build

```sh
scripts/get_toolchain.sh                   # Bootlin armv5-eabi uClibc 2018.02-2 (~48 MB, sha256-checked)
ROOTFS=$PWD/rootfs scripts/build.sh        # → build/libvhook.so
```
The build should end with `imports: all present in the device's libc`, `device loader: LD_PRELOAD supported` and `BUILD OK`.

## 4. Test without installing (optional)

This only uses `/tmp`, so a reboot undoes it.

Feeder:
```sh
mkdir -p /tmp/hook /tmp/bin && nc -l -p 9000 > /tmp/hook/libvhook.so; md5sum /tmp/hook/libvhook.so
```
PC:
```sh
python3 scripts/feeder_xfer.py --host FEEDER send build/libvhook.so    # md5s must match
```
Feeder: create the wrapper. The running `tuya_daemon.sh` has cached the old path, so it has to be restarted with a fresh shell environment:
```sh
cat > /tmp/bin/ak_tuya_ipc <<'EOF'
#!/bin/sh
export LD_PRELOAD=/tmp/hook/libvhook.so
export VHOOK_CHN=0
exec /usr/sbin/ak_tuya_ipc "$@"
EOF
chmod +x /tmp/bin/ak_tuya_ipc
kill $(pgrep -f tuya_daemon.sh); killall ak_tuya_ipc; (. /etc/common.env; . /etc/profile.env; . /etc/sensor_type.env; . /etc/wifi_type.env; trap '' HUP; exec tuya_daemon.sh) </dev/null >/dev/null 2>&1 &
```
About 40 s later:
```sh
grep -c vhook /proc/$(pgrep ak_tuya_ipc)/maps; cat /tmp/vhook/log; cat /tmp/vhook/stats
```
The `maps` count should be above 0, and the log should show `vhook loaded ... chn=0 port=8556` and `offline assist: armed`. Then grab some video on the PC:
```sh
python3 scripts/feeder_xfer.py --host FEEDER capture 20 sample.h265
ffprobe -v error -f hevc -show_entries stream=codec_name,width,height sample.h265   # hevc 1920x1080
```
Feeding is unavailable for about 1 minute whenever `ak_tuya_ipc` restarts.

## 5. Install permanently

Feeder:
```sh
mkdir -p /mnt/config/hook && nc -l -p 9000 > /mnt/config/hook/libvhook.so && nc -l -p 9000 > /mnt/config/tmp_test.sh && chmod 755 /mnt/config/tmp_test.sh && sync && md5sum /mnt/config/hook/libvhook.so /mnt/config/tmp_test.sh && sh -n /mnt/config/tmp_test.sh && echo SYNTAX_OK
```
PC (two transfers, one after the other):
```sh
python3 scripts/feeder_xfer.py --host FEEDER send build/libvhook.so
python3 scripts/feeder_xfer.py --host FEEDER send scripts/tmp_test.sh
```
Make sure `scripts/tmp_test.sh` has Unix line endings.

Choose the parts. Both are on by default:
```sh
echo 'OFFLINE=0' > /mnt/config/hook/vhook.conf     # Part 1 only (camera; device stays on the cloud)
echo 'CAMERA=0'  > /mnt/config/hook/vhook.conf     # Part 2 only (offline, no video tap)
rm -f /mnt/config/hook/vhook.conf                  # both
```
If the MD5s match and you see `SYNTAX_OK`, run `reboot`. About 1 minute later, `cat /tmp/vhook/boot.log` should show `wrapper installed: camera=1 offline=…`.

**Part 1 only:** you're done once the stream plays ([examples/](../examples/)). Steps 6–7 don't apply.

## 6. Block the internet (Part 2)

Follow [network.md](network.md): use a static lease and **REJECT**, not DROP. Then `reboot` the feeder so it starts offline.

## 7. Verify an offline boot (Part 2)

About 2 minutes after power-on:
```sh
uptime; netstat -lnt | grep -E '6668|8556'; cat /tmp/vhook/boot.log; grep 'offline assist' /tmp/vhook/log | tail -3
```
Expected:
```
tcp  0  0 0.0.0.0:6668  0.0.0.0:*  LISTEN
tcp  0  0 0.0.0.0:8556  0.0.0.0:*  LISTEN
[...] wrapper installed: camera=1 offline=1 (boot 1 of max 3 before auto-disable)
... offline assist: station-connected, clock-synced, time-zone and online flags set; main thread released
... offline assist: 6668 LISTENING 10 s after release (nw=1 wifi=6)
```
About 5 minutes after boot, `boot.log` adds `healthy after 5 min ... boot counter reset`.

Then set up go2rtc and Home Assistant: [examples/](../examples/).

## Update the library

Never overwrite the loaded file in place, because the running process has it mapped. Write a new file and rename it:
```sh
nc -l -p 9000 > /mnt/config/hook/libvhook.so.new && md5sum /mnt/config/hook/libvhook.so.new && mv /mnt/config/hook/libvhook.so.new /mnt/config/hook/libvhook.so && sync && rm -f /tmp/vhook/assist_count && killall ak_tuya_ipc
```

## Turn off or remove

```sh
touch /mnt/config/hook/disable; reboot                    # off: the device runs stock (needs the cloud at boot again)
echo 'OFFLINE=0' > /mnt/config/hook/vhook.conf; reboot    # keep the camera, drop Part 2
rm /mnt/config/hook/disable; echo 0 > /mnt/config/hook/boots; reboot   # back on
rm /mnt/config/tmp_test.sh; rm -r /mnt/config/hook; reboot             # remove completely
```
