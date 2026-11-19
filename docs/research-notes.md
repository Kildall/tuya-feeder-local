# Research notes

What was tried, and why the other approaches were dropped.

## Part 1: getting video locally

| Approach | Result |
|---|---|
| ONVIF/RTSP toggle in the app | doesn't exist for this device; no streaming ports are open |
| Built-in RTSP server | none in the firmware |
| Port 9990 | a Wi-Fi helper, not video |
| go2rtc `tuya` source, tuya-ipc-terminal, tuya-rtsp-bridge | need the device online, because Tuya's cloud sets up every stream |
| Cloud-free stream setup over 6668 (works on some Tuya devices) | needs LAN protocol 3.5; this device uses 3.3 |
| Emulating Tuya's cloud locally | would need a forged TLS/MQTT backend; no project does this for cameras |
| tuya-convert / tuya-cloudcutter | made for Wi-Fi modules, not Linux camera chips |
| OpenIPC / Thingino | would replace the program that also drives feeding; Anyka EV330 support is immature |
| **Preloaded library tapping the encoder** | works, and is used here. Same idea as [ingenic-stream-hack](https://github.com/Drafteed/ingenic-stream-hack) |

## Part 2: making it work offline

| Hypothesis | Outcome |
|---|---|
| It only needs NTP | no: with NTP allowed, it never even tried. NTP only runs after a cloud reply |
| Main thread waits for the cloud | yes: setting "online" got it past the first wait |
| …then it needs a cloud time and time zone | yes: setting both got past the second wait |
| One flag guessed wrong at first | the code check stopped the write; corrected in the next build |
| Re-running the SDK's LAN start-up | no effect: the LAN module already existed. The code check also caught a wrong table entry before anything was posted |
| The SDK thinks the network is down | yes: it needs the `baidu.com` connectivity check; setting its result made the network "valid" |
| …but port 6668 still stayed closed | a stack dump of the LAN thread showed it **stuck inside the `baidu.com` connect**, because the firewall dropped packets. **REJECT** fixed it at once |

## Lessons

- Checking the code before every write paid off twice: two mistakes, no harm done.
- For the last step, runtime inspection (thread states in `/proc`, a raw stack dump) beat reading disassembly.
- *How* traffic is blocked matters: DROP and REJECT behave very differently here.
- The vendor's boot hook, the search path and the respawn loop make a no-flash install possible.

## References

- tuya-local and its `v330l_petfeeder` config: <https://github.com/make-all/tuya-local>
- go2rtc: <https://github.com/AlexxIT/go2rtc>
- ingenic-stream-hack: <https://github.com/Drafteed/ingenic-stream-hack>
- tinytuya (local key, UDP discovery): <https://github.com/jasonacox/tinytuya>
- Bootlin toolchains: <https://toolchains.bootlin.com/>
