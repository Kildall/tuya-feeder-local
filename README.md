# Tuya camera pet feeder: local camera and local-only operation

Software-only add-ons for one Tuya/Smart Life camera pet feeder (V330L, Anyka AK3918EV330, firmware V68.01.10). You don't need to solder or flash anything, and removing two files restores stock behaviour.

| | Part 1: [local camera](docs/part1-local-camera.md) | Part 2: [local-only operation](docs/part2-local-only.md) |
|---|---|---|
| **Gives you** | the built-in camera as a LAN stream for go2rtc → Home Assistant, Frigate, VLC | the feeder works with its internet blocked, including after reboots |
| **Internet** | works online or offline | blocked, with **REJECT** |
| **Smart Life app** | keeps working, at the same time as the local stream (tested) | stops working |

Use either part alone or both. Both together give you a feeder with a camera where nothing leaves your network.

## Key facts

- **Stock firmware doesn't come up properly after an offline boot.** If it loses internet while running, it keeps working, so this is easy to miss. But after a reboot or power cut with no internet, it waits for Tuya's cloud indefinitely, and parts of it never start. Part 2 fixes that: local control (tuya-local) is back about **1 minute** after power-on.
- **Block with REJECT, not DROP.** The firmware tests its internet connection by connecting to `baidu.com` on the same thread that serves local control. With silently dropped packets, each test hangs for minutes. With REJECT it fails instantly.
- **No cloud-free video in the stock firmware.** There's no RTSP, no ONVIF toggle and no local stream. Video only goes through Tuya's cloud. Part 1 adds a local stream.
- **Stream:** H.265, 1920×1080, about 10 fps, a keyframe every ~3 s, around 50–70 kbit/s for a still scene. One client at a time, so put go2rtc in front of it.
- **Feeding is never touched.** The motor runs on its own microcontroller, driven by the stock program. These add-ons only read video and set the firmware's own status flags.
- **Firmware-specific:** Part 2 checks the firmware's code before acting and stays passive on any other build. Keep the device blocked so it can't update itself.
- **Time:** the device's internal Tuya clock reads 1970 when offline. Schedule meals in Home Assistant, not in the app. NTP alone doesn't help.
- **Tight on memory:** about 1.7 MB of RAM is free on the device, so nothing extra runs there. Everything runs inside the stock program.

## How to do it (high level)

1. **Check your unit:** product ID `cjrm04np59ut4pwb`, firmware V68.01.10 in the Smart Life app, and telnet port 23 open ([device.md](docs/device.md)).
2. **Get the local key** for tuya-local while the feeder is still online (for example with tinytuya's wizard).
3. **Log in over telnet** as root. It uses the factory login, user `root` pass `AK2040jk` ([source](https://www.linkedin.com/pulse/snhack-attack-how-hackers-could-turn-your-smart-pet-feeder-bongiorni-fxtsf/))
4. **Back up the flash** to your PC ([install.md § 2](docs/install.md#2-back-up-the-flash)).
5. **Build the library** with the matching toolchain: `scripts/get_toolchain.sh`, then `scripts/build.sh`.
6. **Install two files** in the feeder's writable partition: the library and the boot hook. Optionally add a one-line config to pick the parts.
7. **Reboot.** For Part 1, add the stream to go2rtc ([examples](examples/)).
8. **For Part 2, block the feeder's internet with REJECT** ([network.md](docs/network.md)), reboot, and check that local control is back within about a minute.

Step-by-step commands: [docs/install.md](docs/install.md).

## Safety

- If the code doesn't match the expected firmware, Part 2 does nothing and logs why.
- Part 2 makes at most 3 attempts per boot. After 3 bad boots in a row, the hook disables itself and the feeder runs stock.
- Telnet starts before the hook, so you can always remove it: `rm /mnt/config/tmp_test.sh; rm -r /mnt/config/hook; reboot`.

## Docs

| | |
|---|---|
| [part1-local-camera.md](docs/part1-local-camera.md) | how the video tap works |
| [part2-local-only.md](docs/part2-local-only.md) | why the stock firmware stalls offline and what the fix does |
| [install.md](docs/install.md) | commands for every step, plus update and removal |
| [loader.md](docs/loader.md) | how the library gets loaded at boot, and the safety guards |
| [network.md](docs/network.md) | blocking the device correctly |
| [device.md](docs/device.md) | hardware, flash layout, boot process |
| [troubleshooting.md](docs/troubleshooting.md) | symptoms, settings, diagnostics |
| [research-notes.md](docs/research-notes.md) | approaches ruled out |

The exact firmware locations used by Part 2 are documented in the comments of [src/vhook.c](src/vhook.c).

MIT licensed. Not affiliated with Tuya, Anyka or any feeder brand. No vendor firmware is included.
