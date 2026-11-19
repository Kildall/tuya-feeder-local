# The device

Observed on one unit with firmware V68.01.10.

## Is yours the same?

| Check | Expected |
|---|---|
| Tuya product ID | `cjrm04np59ut4pwb` (model string `V330L-DU4L-VS`; tuya-local config `v330l_petfeeder`) |
| Firmware (Smart Life → Device update) | V68.01.10 for both "main module" and "MCU module" |
| Open TCP ports | 23 (telnet, banner `ipc login:`), 6668 (only when "online", see [Part 2](part2-local-only.md)), 9990; no 80, 443, 554 or ONVIF |
| `cat /proc/cpuinfo` | `Hardware : AK39EV330` |
| `md5sum /usr/sbin/ak_tuya_ipc` | `520b3b67df9c22afa8e03d636a6911ea` |
| `cat /usr/ipcam/bak/jiake_version.txt` | `version=68.00.10` (the OEM's internal number) |

tuya-local lists sibling product IDs (`ibnk6keua0zzc2gr`, `7bkqbfbpv7qbfskr`) for the same config. They're probably the same design, but untested here.

## Hardware

- **SoC:** Anyka AK3918EV330 (ARMv5, ARM926EJ-S).
- **RAM:** 64 MB, of which Linux sees ~27 MB. About **1.7 MB is usually free**, plus zram swap.
- **Flash:** 8 MB SPI NOR. The system image is read-only and full. One small writable partition (`/mnt/config`, ~640 KB free) holds the settings, the Tuya keys and the Wi-Fi password, and it's where this project puts its two files.
- **Camera:** SC223A 1080p sensor. The encoder produces H.265 on two channels (main 1080p, plus a smaller sub-stream).
- **Wi-Fi:** USB Wi-Fi chip. The MAC prefix `34:7D:E4` belongs to a common Tuya module vendor.
- **Feeding:** a separate microcontroller drives the motor, listed as the "MCU module" in Smart Life. The camera SoC talks to it over a serial port.
- **SD card:** the firmware supports one (mounted under `/mnt/config/record`). The tested unit had no card.

## Software

- **Base system:** Linux 4.4, BusyBox, uClibc-ng 1.0.28 (Buildroot 2018.02). BusyBox is minimal: no `head`, `tr` or `strings`, but it has `nc -l`, `sed`, `awk`, `dd`, `hexdump`, `md5sum`, `netstat`, `lsof`, `pgrep`.
- **`ak_tuya_ipc`** does nearly everything: camera and encoder, the serial link to the feeding MCU, Tuya cloud (MQTT/HTTPS), and the Tuya local protocol (TCP 6668, UDP 6667 broadcast). It contains the Tuya IoT SDK 4.1.1 (LAN protocol 3.3). It can't be stopped without losing feeding.
- **`telnetd`:** root shell on port 23 with a factory login.
- **`jksdk_tcp`:** a tiny vendor helper on port 9990, used only by the Wi-Fi scripts over localhost. It isn't video.

## Boot process (the parts that matter)

1. `rcS` mounts `/mnt/config`, loads drivers, and starts `telnetd` (so it's **always reachable**).
2. `start_tuya_ipc.sh` runs **`/mnt/config/tmp_test.sh` if it exists**. That's a vendor hook, and this project uses it.
3. It then starts `tuya_daemon.sh`, a loop that runs `ak_tuya_ipc` **by name** and restarts it if it exits. `/tmp/bin` comes before `/usr/sbin` in the search path, so a wrapper placed there runs instead.
4. `video_stream_detect.sh` **reboots the device** if the encoder stops for about a minute.
5. `http_auto_upgrade.sh` checks for firmware updates at every boot. That's harmless once the device is blocked.

## Network behaviour offline

- The DNS list starts with hard-coded **8.8.8.8 and 114.114.114.114**, before your DHCP-provided server. Lookups only reach your resolver after those fail.
- The firmware checks for internet with a connect to **`baidu.com:80`**.
- It retries the Tuya cloud continuously. That's harmless when blocked.
- The only NTP server it uses (`time.windows.com`) is contacted only after the cloud has answered.
