# Blocking the device from the internet

The goal is for the feeder to reach **nothing outside your LAN**, while Home Assistant and go2rtc can still reach it on ports 6668 and 8556.

## Requirements

1. **A fixed IP address** for the feeder: a static DHCP lease. tuya-local, go2rtc and your firewall rule all refer to it.
2. **Block all traffic from the feeder to the internet**, in both TCP and UDP, including DNS to public resolvers. The firmware hard-codes `8.8.8.8` and `114.114.114.114` ahead of your DHCP-provided DNS.
3. **Use REJECT, not DROP.** This is essential, not a nicety:
   - The firmware's LAN task checks internet access by calling `connect()` to `baidu.com:80`, twice and blocking, *on the same thread that opens port 6668*.
   - With DROP, each connect waits for the kernel's SYN retries (~2 minutes), so 6668 stays closed for 4+ minutes after each check, sometimes indefinitely.
   - With REJECT (TCP RST, ICMP port/host unreachable), every check fails instantly and 6668 opens about 10 s after the hook releases the firmware.
   - REJECT also makes the public-DNS fallbacks fail fast, so local name lookups don't wait.
4. Leave LAN traffic alone. Your local DNS can stay reachable; it doesn't matter either way.

Traffic between the feeder and your LAN, including Home Assistant, go2rtc and your PC, normally doesn't cross the router's firewall at all, so blocking "LAN → internet" is enough. If the feeder sits on a separate VLAN or subnet, also allow TCP 6668 and 8556 from the hosts running Home Assistant and go2rtc.

## Examples

Replace `192.0.2.50` with the feeder's address, `192.0.2.0/24` with your LAN, and `wan0` with your WAN interface.

### iptables (a Linux router)
```sh
iptables -I FORWARD -s 192.0.2.50 -o wan0 -p tcp -j REJECT --reject-with tcp-reset
iptables -I FORWARD -s 192.0.2.50 -o wan0 -j REJECT --reject-with icmp-port-unreachable
```

### nftables
```
table inet filter {
  chain forward {
    type filter hook forward priority 0;
    ip saddr 192.0.2.50 oifname "wan0" meta l4proto tcp reject with tcp reset
    ip saddr 192.0.2.50 oifname "wan0" reject
  }
}
```

### OpenWrt (`/etc/config/firewall`)
```
config rule
    option name 'feeder-no-internet'
    option src 'lan'
    option src_ip '192.0.2.50'
    option dest 'wan'
    option proto 'all'
    option target 'REJECT'
```

### Other routers and firewalls
Look for a per-device "block internet" or firewall rule and pick the **reject** action, sometimes called "deny with reset" or "reject with ICMP". If your router only offers silent dropping, the offline boot still works eventually, but port 6668 can take many minutes to come up after each boot.

## Checking it

From the feeder's shell:
```sh
nslookup example.com 8.8.8.8      # should fail immediately ("can't resolve"), not hang
wget -T 5 -O - http://example.com # should fail immediately
```

Also confirm the Tuya app shows the device as offline when your phone is on mobile data.

## Before you block it

- **Get the local key first**, while the device is still online, using the Tuya IoT platform, tinytuya's wizard or similar. tuya-local needs it, and it doesn't change while the device stays paired.
- **Turn off any meal plan in the Smart Life app** if Home Assistant will schedule meals. The device's clock-based features shouldn't be relied on offline.
- Keep the device blocked permanently. If it ever reaches the cloud it may install a firmware update, which would likely remove the hook's foundations. The library would then detect the changed code and stay passive, and the camera tap and offline boot would stop working.
