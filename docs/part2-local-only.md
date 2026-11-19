# Part 2: local-only operation

**Result:** the feeder keeps working with its internet blocked permanently, including after reboots and power cuts. Local control (TCP 6668: tuya-local, LocalTuya, tinytuya) is back about 1 minute after power-on.

**You lose:** the Smart Life app, cloud video, cloud schedules and notifications, and firmware updates.

**Requires:** the device blocked with **REJECT** ([network.md](network.md)), and its local key obtained beforehand.

## The problem: the stock firmware after an offline boot

| Situation | Stock behaviour |
|---|---|
| Internet lost **while running** | keeps working locally: 6668 stays open |
| **Booted** without internet | waits for the cloud indefinitely: the app's start-up never finishes, and local control depends on a slow fallback (below) |

There are four things the firmware waits for, each normally provided by the Tuya cloud:

1. **"Cloud online"**: the main thread loops until the cloud connection reports online, and only then starts its remaining services.
2. **"Clock synced" and "time zone received"**: the next step retries "get server time" until the cloud has sent both.
3. **"Network usable"**: the SDK only starts the local-control server once the vendor's network check passes. That check is a TCP connect to `baidu.com:80`. Its offline fallback is "`/tmp/resolv.conf` has existed for 200 polls".
4. **The server thread runs that check itself.** The thread that should open port 6668 is the one doing the `baidu.com` connect, twice in a row, blocking. If the firewall **drops** packets, each attempt hangs for about 2 minutes. That's why DROP makes everything take forever, and why REJECT is required.

Points 1 and 2 never resolve offline. Point 3's fallback eventually would, but slowly: every poll waits for the network check, so with DROP it takes hours. A stock unit's offline boot hasn't been timed end to end.

## The fix

45 seconds after the program starts, if the cloud still isn't online, the library sets the four status flags the cloud would have caused: station connected, clock synced, time zone received, cloud online. It writes the same value (1) the firmware itself writes at those places.

The firmware then finishes its normal start-up, and its own local-control thread opens port 6668 a few seconds later. Nothing else is patched, and the code itself is never modified.

Before writing anything, the library checks that the firmware code around each flag is exactly what's expected for V68.01.10. On any difference it does nothing and logs which check failed. The exact locations are in the comments of [src/vhook.c](../src/vhook.c).

## Side effects

- **The firmware thinks it's online,** so it also starts its cloud services: storage, MQTT and so on. They keep failing quietly, and fast, because the traffic is rejected.
- **The Tuya SDK's internal clock reads 1970 plus uptime,** because it never got a real time. The system clock isn't changed. Don't rely on schedules stored in the device.
- **The firmware's NTP fallback (`time.windows.com`)** only runs after a cloud time reply, so allowing NTP alone changes nothing.
- **With internet access,** the assist does nothing (`offline assist: not needed, cloud came online`, tested). If internet comes back after the flags were set, the device connects normally, and the flags match what the cloud would set anyway.

## Timeline of an offline boot (measured)

| | |
|---|---|
| ~6 s | boot hook installs the wrapper |
| ~13 s | stock program starts with the library |
| ~58 s | flags set (45 s later) |
| ~68 s | port 6668 listening |
| ~1 min | Home Assistant reconnects |

## Setup

Follow [install.md](install.md); Part 2 is on by default. Then [network.md](network.md), and [verify an offline boot](install.md#7-verify-an-offline-boot-part-2).
